// The diagnostic lines in the logs that feedback uploads (the Windows half of
// the app-log path; open bugs P021 and P052).
//
// "Send feedback with logs" uploads the service's glog files, and only them:
// the app's DeviceRemote asks the service's DeviceLocal to upload
// (DeviceLocalRpc.UploadLogs), which zips its own process's log directory.
// The app's logs and the service's own log stay on the machine. So a fact
// support needs from an uploaded log is written by the service, through the
// sdk (urnet::logAppInfo: one "[app][<tag>] <message>" line per call, which
// the sdk bounds and sanitizes again), when it becomes true
// (Service/ServiceDiagnostics.h):
//
//   [app][service]          the state the service pushes to the app: the
//                           session, the firewall (WFP) policy in force, the
//                           kill switch preference, the failsafe, and the
//                           provider-only device
//   [app][adapter]          whether the tunnel's routes are on the tun, and the
//                           kind of physical interface its traffic leaves by
//   [app][dns]              the Windows DNS client settings that decide where a
//                           tunnel's queries can go: the DNS-over-HTTPS policy
//                           and the Name Resolution Policy Table
//   [app][proxy]            the user's system proxy, as the app read it when it
//                           asked for the tunnel
//   [app][network-country]  the network country and its source
//                           (Common/NetworkCountry.h)
//
// Every value is a token from a closed set or a small number. No address, host
// name, interface or adapter name, URL, path, GUID or identifier, and no text
// from the OS or from a peer, reaches a line: each fact is reduced to its kind
// before it is written, and anything that is not a known kind is written as
// "other" or "unknown". So nothing personal and nothing secret is in them,
// and every line is short and has a fixed shape.
//
// Pure and portable: tools/diagnostic-lines-tests.cpp runs it on any host.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "NetworkCountry.h"

