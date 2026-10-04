// The app's one percent-encoder and query parser, for the urls the app builds
// and the urnetwork:// hand-backs it reads (the pay sheet, the wallet-connect
// and sign-in bridges). The ur.io/checkout bridge envelope itself is the SDK's
// (urnet::buildInlineCheckoutBridgeUrl, urnet::parseCheckoutRedirect).
//
// WinRT-free so tools/url-query-tests.cpp runs it on any host.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <map>
#include <string>

namespace urnw {

// Percent-encode everything except RFC 3986 unreserved characters.
inline std::string PercentEncode(std::string const& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0xF]);
    }
  }
  return out;
}

// Form decoding (what URLSearchParams writes): %XX escapes, and '+' is a space.
// A malformed escape is kept as written.
inline std::string PercentDecode(std::string const& s) {
  auto hexv = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      const int hi = hexv(s[i + 1]), lo = hexv(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(s[i] == '+' ? ' ' : s[i]);
  }
  return out;
}

// The decoded key=value pairs of a query string (no leading '?'); a pair with
// no '=' is skipped, and a repeated key keeps its last value.
inline std::map<std::string, std::string> ParseQueryString(std::string const& query) {
  std::map<std::string, std::string> out;
  size_t i = 0;
  while (i < query.size()) {
    const auto amp = query.find('&', i);
    const std::string pair =
        query.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
    const auto eq = pair.find('=');
    if (eq != std::string::npos) out[pair.substr(0, eq)] = PercentDecode(pair.substr(eq + 1));
    if (amp == std::string::npos) break;
    i = amp + 1;
  }
  return out;
}

// The decoded query parameters of a url ("" query when it has no '?').
inline std::map<std::string, std::string> ParseUrlQuery(std::string const& url) {
  const size_t q = url.find('?');
  return ParseQueryString(q == std::string::npos ? std::string() : url.substr(q + 1));
}

}  // namespace urnw
