// The connection region (code name: ingress country): where the user says they
// connect from, which picks the resolvers the app looks URnetwork's own names
// (api, connect, extender) up through. It is a value of the active network
// space (sdk control_doh_ui.go NetworkSpace.Get/SetIngressCountryCode), local
// only, never sent to the server, and applies in place from the next lookup.
// It is separate from the tunnel's (egress) DNS settings: changing one never
// changes the other.
//
// Three doors read and write the same stored value through SdkHost: the login
// screen's optional "Connecting from" control, the "Connection region" picker
// at the top of the DNS editor, and the "Can't reach URnetwork" prompt that a
// network-level failure to reach the platform raises. None of them ever picks
// a region by itself.
//
// Everything here is pure and runs off-Windows
// (tools/connection-region-tests.cpp): the picker's rows, the stored value a
// selection writes, the resolver summary and the can't-reach classification.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace urnw::connection_region {

// store keys (localizations keys/, generated into Strings/*/Resources.resw)
inline constexpr std::string_view kTitleKey = "connection_region";
inline constexpr std::string_view kDescriptionKey = "connection_region_description";
inline constexpr std::string_view kConnectingFromKey = "connecting_from";
inline constexpr std::string_view kUndisclosedKey = "connection_region_undisclosed";
inline constexpr std::string_view kCantReachKey = "cant_reach_urnetwork_choose_region";
inline constexpr std::string_view kPickerTitleKey = "where_are_you_connecting_from";
inline constexpr std::string_view kEncryptedResolversKey = "connection_region_encrypted_resolvers";
inline constexpr std::string_view kDefaultResolversKey = "connection_region_default_resolvers";
inline constexpr std::string_view kFallbackResolversKey = "connection_region_fallback_resolvers";
inline constexpr std::string_view kTrafficDnsKey = "traffic_dns";

// "Other / Undisclosed" is stored as no region.
inline constexpr std::string_view kUndisclosed = "";

inline std::string Lower(std::string_view code) {
  std::string out(code);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// The value a selection stores: the row's code, lower case; Undisclosed is "".
inline std::string StoredValueForSelection(std::string_view code) { return Lower(code); }

struct Row {
  std::string code;   // "" for Other / Undisclosed
  std::wstring label;
};

// The picker's rows: Other / Undisclosed first, then exactly the regions the
// sdk has a recommendation for (sdk IngressCountryCodes), by display name.
// `nameOf` is the platform's localized country name for a code (the app passes
// Windows.Globalization.GeographicRegion); an empty name falls back to the
// upper-case code. A repeated or empty sdk code is listed once / not at all.
inline std::vector<Row> PickerRows(const std::vector<std::string>& sdkCodes,
                                   const std::wstring& undisclosedLabel,
                                   const std::function<std::wstring(const std::string&)>& nameOf) {
  std::vector<Row> regions;
  for (const auto& raw : sdkCodes) {
    std::string code = Lower(raw);
    if (code.empty()) continue;
    if (std::any_of(regions.begin(), regions.end(),
                    [&code](const Row& row) { return row.code == code; })) {
      continue;
    }
    std::wstring label = nameOf ? nameOf(code) : std::wstring{};
    if (label.empty()) {
      for (char c : code) label.push_back(static_cast<wchar_t>(std::toupper(static_cast<unsigned char>(c))));
    }
    regions.push_back({std::move(code), std::move(label)});
  }
  std::stable_sort(regions.begin(), regions.end(),
                   [](const Row& a, const Row& b) { return a.label < b.label; });
  std::vector<Row> rows;
  rows.push_back({std::string(kUndisclosed), undisclosedLabel});
  rows.insert(rows.end(), regions.begin(), regions.end());
  return rows;
}

// The row a stored value selects; a value the picker does not list (a region
// whose recommendation a later sdk dropped) shows as Other / Undisclosed, row 0.
inline size_t SelectedRow(const std::vector<Row>& rows, std::string_view stored) {
  const std::string code = Lower(stored);
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].code == code) return i;
  }
  return 0;
}

// What the picker says a selection resolves through: the encrypted (DoH)
// servers tried ahead of the defaults, and the unencrypted last resort for
// URnetwork names only. Neither -> "Default resolvers".
struct ResolverSummary {
  std::vector<std::string> encrypted;
  std::vector<std::string> fallback;
  bool Defaults() const { return encrypted.empty() && fallback.empty(); }
};

// The region store the doors share: SdkHost in the app, a fake in the tests.
// Get answers the stored code ("" undisclosed); Set stores and answers what
// was stored (an unknown code stores ""). SdkHost answers std::optional, empty
// when the call could not run.
template <class Store>
class RegionSelection {
 public:
  explicit RegionSelection(Store& store) : store_(store) {}
  auto Current() const { return store_.GetIngressCountryCode(); }
  auto Select(const Row& row) {
    return store_.SetIngressCountryCode(StoredValueForSelection(row.code));
  }

 private:
  Store& store_;
};

namespace detail {
inline bool Contains(const std::string& haystack, std::string_view needle) {
  return haystack.find(needle) != std::string::npos;
}
}  // namespace detail

// Whether a failed platform call failed to REACH URnetwork: a transport error
// from the sdk's http client (connect net_http.go: "http request failed: ...",
// Go's dial / lookup / timeout / reset errors). An answer from the server -- a
// status error ("401 Unauthorized: ..."), a validation or auth message in a
// result -- is not: URnetwork was reached. Only this raises the can't-reach
// prompt; it never changes the region.
inline bool IsCantReachFailure(std::string_view error) {
  if (error.empty()) return false;
  const std::string text = Lower(error);
  // HttpStatusError renders as "<code> <status>: <body>": the server answered
  if (3 < text.size() && std::isdigit(static_cast<unsigned char>(text[0])) &&
      std::isdigit(static_cast<unsigned char>(text[1])) &&
      std::isdigit(static_cast<unsigned char>(text[2])) && text[3] == ' ') {
    return false;
  }
  static constexpr std::string_view kTransport[] = {
      "http request failed",
      "dial tcp",
      "dial udp",
      "no such host",
      "server misbehaving",
      "i/o timeout",
      "connection refused",
      "connection reset",
      "network is unreachable",
      "host is unreachable",
      "no route to host",
      "context deadline exceeded",
      "tls handshake",
      "client.timeout exceeded",
      "unexpected eof",
  };
  for (std::string_view needle : kTransport) {
    if (detail::Contains(text, needle)) return true;
  }
  return false;
}

// Signed in, the sdk's own diagnosis of a stalled connection says when the
// platform cannot be reached (window stall reason "platform-unreachable"). The
// prompt is raised once as the reason turns to it, not on every stats push.
inline constexpr std::string_view kStallPlatformUnreachable = "platform-unreachable";

inline bool ShouldPromptOnStall(std::string_view previous, std::string_view current) {
  return current == kStallPlatformUnreachable && previous != kStallPlatformUnreachable;
}

}  // namespace urnw::connection_region