namespace urnw::diag {

// The tags, as the sdk keeps them: letters, digits and '-'.
inline constexpr std::string_view kTagService = "service";
inline constexpr std::string_view kTagAdapter = "adapter";
inline constexpr std::string_view kTagDns = "dns";
inline constexpr std::string_view kTagProxy = "proxy";
inline constexpr std::string_view kTagNetworkCountry = "network-country";

// `value` when it is one of `known`, else `fallback`: the one way a string
// becomes part of a line.
constexpr std::string_view OneOf(std::string_view value,
                                 std::initializer_list<std::string_view> known,
                                 std::string_view fallback) {
  for (const std::string_view k : known) {
    if (value == k) return k;
  }
  return fallback;
}

// A flag as a line writes it.
constexpr std::string_view YesNo(bool value) { return value ? "yes" : "no"; }

// ---- [app][service] -------------------------------------------------------------

// What one status the service pushes says, by the protocol's own spellings
// (Protocol.h ToString, TunnelStatus::wfp_state and stop_reason).
struct ServiceFacts {
  std::string_view state;
  std::string_view mode;
  bool routes = false;
  bool dns = false;
  std::string_view firewall;
  // the kill switch preference the app last sent (TunnelController), which
  // the firewall state alone does not tell: on with the firewall off is a
  // guarantee the machine does not have
  bool killSwitch = false;
  bool failsafeArmed = false;
  std::string_view stopReason;
  bool providerRunning = false;
  // what the app asked the provider-only device for, verbatim from the request
  std::string_view providerControlMode;
  // the live tier as the sdk applied it: 0 none, 1 network, 3 public
  int64_t providerTier = 0;
};

// The sdk's provide tier by name; a tier this build does not know is "other".
constexpr std::string_view ProviderTierName(int64_t tier) {
  switch (tier) {
    case 0: return "none";
    case 1: return "network";
    case 3: return "public";
    default: break;
  }
  return "other";
}

// The [app][service] line, every field from its closed set.
inline std::string ServiceLine(const ServiceFacts& f) {
  std::string line;
  line += "state=";
  line += OneOf(f.state, {"stopped", "starting", "up", "stopping", "error", "rpc_only", "preparing"},
                "other");
  line += " mode=";
  line += OneOf(f.mode, {"tunnel", "rpc_only"}, "other");
  line += " routes=";
  line += YesNo(f.routes);
  line += " dns=";
  line += YesNo(f.dns);
  line += " firewall=";
  line += OneOf(f.firewall, {"off", "armed", "connecting", "connected"}, "other");
  line += " kill_switch=";
  line += f.killSwitch ? "on" : "off";
  line += " failsafe=";
  line += f.failsafeArmed ? "armed" : "no";
  line += " stop=";
  line += f.stopReason.empty()
              ? std::string_view("-")
              : OneOf(f.stopReason,
                      {"user", "failsafe_no_exit", "failsafe_no_inbound", "failsafe_sdk_unresponsive"},
                      "other");
  line += " provider=";
  if (f.providerRunning) {
    line += OneOf(f.providerControlMode, {"never", "always", "network", "auto"}, "unknown");
    line += "/";
    line += ProviderTierName(f.providerTier);
  } else {
    line += "off";
  }
  return line;
}

// ---- [app][adapter] -------------------------------------------------------------

// What kind of link an interface is, and nothing else about it.
enum class InterfaceKind { None, Wifi, Ethernet, MobileBroadband, Other };

// IF_TYPE_ETHERNET_CSMACD and IF_TYPE_IEEE80211 (ipifcons.h), and
// NdisPhysicalMediumWirelessLan and NdisPhysicalMediumNative802_11
// (ntddndis.h), restated like NetworkCountry.h's; Service/ServiceDiagnostics.cpp
// checks them against the SDK's.
inline constexpr uint32_t kIfTypeEthernet = 6;
inline constexpr uint32_t kIfTypeWifi = 71;
inline constexpr uint32_t kPhysicalMediumWirelessLan = 1;
inline constexpr uint32_t kPhysicalMediumNative80211 = 9;

// The kind, by the interface's type and its physical medium.
constexpr InterfaceKind InterfaceKindFor(uint32_t ifType, uint32_t physicalMedium) {
  if (netcountry::IsMobileBroadbandInterface(ifType, physicalMedium))
    return InterfaceKind::MobileBroadband;
  if (ifType == kIfTypeWifi || physicalMedium == kPhysicalMediumNative80211 ||
      physicalMedium == kPhysicalMediumWirelessLan)
    return InterfaceKind::Wifi;
  if (ifType == kIfTypeEthernet) return InterfaceKind::Ethernet;
  return InterfaceKind::Other;
}

// One egress interface as the [app][adapter] line describes it.
struct InterfaceFacts {
  InterfaceKind kind = InterfaceKind::None;
  // the media is connected; an unreadable state counts as connected
  bool connected = true;
};

// The kind as a line writes it, with "-down" for a link that is down.
inline std::string InterfaceName(const InterfaceFacts& f) {
  std::string name;
  switch (f.kind) {
    case InterfaceKind::None: return "none";
    case InterfaceKind::Wifi: name = "wifi"; break;
    case InterfaceKind::Ethernet: name = "ethernet"; break;
    case InterfaceKind::MobileBroadband: name = "mobile-broadband"; break;
    case InterfaceKind::Other: name = "other"; break;
  }
  if (!f.connected) name += "-down";
  return name;
}

// `tunnel`: the tun carries the capture routes (TunnelStatus::routes_installed).
// The egress is the physical interface the service's sdk is pinned to for each
// family (TunnelStatus::egress_index4/6); none while nothing is pinned, which
// is every state without a tunnel session.
inline std::string AdapterLine(bool tunnel, const InterfaceFacts& egress4,
                               const InterfaceFacts& egress6) {
  return std::string("tunnel=") + (tunnel ? "up" : "none") + " egress_v4=" +
         InterfaceName(egress4) + " egress_v6=" + InterfaceName(egress6);
}

// ---- [app][dns] -----------------------------------------------------------------

// The Group Policy "Configure DNS over HTTPS (DoH) name resolution"
// (DoHPolicy: 1 prohibit, 2 allow, 3 require). Require is the one that breaks a
// tunnel: the DNS client then sends nothing to a resolver it has no DoH
// template for, and the tun's resolvers are plain DNS.
enum class DohPolicy { Unset, Prohibit, Allow, Require, Other };

// The policy for the DoHPolicy value, none when it is not set.
constexpr DohPolicy DohPolicyFor(std::optional<uint32_t> value) {
  if (!value) return DohPolicy::Unset;
  switch (*value) {
    case 1: return DohPolicy::Prohibit;
    case 2: return DohPolicy::Allow;
    case 3: return DohPolicy::Require;
    default: break;
  }
  return DohPolicy::Other;
}

// The policy as a line writes it.
constexpr std::string_view DohPolicyName(DohPolicy policy) {
  switch (policy) {
    case DohPolicy::Unset: return "unset";
    case DohPolicy::Prohibit: return "prohibit";
    case DohPolicy::Allow: return "allow";
    case DohPolicy::Require: return "require";
    case DohPolicy::Other: break;
  }
  return "other";
}

// More rules than this are written as this many and a "+".
inline constexpr int64_t kNrptRulesCap = 99;

// `nrptRules`: the Name Resolution Policy Table's rules (Group Policy and
// local), each of which sends a namespace to resolvers of its own, outside the
// tun's; negative when they could not be counted.
inline std::string DnsLine(DohPolicy policy, int64_t nrptRules) {
  std::string line = "doh_policy=" + std::string(DohPolicyName(policy)) + " nrpt_rules=";
  if (nrptRules < 0) {
    line += "unknown";
  } else if (nrptRules > kNrptRulesCap) {
    line += std::to_string(kNrptRulesCap) + "+";
  } else {
    line += std::to_string(nrptRules);
  }
  return line;
}

// ---- [app][proxy] ---------------------------------------------------------------

// The kinds of a user's system proxy (WinINet: what browsers use), in the order
// a kind lists them. A manual proxy is reduced to where its host is: this
// machine, a private network, a public address, or a name that cannot be told
// without resolving it.
inline constexpr std::string_view kProxyNone = "none";
inline constexpr std::string_view kProxyUnknown = "unknown";
inline constexpr std::array<std::string_view, 6> kProxyParts = {
    "auto-detect", "pac", "manual-loopback", "manual-private", "manual-public", "manual-name"};

// Where a proxy's host is, as far as its text says.
enum class HostKind { Loopback, Private, Public, Name };

namespace detail {

// An ASCII letter in lower case; anything else as it is.
constexpr char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// Equal but for the case of ASCII letters.
constexpr bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (Lower(a[i]) != Lower(b[i])) return false;
  }
  return true;
}

