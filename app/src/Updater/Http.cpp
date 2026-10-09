// SPDX-License-Identifier: MPL-2.0
#include "Http.h"

#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <format>
#include <string_view>
#include <vector>

#include "Version.h"

// winhttp.h names it from the Windows 10 20348 SDK on
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

namespace urnw::updater {
namespace {

class InternetHandle {
 public:
  explicit InternetHandle(HINTERNET handle) : handle_(handle) {}
  ~InternetHandle() {
    if (handle_) ::WinHttpCloseHandle(handle_);
  }
  InternetHandle(const InternetHandle&) = delete;
  InternetHandle& operator=(const InternetHandle&) = delete;
  HINTERNET get() const { return handle_; }

 private:
  HINTERNET handle_;
};

std::wstring WidenAscii(std::string_view text) { return std::wstring(text.begin(), text.end()); }

// The header named by `query` as text, empty when it is absent or not ASCII.
std::string HeaderText(HINTERNET request, DWORD query) {
  DWORD size = 0;
  ::WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                        &size, WINHTTP_NO_HEADER_INDEX);
  if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return {};
  std::wstring value(size / sizeof(wchar_t), L'\0');
  if (!::WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size,
                             WINHTTP_NO_HEADER_INDEX)) {
    return {};
  }
  value.resize(size / sizeof(wchar_t));
  std::string ascii;
  ascii.reserve(value.size());
  for (const wchar_t c : value) {
    if (c < 0x20 || c > 0x7e) return {};
    ascii.push_back(static_cast<char>(c));
  }
  return ascii;
}

std::int64_t DateHeaderUnixSeconds(HINTERNET request) {
  SYSTEMTIME date{};
  DWORD size = sizeof(date);
  if (!::WinHttpQueryHeaders(request, WINHTTP_QUERY_DATE | WINHTTP_QUERY_FLAG_SYSTEMTIME,
                             WINHTTP_HEADER_NAME_BY_INDEX, &date, &size,
                             WINHTTP_NO_HEADER_INDEX)) {
    return 0;
  }
  FILETIME file{};
  if (!::SystemTimeToFileTime(&date, &file)) return 0;
  ULARGE_INTEGER ticks{};
  ticks.LowPart = file.dwLowDateTime;
  ticks.HighPart = file.dwHighDateTime;
  // FILETIME counts 100 ns from 1601-01-01, 11644473600 s before Unix time
  return static_cast<std::int64_t>(ticks.QuadPart / 10000000ULL) - 11644473600LL;
}

// Whether the machine's own trust, without the user's, accepts the server's
// certificate for `host`.
bool MachineTrusts(HINTERNET request, const std::wstring& host, std::string& error) {
  PCCERT_CONTEXT certificate = nullptr;
  DWORD size = sizeof(certificate);
  if (!::WinHttpQueryOption(request, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &certificate, &size) ||
      !certificate) {
    error = std::format("the server's certificate could not be read: {}", ::GetLastError());
    return false;
  }
  CERT_CHAIN_PARA para{};
  para.cbSize = sizeof(para);
  LPSTR serverAuth = const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH);
  para.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
  para.RequestedUsage.Usage.cUsageIdentifier = 1;
  para.RequestedUsage.Usage.rgpszUsageIdentifier = &serverAuth;
  PCCERT_CHAIN_CONTEXT chain = nullptr;
  const BOOL built = ::CertGetCertificateChain(HCCE_LOCAL_MACHINE, certificate, nullptr,
                                               certificate->hCertStore, &para, 0, nullptr, &chain);
  ::CertFreeCertificateContext(certificate);
  if (!built || !chain) {
    error = std::format("the certificate chain could not be built: {}", ::GetLastError());
    return false;
  }
  HTTPSPolicyCallbackData https{};
  https.cbStruct = sizeof(https);
  https.dwAuthType = AUTHTYPE_SERVER;
  https.pwszServerName = const_cast<wchar_t*>(host.c_str());
  CERT_CHAIN_POLICY_PARA policy{};
  policy.cbSize = sizeof(policy);
  policy.pvExtraPolicyPara = &https;
  CERT_CHAIN_POLICY_STATUS status{};
  status.cbSize = sizeof(status);
  const BOOL checked = ::CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &status);
  ::CertFreeCertificateChain(chain);
  if (!checked || status.dwError != 0) {
    error = std::format("the machine's trust does not accept the certificate for this host: 0x{:x}",
                        checked ? status.dwError : ::GetLastError());
    return false;
  }
  return true;
}

}  // namespace

