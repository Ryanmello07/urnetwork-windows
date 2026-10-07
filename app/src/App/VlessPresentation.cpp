// SPDX-License-Identifier: MPL-2.0
//
// No pch.h on purpose -- see VlessPresentation.h. App.vcxproj compiles this
// with PrecompiledHeader=NotUsing, like ExtenderPresentation.cpp.
#include "VlessPresentation.h"

#include <cstddef>

namespace urnw::vless {
namespace {

constexpr bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

constexpr bool IsDigit(char c) { return '0' <= c && c <= '9'; }

std::string_view Trim(std::string_view value) {
  while (!value.empty() && IsSpace(value.front())) value.remove_prefix(1);
  while (!value.empty() && IsSpace(value.back())) value.remove_suffix(1);
  return value;
}

// ASCII only, as the sdk's strings.ToLower is for every token it offers.
std::string LowerAscii(std::string_view value) {
  std::string lower(value);
  for (char& c : lower) {
    if ('A' <= c && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return lower;
}

// "65535" is five digits; a sixth is never a port.
constexpr std::size_t kPortDigits = 5;

}  // namespace

const char* ErrorKey(std::string_view errorId) {
  for (const char* id : kErrorIds) {
    if (errorId == id) return id;
  }
  return "something_went_wrong";
}

namespace detail {

std::string Token(std::optional<std::string> const& value, std::string_view fallback) {
  const std::string token = LowerAscii(Trim(value.value_or(std::string())));
  if (token.empty()) return std::string(fallback);
  return token;
}

}  // namespace detail

// ---- the form ---------------------------------------------------------------

Form NewForm() {
  // sdk vless_settings_ui.go NewVlessSettings
  Form form;
  form.port = PortText(443);
  form.network = kNetworkTcp;
  form.security = kSecurityReality;
  form.flow = kFlowVision;
  form.fingerprint = "chrome";
  return form;
}

FieldVisibility VisibilityFor(std::string_view network, std::string_view security) {
  const std::string transport = detail::Token(std::string(network), kNetworkTcp);
  const std::string outer = detail::Token(std::string(security), kSecurityNone);
  const bool secured = outer != kSecurityNone;
  FieldVisibility shown;
  shown.flow = transport == kNetworkTcp && secured;
  shown.serverNameAndFingerprint = secured;
  shown.alpnAndAllowInsecure = outer == kSecurityTls;
  shown.publicKeyAndShortId = outer == kSecurityReality;
  shown.pathAndHost = transport == kNetworkWs || transport == kNetworkHttpUpgrade;
  return shown;
}

std::string PortText(std::int64_t port) {
  if (port <= 0) return {};
  return std::to_string(port);
}

std::optional<std::int64_t> ParsePort(std::string_view text) {
  const std::string_view digits = Trim(text);
  if (digits.empty()) return 0;
  if (kPortDigits < digits.size()) return std::nullopt;
  std::int64_t port = 0;
  for (char c : digits) {
    if (!IsDigit(c)) return std::nullopt;
    port = port * 10 + (c - '0');
  }
  return port;
}

bool IsPortInput(std::string_view text) {
  if (kPortDigits < text.size()) return false;
  for (char c : text) {
    if (!IsDigit(c)) return false;
  }
  return true;
}

std::string LinkToParse(std::optional<std::string> const& clipboard, std::string const& typed) {
  if (clipboard) {
    // trimmed for the box, which shows one line; the sdk trims a link anyway
    const std::string_view pasted = Trim(*clipboard);
    if (!pasted.empty()) return std::string(pasted);
  }
  return typed;
}

// ---- the pickers --------------------------------------------------------------

std::vector<Option> NetworkOptions() {
  return {
      {kNetworkTcp, "vless_network_tcp"},
      {kNetworkWs, "vless_network_ws"},
      {kNetworkHttpUpgrade, "vless_network_httpupgrade"},
  };
}

std::vector<Option> SecurityOptions() {
  return {
      {kSecurityNone, "none"},
      {kSecurityTls, "vless_security_tls"},
      {kSecurityReality, "vless_security_reality"},
  };
}

std::vector<Option> FlowOptions() {
  return {
      {kFlowNone, "none"},
      {kFlowVision, "vless_flow_vision"},
  };
}

std::vector<Option> FingerprintOptions() {
  // sdk VlessFingerprints: the empty fingerprint is the Go tls client for tls
  // and chrome for REALITY; the rest are uTLS hellos, shown by their own names
  std::vector<Option> options = {{"", "none"}};
  for (const char* fingerprint : {"chrome", "firefox", "safari", "ios", "android", "edge",
                                  "360", "qq", "random", "randomized"}) {
    options.push_back({fingerprint, ""});
  }
  return options;
}

std::vector<Option> OptionsWith(std::vector<Option> options, std::string const& current) {
  if (IndexOf(options, current) < 0) options.push_back({current, ""});
  return options;
}

int IndexOf(std::vector<Option> const& options, std::string_view value) {
  for (std::size_t i = 0; i < options.size(); ++i) {
    if (options[i].value == value) return static_cast<int>(i);
  }
  return -1;
}

std::string ValueAt(std::vector<Option> const& options, int index) {
  if (index < 0 || options.size() <= static_cast<std::size_t>(index)) return {};
  return options[static_cast<std::size_t>(index)].value;
}

}  // namespace urnw::vless
