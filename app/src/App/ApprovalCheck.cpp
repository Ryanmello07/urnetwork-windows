// SPDX-License-Identifier: MPL-2.0
// Built without the precompiled header (App.vcxproj): plain Win32, no WinRT.
#include "ApprovalCheck.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include "ApprovalDescriptor.h"
#include "Config.h"
#include "Log.h"
#include "Strings.h"
#include "Version.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "winhttp.lib")

namespace urnw {
namespace {

// The one answer of this run. Written by the worker thread, read from the UI thread.
std::atomic<int> g_state{static_cast<int>(ApprovalPage::NotChecked)};
// The contract versions the page listed (ApprovalDescriptor.h, Listing): written before g_state says Live, so a reader that
// sees Live sees them.
std::atomic<std::uint32_t> g_contracts{0};

// A WinHTTP handle, closed when it goes.
struct Handle {
  HINTERNET h = nullptr;
  explicit Handle(HINTERNET handle) : h(handle) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  ~Handle() {
    if (h) ::WinHttpCloseHandle(h);
  }
};

// "offered", or the closed word for why the page is not offered. Nothing about the
// response is in the word, let alone the response. `listed` is the versions the page
// lists (bit n for version n) when it is offered, and the caller reads it only then.
std::string_view Probe(std::uint32_t& listed) {
  const auto started = std::chrono::steady_clock::now();
  const auto tooSlow = [&started] { return std::chrono::steady_clock::now() - started > approval::kCheckBudget; };
  const DWORD budgetMs = static_cast<DWORD>(approval::kCheckBudget.count());

  const std::wstring url = Widen(config::kApprovalDescriptorUrl);
  wchar_t host[256] = {};
  wchar_t path[1024] = {};
  wchar_t extra[1024] = {};
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  parts.lpszHostName = host;
  parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
  parts.lpszExtraInfo = extra;
  parts.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
  if (!::WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) return "bad-url";
  const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
  if (!secure && parts.nScheme != INTERNET_SCHEME_HTTP) return "bad-url";

  const std::wstring userAgent = L"URnetwork-Windows/" + Widen(version::kString);
  Handle session(::WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session.h) return "no-connection";
  ::WinHttpSetTimeouts(session.h, static_cast<int>(budgetMs), static_cast<int>(budgetMs), static_cast<int>(budgetMs),
                       static_cast<int>(budgetMs));
  Handle connection(::WinHttpConnect(session.h, host, parts.nPort, 0));
  if (!connection.h) return "no-connection";
  const std::wstring target = std::wstring(path) + extra;
  Handle request(::WinHttpOpenRequest(connection.h, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
  if (!request.h) return "no-connection";

  // A redirect is a failure, not a hop: the page is where the constant says, or it is not live.
  DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  if (!::WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy))) return "no-connection";
  DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
  ::WinHttpSetOption(request.h, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled));
  ::WinHttpAddRequestHeaders(request.h, L"Accept: application/json", static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);

  if (!::WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !::WinHttpReceiveResponse(request.h, nullptr)) {
    return tooSlow() ? "too-slow" : "no-connection";
  }
  if (tooSlow()) return "too-slow";

  DWORD status = 0;
  DWORD statusSize = sizeof(status);
  if (!::WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) ||
      status != 200) {
    return "bad-status";
  }

  wchar_t contentType[256] = {};
  DWORD contentTypeBytes = sizeof(contentType) - sizeof(wchar_t);
  if (!::WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, contentType,
                             &contentTypeBytes, WINHTTP_NO_HEADER_INDEX)) {
    contentType[0] = L'\0';
  }

  // the body, at most one byte past what the descriptor may be (enough to tell it is too large)
  std::string body;
  for (;;) {
    if (tooSlow()) return "too-slow";
    DWORD available = 0;
    if (!::WinHttpQueryDataAvailable(request.h, &available)) return "unreadable";
    if (available == 0) break;
    char chunk[1024];
    DWORD read = 0;
    if (!::WinHttpReadData(request.h, chunk, available < sizeof(chunk) ? available : static_cast<DWORD>(sizeof(chunk)),
                           &read) ||
        read == 0) {
      return "unreadable";
    }
    body.append(chunk, read);
    if (body.size() > approval::kMaxDescriptorBytes) break;
  }
  if (tooSlow()) return "too-slow";
  const approval::Listing answer = approval::ReadDescriptor(Narrow(contentType), body);
  listed = answer.contracts;
  return approval::ToString(answer.verdict);
}

void Run() {
  std::uint32_t listed = 0;
  const std::string_view word = Probe(listed);
  const bool live = word == approval::ToString(approval::DescriptorVerdict::Offered);
  // success and failure alike are kept for the run: nothing asks again
  g_contracts.store(live ? listed : 0U, std::memory_order_release);
  g_state.store(static_cast<int>(live ? ApprovalPage::Live : ApprovalPage::Down), std::memory_order_release);
  LogInfo("approval: the page check: {}", word);
}

}  // namespace

bool ProcessIsElevated() {
  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return true;
  TOKEN_ELEVATION elevation{};
  DWORD size = 0;
  const bool read = ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
  ::CloseHandle(token);
  return !read || elevation.TokenIsElevated != 0;
}

void StartApprovalPageCheck() {
  int expected = static_cast<int>(ApprovalPage::NotChecked);
  if (!g_state.compare_exchange_strong(expected, static_cast<int>(ApprovalPage::Checking))) return;
  try {
    std::thread(Run).detach();
  } catch (const std::system_error&) {
    g_state.store(static_cast<int>(ApprovalPage::Down), std::memory_order_release);
    LogWarn("approval: the page check could not start");
  }
}

ApprovalPage ApprovalPageState() {
  return static_cast<ApprovalPage>(g_state.load(std::memory_order_acquire));
}

bool ApprovalPageLists(std::int64_t contract) {
  if (ApprovalPageState() != ApprovalPage::Live) return false;
  approval::Listing answer;
  answer.contracts = g_contracts.load(std::memory_order_acquire);
  return answer.Lists(contract);
}

}  // namespace urnw