// A dotted-quad IPv4 address, or nullopt.
constexpr std::optional<std::array<int, 4>> Ipv4(std::string_view s) {
  std::array<int, 4> octets{};
  int index = 0;
  int value = -1;
  for (const char c : s) {
    if (c == '.') {
      if (value < 0 || index == 3) return std::nullopt;
      octets[index++] = value;
      value = -1;
    } else if (c >= '0' && c <= '9') {
      value = (value < 0 ? 0 : value * 10) + (c - '0');
      if (value > 255) return std::nullopt;
    } else {
      return std::nullopt;
    }
  }
  if (value < 0 || index != 3) return std::nullopt;
  octets[3] = value;
  return octets;
}

// Hex digits, ':' and '.', with at least one ':'.
constexpr bool IsIpv6Literal(std::string_view s) {
  if (s.find(':') == std::string_view::npos) return false;
  for (const char c : s) {
    const char l = Lower(c);
    if (!((l >= '0' && l <= '9') || (l >= 'a' && l <= 'f') || l == ':' || l == '.')) return false;
  }
  return true;
}

}  // namespace detail

// This machine, a private or link-local range, any other address, or a name
// that cannot be told without resolving it, which is never done here.
constexpr HostKind HostKindFor(std::string_view host) {
  using detail::EqualsIgnoreCase;
  if (EqualsIgnoreCase(host, "localhost") ||
      (host.size() > 10 && EqualsIgnoreCase(host.substr(host.size() - 10), ".localhost")))
    return HostKind::Loopback;
  if (const auto v4 = detail::Ipv4(host)) {
    const auto& o = *v4;
    if (o[0] == 127) return HostKind::Loopback;
    if (o[0] == 10 || (o[0] == 172 && o[1] >= 16 && o[1] <= 31) || (o[0] == 192 && o[1] == 168) ||
        (o[0] == 169 && o[1] == 254) || (o[0] == 100 && o[1] >= 64 && o[1] <= 127))
      return HostKind::Private;
    return HostKind::Public;
  }
  if (detail::IsIpv6Literal(host)) {
    if (host == "::1" || host == "0:0:0:0:0:0:0:1") return HostKind::Loopback;
    const char a = detail::Lower(host[0]);
    const char b = host.size() > 1 ? detail::Lower(host[1]) : '\0';
    const char c = host.size() > 2 ? detail::Lower(host[2]) : '\0';
    if (a == 'f' && (b == 'c' || b == 'd')) return HostKind::Private;  // fc00::/7
    if (a == 'f' && b == 'e' && (c == '8' || c == '9' || c == 'a' || c == 'b'))
      return HostKind::Private;  // fe80::/10
    return HostKind::Public;
  }
  return HostKind::Name;
}