bool HttpUrlHostIsOneOf(const std::wstring& url, const std::string_view* hosts, std::size_t count) {
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!::WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) ||
      parts.nScheme != INTERNET_SCHEME_HTTPS || parts.nPort != INTERNET_DEFAULT_HTTPS_PORT ||
      parts.dwUserNameLength != 0 || parts.dwPasswordLength != 0) {
    return false;
  }
  const std::wstring_view host(parts.lpszHostName, parts.dwHostNameLength);
  for (std::size_t i = 0; i < count; ++i) {
    const std::wstring want = WidenAscii(hosts[i]);
    if (::CompareStringOrdinal(host.data(), static_cast<int>(host.size()), want.c_str(),
                               static_cast<int>(want.size()), TRUE) == CSTR_EQUAL) {
      return true;
    }
  }
  return false;
}

bool HttpGet(const std::wstring& url, const wchar_t* accept, std::uint64_t maxBytes,
             const std::function<bool(const char*, unsigned long)>& sink, HttpResponse& response,
             std::string& error) {
  response = {};
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!::WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) ||
      parts.nScheme != INTERNET_SCHEME_HTTPS || parts.dwHostNameLength == 0) {
    error = "not an https URL";
    return false;
  }
  const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
  if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

  const std::wstring agent = L"URnetwork-Windows-Update/" + WidenAscii(version::kString);
  InternetHandle session(::WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session.get()) {
    error = std::format("WinHttpOpen failed: {}", ::GetLastError());
    return false;
  }
  ::WinHttpSetTimeouts(session.get(), 10000, 10000, 30000, 30000);
  // TLS 1.2 and 1.3; a Windows that knows no 1.3 takes 1.2 alone
  DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
  if (!::WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols,
                          sizeof(protocols))) {
    protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    ::WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
  }
  InternetHandle connection(::WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
  if (!connection.get()) {
    error = std::format("WinHttpConnect failed: {}", ::GetLastError());
    return false;
  }
  InternetHandle request(::WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              WINHTTP_FLAG_SECURE));
  if (!request.get()) {
    error = std::format("WinHttpOpenRequest failed: {}", ::GetLastError());
    return false;
  }
  DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  if (!::WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy))) {
    error = std::format("WinHttpSetOption(redirect policy) failed: {}", ::GetLastError());
    return false;
  }
  if (accept) {
    const std::wstring header = std::wstring(L"Accept: ") + accept;
    ::WinHttpAddRequestHeaders(request.get(), header.c_str(), static_cast<DWORD>(-1),
                               WINHTTP_ADDREQ_FLAG_ADD);
  }
  if (!::WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !::WinHttpReceiveResponse(request.get(), nullptr)) {
    error = std::format("the request failed: {}", ::GetLastError());
    return false;
  }
  if (!MachineTrusts(request.get(), host, error)) return false;

  DWORD status = 0;
  DWORD statusSize = sizeof(status);
  if (!::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                             WINHTTP_NO_HEADER_INDEX)) {
    error = std::format("the response had no status: {}", ::GetLastError());
    return false;
  }
  response.status = status;
  response.serverUnixSeconds = DateHeaderUnixSeconds(request.get());
  response.location = HeaderText(request.get(), WINHTTP_QUERY_LOCATION);
  if (status != 200 || !sink) return true;

  std::uint64_t total = 0;
  std::vector<char> chunk;
  for (;;) {
    DWORD available = 0;
    if (!::WinHttpQueryDataAvailable(request.get(), &available)) {
      error = std::format("WinHttpQueryDataAvailable failed: {}", ::GetLastError());
      return false;
    }
    if (available == 0) return true;  // the body is complete
    chunk.resize(available);
    DWORD read = 0;
    if (!::WinHttpReadData(request.get(), chunk.data(), available, &read)) {
      error = std::format("WinHttpReadData failed: {}", ::GetLastError());
      return false;
    }
    total += read;
    if (total > maxBytes) {
      error = std::format("the response is larger than the {} byte cap", maxBytes);
      return false;
    }
    if (read && !sink(chunk.data(), read)) {
      error = "the body could not be written";
      return false;
    }
  }
}

}  // namespace urnw::updater
