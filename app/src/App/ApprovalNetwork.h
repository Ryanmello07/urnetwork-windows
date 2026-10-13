// Which network a browser approval is for, and how a request names it.
//
// The page that approves a sign-in mints a one-use code on one API server, and the code is good on that server only.
// A request for the official network is version 1 and names nothing: the page's own default server is the official one.
// A person who points the app at another network (Change Network API: a beta or a self-hosted deployment) has no account
// on the official one, so the request for that network is version 2 and carries `api`, the host of the API this app
// talks to. The page compares it with the host its browser is set to and, when they differ, asks before it switches.
// The contract is docs/app-approval-contract.md, section 11.
//
// `api` is read off the API url the client really uses (the SDK's getApiUrl), never off the host name that was typed,
// and only a url that is https://host[:port] names a network (a URL does not tell capitals from small letters in its scheme
// or its host, nor a port of 443 from none, so those are read the way a URL is read): an http api, a path, a query, a user,
// a non-ASCII host and an IP literal are not offered, because the app could not say to the page which server it is, or the
// page could not hold the request to that server.
//
// Standard C++ and nothing else, so tools/approval-network-tests.cpp runs it on any host.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace urnw::approval {

// The versions of the contract this app speaks: 1 is a request for the official network, 2 names another one.
inline constexpr std::int64_t kContract = 1;
inline constexpr std::int64_t kContractNamed = 2;

// The grammar of `api` (contract section 11): at most this many characters in all, labels of at most this many, and
// a port that is neither https' own nor out of range.
inline constexpr std::size_t kMaxApiChars = 200;
inline constexpr std::size_t kMaxApiLabelChars = 63;
inline constexpr unsigned kMaxApiPort = 65535;
inline constexpr unsigned kHttpsPort = 443;

namespace detail {

constexpr bool IsLowerOrDigit(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

constexpr bool IsAllDigits(std::string_view text) {
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
  }
  return !text.empty();
}

// 0x and any number of hex digits (small letters: nothing else gets this far): what a browser reads as a hexadecimal number.
constexpr bool IsHexNumber(std::string_view text) {
  if (text.size() < 2 || text[0] != '0' || text[1] != 'x') return false;
  for (const char c : text.substr(2)) {
    if ((c < '0' || c > '9') && (c < 'a' || c > 'f')) return false;
  }
  return true;
}

// One label of a host name: 1 to 63 of a-z 0-9 and the hyphen, and a letter or a digit at both ends.
constexpr bool IsApiLabel(std::string_view label) {
  if (label.empty() || label.size() > kMaxApiLabelChars) return false;
  if (!IsLowerOrDigit(label.front()) || !IsLowerOrDigit(label.back())) return false;
  for (const char c : label) {
    if (!IsLowerOrDigit(c) && c != '-') return false;
  }
  return true;
}

// 1 to 65535 without a leading zero (so not 0 either), and never 443: https says it without being told.
constexpr bool IsApiPort(std::string_view port) {
  if (!IsAllDigits(port) || port.size() > 5 || port.front() == '0') return false;
  unsigned value = 0;
  for (const char c : port) value = value * 10 + static_cast<unsigned>(c - '0');
  return value <= kMaxApiPort && value != kHttpsPort;
}

}  // namespace detail