// The host of one WinINet proxy entry ("host:port", "scheme=host:port",
// "http://host:port", "[v6]:port"), or "" for none. Credentials and a path,
// which WinINet does not take but a hand-edited setting can carry, are cut
// away with the rest.
constexpr std::string_view ProxyEntryHost(std::string_view entry) {
  if (const auto eq = entry.find('='); eq != std::string_view::npos) entry = entry.substr(eq + 1);
  if (const auto scheme = entry.find("://"); scheme != std::string_view::npos)
    entry = entry.substr(scheme + 3);
  if (const auto slash = entry.find('/'); slash != std::string_view::npos)
    entry = entry.substr(0, slash);
  if (const auto at = entry.rfind('@'); at != std::string_view::npos) entry = entry.substr(at + 1);
  if (!entry.empty() && entry.front() == '[') {
    const auto close = entry.find(']');
    return close == std::string_view::npos ? std::string_view() : entry.substr(1, close - 1);
  }
  const auto firstColon = entry.find(':');
  if (firstColon != std::string_view::npos && entry.find(':', firstColon + 1) == std::string_view::npos)
    return entry.substr(0, firstColon);  // host:port
  return entry;  // a bare host, or a bare IPv6 literal
}

// The kind of the user's proxy configuration (WinHttpGetIEProxyConfigForCurrentUser):
// auto-detect, a PAC script, and the manual proxies by where their hosts are.
// The URL, the hosts, the ports and the bypass list are never part of it.
inline std::string UserProxyKind(bool autoDetect, bool pacScript, std::string_view proxyList) {
  std::array<bool, kProxyParts.size()> present{};
  present[0] = autoDetect;
  present[1] = pacScript;
  std::size_t at = 0;
  while (at < proxyList.size()) {
    std::size_t end = proxyList.find_first_of("; \t\r\n", at);
    if (end == std::string_view::npos) end = proxyList.size();
    const std::string_view host = ProxyEntryHost(proxyList.substr(at, end - at));
    if (!host.empty()) {
      switch (HostKindFor(host)) {
        case HostKind::Loopback: present[2] = true; break;
        case HostKind::Private: present[3] = true; break;
        case HostKind::Public: present[4] = true; break;
        case HostKind::Name: present[5] = true; break;
      }
    }
    at = end + 1;
  }
  std::string kind;
  for (std::size_t i = 0; i < kProxyParts.size(); ++i) {
    if (!present[i]) continue;
    if (!kind.empty()) kind += "+";
    kind += kProxyParts[i];
  }
  return kind.empty() ? std::string(kProxyNone) : kind;
}

// What the service takes off the pipe (StartTunnel::system_proxy): a kind
// UserProxyKind can produce, exactly, or "unknown" -- also for an app too old
// to send one.
inline std::string ProxyKindOrUnknown(std::string_view kind) {
  if (kind == kProxyNone) return std::string(kProxyNone);
  std::size_t next = 0;  // parts must come in kProxyParts order, once each
  std::size_t at = 0;
  bool any = false;
  while (at <= kind.size()) {
    std::size_t end = kind.find('+', at);
    if (end == std::string_view::npos) end = kind.size();
    const std::string_view part = kind.substr(at, end - at);
    bool matched = false;
    while (next < kProxyParts.size()) {
      if (kProxyParts[next++] == part) {
        matched = true;
        break;
      }
    }
    if (!matched) return std::string(kProxyUnknown);
    any = true;
    at = end + 1;
  }
  return any ? std::string(kind) : std::string(kProxyUnknown);
}

// The [app][proxy] line.
inline std::string ProxyLine(std::string_view kind) { return "user=" + ProxyKindOrUnknown(kind); }

// ---- [app][network-country] -----------------------------------------------------

// The [app][network-country] line, normalized as the service takes it.
inline std::string NetworkCountryLine(std::string_view code, std::string_view source) {
  const netcountry::Reading reading = netcountry::Normalized(code, source);
  return "country=" + (reading.code.empty() ? std::string("none") : reading.code) +
         " source=" + reading.source;
}

}  // namespace urnw::diag