// Whether `host` is a value the contract allows for `api`: `host` or `host:port`, https implied.
//   ASCII lower-case letters, digits, hyphens and dots; never a scheme, a path, a user or a query;
//   at least two labels, each of 1 to 63 characters that starts and ends with a letter or a digit;
//   the last label is neither all digits nor 0x and hex digits (a browser reads either as a number, so a name that ends in
//   one is an IP address or no name at all: no IP literal in any spelling); a trailing dot is an empty label;
//   a port of 1 to 65535 without a leading zero, never 443; at most 200 characters in all.
constexpr bool IsValidApiHost(std::string_view host) {
  if (host.empty() || host.size() > kMaxApiChars) return false;
  std::string_view name = host;
  const std::size_t colon = host.find(':');
  if (colon != std::string_view::npos) {
    if (!detail::IsApiPort(host.substr(colon + 1))) return false;
    name = host.substr(0, colon);
  }
  std::size_t labels = 0;
  std::string_view last;
  for (std::size_t start = 0;;) {
    const std::size_t dot = name.find('.', start);
    last = name.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
    if (!detail::IsApiLabel(last)) return false;
    ++labels;
    if (dot == std::string_view::npos) break;
    start = dot + 1;
  }
  return labels >= 2 && !detail::IsAllDigits(last) && !detail::IsHexNumber(last);
}

// The `api` value an API url names: the host, and the port if it has one, of a url that is https://host[:port] (one trailing
// slash is allowed) with a host IsValidApiHost accepts. The url is read the way a URL is read, because that is how the client
// and the page read it: the scheme and the host are the same in capitals, and a port of 443 is https' own, which a URL
// spells without it. So the value is the host the client dials, in small letters and without ":443". Anything else - another
// scheme, a path, a query, a fragment, a user, a host outside ASCII, an IP literal - is nothing, and the result is empty.
inline std::string ApiHostOfUrl(std::string_view apiUrl) {
  constexpr std::string_view kScheme = "https://";
  constexpr std::string_view kOwnPort = ":443";
  std::string url(apiUrl);
  for (char& c : url) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  std::string_view host = url;
  if (host.substr(0, kScheme.size()) != kScheme) return {};
  host.remove_prefix(kScheme.size());
  if (!host.empty() && host.back() == '/') host.remove_suffix(1);
  if (host.size() > kOwnPort.size() && host.substr(host.size() - kOwnPort.size()) == kOwnPort) {
    // only when it is the port: after another port it is a second one, and the host stays as it is, which is no host
    const std::string_view name = host.substr(0, host.size() - kOwnPort.size());
    if (name.find(':') == std::string_view::npos) host = name;
  }
  return IsValidApiHost(host) ? std::string(host) : std::string();
}

enum class NetworkKind {
  None,      // no network a request can name: the space could not be read, or its API url cannot be named
  Official,  // the operator's own space, which the page's default server serves: version 1
  Custom,    // any other network whose API url names a host: version 2
};

// The network an approval would be for. `api` is the host to name, for a Custom network only.
struct Network {
  NetworkKind kind = NetworkKind::None;
  std::string api;
  friend bool operator==(const Network&, const Network&) = default;
};

// The network of a space the SDK host has read. `officialSpace` is the host's own judgment of the operator's space (its
// host, and no api or connect url of the person's own); `liveApiUrl` is the API url the client really uses; `operatorHost`
// is the operator's host name. The official network is the one the page's default server serves, and that is
// api.<operatorHost>: a space that has the operator's host but talks to another API (a test environment of the operator, whose
// name the SDK puts in front of the service label) is not that network, and a request for the official one would have the
// page mint the code on one server and the app redeem it on another. So it is named like any other space, by its API. The
// official network never names itself. Any other space is Custom only if its url names a host.
inline Network NetworkOf(bool officialSpace, std::string_view liveApiUrl, std::string_view operatorHost) {
  std::string api = ApiHostOfUrl(liveApiUrl);
  if (api.empty()) return Network{};
  if (officialSpace && api == "api." + std::string(operatorHost)) return Network{NetworkKind::Official, std::string()};
  return Network{NetworkKind::Custom, std::move(api)};
}

// The version of the contract a request for a network is, and so the one the page has to list for the action to be
// offered there: 0 for no network.
constexpr std::int64_t ContractFor(NetworkKind kind) {
  switch (kind) {
    case NetworkKind::Official: return kContract;
    case NetworkKind::Custom: return kContractNamed;
    case NetworkKind::None: return 0;
  }
  return 0;
}

}  // namespace urnw::approval
