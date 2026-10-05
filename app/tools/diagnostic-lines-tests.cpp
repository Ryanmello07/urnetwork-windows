// Executable spec for the service's diagnostic lines (Common/DiagnosticLines.h):
// the [app][service], [app][adapter], [app][dns], [app][proxy] and
// [app][network-country] lines that "send feedback with logs" uploads. Every
// value in them is a token from a closed set or a small number, so the checks
// feed each formatter what a peer, the OS or a hand-edited setting could put
// in front of it -- line breaks, forged prefixes, paths, addresses, host
// names -- and require that none of it comes out, and that every line stays
// short.
//
//   c++ -std=c++20 -I ../src/Common diagnostic-lines-tests.cpp -o /tmp/diagnostic-lines-tests && /tmp/diagnostic-lines-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>
#include <string_view>

#include "DiagnosticLines.h"

using namespace urnw;

namespace {

int gFailures = 0;
int gCases = 0;

// Counts a case, and prints `what` when it fails.
void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << std::endl;
  }
}

// The line is exactly `want`.
void CheckLine(const std::string& got, std::string_view want, const std::string& what) {
  Check(got == want, what + " (got \"" + got + "\", want \"" + std::string(want) + "\")");
}

// The shape every line keeps: one short line of printable ASCII.
void CheckBounded(const std::string& line, const std::string& what) {
  bool printable = true;
  for (const char c : line) {
    if (c < 0x20 || c > 0x7e) printable = false;
  }
  Check(printable && line.size() <= 160, what + ": one short printable line (" + std::to_string(line.size()) +
                                             " bytes)");
}

// Nothing of `secret` reached the line.
void CheckAbsent(const std::string& line, std::string_view secret, const std::string& what) {
  Check(line.find(secret) == std::string::npos,
        what + ": \"" + std::string(secret) + "\" must not reach the line \"" + line + "\"");
}

// The hostile inputs: a forged second line, a path with a user name in it, an
// address, a host name, and text long enough to matter.
const std::string kHostile[] = {
    "up\n[app][service] forged",
    "C:\\Users\\alice\\proxy.pac",
    "198.51.100.40",
    "proxy.alice-home.example",
    std::string(400, 'x'),
};

// The [app][service] line, and every field from its closed set.
void TestServiceLine() {
  diag::ServiceFacts f;
  f.state = "up";
  f.mode = "tunnel";
  f.routes = true;
  f.dns = true;
  f.firewall = "connected";
  f.killSwitch = true;
  f.stopReason = "";
  CheckLine(diag::ServiceLine(f),
            "state=up mode=tunnel routes=yes dns=yes firewall=connected kill_switch=on failsafe=no "
            "stop=- provider=off",
            "service: a protected tunnel");

  diag::ServiceFacts stopped;
  stopped.state = "stopped";
  stopped.mode = "tunnel";
  stopped.firewall = "armed";
  stopped.killSwitch = true;
  stopped.stopReason = "failsafe_no_exit";
  stopped.providerRunning = true;
  stopped.providerControlMode = "auto";
  stopped.providerTier = 1;
  CheckLine(diag::ServiceLine(stopped),
            "state=stopped mode=tunnel routes=no dns=no firewall=armed kill_switch=on failsafe=no "
            "stop=failsafe_no_exit provider=auto/network",
            "service: a failsafe stop, the armed floor and the provider-only device");

  diag::ServiceFacts unguarded = f;
  unguarded.firewall = "off";
  unguarded.failsafeArmed = true;
  CheckLine(diag::ServiceLine(unguarded),
            "state=up mode=tunnel routes=yes dns=yes firewall=off kill_switch=on failsafe=armed "
            "stop=- provider=off",
            "service: a kill switch the firewall does not hold says both");

  for (const std::string& hostile : kHostile) {
    diag::ServiceFacts h;
    h.state = hostile;
    h.mode = hostile;
    h.firewall = hostile;
    h.stopReason = hostile;
    h.providerRunning = true;
    h.providerControlMode = hostile;
    h.providerTier = 2;
    const std::string line = diag::ServiceLine(h);
    CheckLine(line,
              "state=other mode=other routes=no dns=no firewall=other kill_switch=off failsafe=no "
              "stop=other provider=unknown/other",
              "service: an unknown value of every field is a fixed word");
    CheckBounded(line, "service");
    CheckAbsent(line, hostile.substr(0, 10), "service");
  }
  diag::ServiceFacts verbatim = f;
  verbatim.providerRunning = true;
  verbatim.providerControlMode = "always\n[app][x] y";
  CheckAbsent(diag::ServiceLine(verbatim), "[app][x]",
              "service: the app's provide mode is never written verbatim");
  diag::ServiceFacts unknownFirewall = f;
  unknownFirewall.firewall = "blocking everything";
  Check(diag::ServiceLine(unknownFirewall).find("firewall=other ") != std::string::npos,
        "service: an unknown firewall state is other");
}

// The interface kinds and the [app][adapter] line.
void TestAdapterLine() {
  Check(diag::InterfaceKindFor(71, 9) == diag::InterfaceKind::Wifi, "adapter: Wi-Fi by type");
  Check(diag::InterfaceKindFor(6, 9) == diag::InterfaceKind::Wifi, "adapter: Wi-Fi by medium");
  Check(diag::InterfaceKindFor(6, 14) == diag::InterfaceKind::Ethernet, "adapter: Ethernet");
  Check(diag::InterfaceKindFor(243, 8) == diag::InterfaceKind::MobileBroadband, "adapter: mobile broadband");
  Check(diag::InterfaceKindFor(53, 0) == diag::InterfaceKind::Other, "adapter: a virtual interface is other");
  CheckLine(diag::AdapterLine(true, {.kind = diag::InterfaceKind::Wifi, .connected = true},
                              {.kind = diag::InterfaceKind::None, .connected = true}),
            "tunnel=up egress_v4=wifi egress_v6=none", "adapter: a tunnel over Wi-Fi");
  CheckLine(diag::AdapterLine(false, {.kind = diag::InterfaceKind::Ethernet, .connected = false},
                              {.kind = diag::InterfaceKind::MobileBroadband, .connected = true}),
            "tunnel=none egress_v4=ethernet-down egress_v6=mobile-broadband",
            "adapter: a link that is down says so");
  CheckBounded(diag::AdapterLine(true, {.kind = diag::InterfaceKind::MobileBroadband, .connected = false},
                                 {.kind = diag::InterfaceKind::MobileBroadband, .connected = false}),
               "adapter");
}

// The DoH policy values and the [app][dns] line.
void TestDnsLine() {
  Check(diag::DohPolicyFor(std::nullopt) == diag::DohPolicy::Unset, "dns: no policy value is unset");
  Check(diag::DohPolicyFor(1) == diag::DohPolicy::Prohibit, "dns: 1 prohibits DoH");
  Check(diag::DohPolicyFor(2) == diag::DohPolicy::Allow, "dns: 2 allows DoH");
  Check(diag::DohPolicyFor(3) == diag::DohPolicy::Require, "dns: 3 requires DoH");
  Check(diag::DohPolicyFor(7) == diag::DohPolicy::Other, "dns: anything else is other");
  CheckLine(diag::DnsLine(diag::DohPolicy::Require, 2), "doh_policy=require nrpt_rules=2",
            "dns: a DoH requirement and two NRPT rules");
  CheckLine(diag::DnsLine(diag::DohPolicy::Unset, 0), "doh_policy=unset nrpt_rules=0", "dns: nothing set");
  CheckLine(diag::DnsLine(diag::DohPolicy::Other, -1), "doh_policy=other nrpt_rules=unknown",
            "dns: rules that could not be counted");
  CheckLine(diag::DnsLine(diag::DohPolicy::Allow, 5000), "doh_policy=allow nrpt_rules=99+",
            "dns: more than 99 rules are 99+");
}

// Where a proxy's host is, the user's proxy kind, and what the service takes.
void TestProxy() {
  using diag::HostKind;
  Check(diag::HostKindFor("127.0.0.1") == HostKind::Loopback, "host: 127.0.0.1 is this machine");
  Check(diag::HostKindFor("localhost") == HostKind::Loopback, "host: localhost is this machine");
  Check(diag::HostKindFor("LocalHost") == HostKind::Loopback, "host: in any case");
  Check(diag::HostKindFor("::1") == HostKind::Loopback, "host: ::1 is this machine");
  Check(diag::HostKindFor("10.0.0.5") == HostKind::Private, "host: 10.0.0.5 is private");
  Check(diag::HostKindFor("172.20.1.1") == HostKind::Private, "host: 172.16/12 is private");
  Check(diag::HostKindFor("172.32.1.1") == HostKind::Public, "host: 172.32 is not");
  Check(diag::HostKindFor("192.168.1.10") == HostKind::Private, "host: 192.168/16 is private");
  Check(diag::HostKindFor("100.64.0.1") == HostKind::Private, "host: shared address space is private");
  Check(diag::HostKindFor("fd12:3456::1") == HostKind::Private, "host: a ULA is private");
  Check(diag::HostKindFor("fe80::1") == HostKind::Private, "host: link-local is private");
  Check(diag::HostKindFor("203.0.113.9") == HostKind::Public, "host: an address on the internet is public");
  Check(diag::HostKindFor("2001:db8::1") == HostKind::Public, "host: and in IPv6");
  Check(diag::HostKindFor("proxy.corp.example") == HostKind::Name, "host: a name cannot be told");
  Check(diag::HostKindFor("256.1.1.1") == HostKind::Name, "host: not an address is a name");

  Check(diag::ProxyEntryHost("127.0.0.1:8888") == "127.0.0.1", "entry: host:port");
  Check(diag::ProxyEntryHost("https=10.0.0.5:3128") == "10.0.0.5", "entry: scheme=host:port");
  Check(diag::ProxyEntryHost("http://user:secret@proxy.example:8080/") == "proxy.example",
        "entry: credentials and a path are cut away");
  Check(diag::ProxyEntryHost("socks=[::1]:1080") == "::1", "entry: a bracketed IPv6 host");
  Check(diag::ProxyEntryHost("") == "", "entry: nothing");

  CheckLine(diag::UserProxyKind(false, false, ""), "none", "proxy: no proxy");
  CheckLine(diag::UserProxyKind(true, false, ""), "auto-detect", "proxy: auto-detect");
  CheckLine(diag::UserProxyKind(false, true, ""), "pac", "proxy: a PAC script");
  CheckLine(diag::UserProxyKind(false, false, "127.0.0.1:10809"), "manual-loopback",
            "proxy: a local proxy app");
  CheckLine(diag::UserProxyKind(false, false, "http=10.0.0.5:3128;https=10.0.0.5:3128"), "manual-private",
            "proxy: a LAN proxy, once however many schemes name it");
  CheckLine(diag::UserProxyKind(true, true,
                                "http=127.0.0.1:8080;https=proxy.alice-home.example:443 socks=203.0.113.9:1080"),
            "auto-detect+pac+manual-loopback+manual-public+manual-name", "proxy: everything at once, in order");
  for (const std::string& hostile : kHostile) {
    const std::string kind = diag::UserProxyKind(false, false, hostile);
    CheckBounded(kind, "proxy kind");
    CheckAbsent(kind, "alice", "proxy kind");
    CheckAbsent(kind, "198.51.100.40", "proxy kind");
    CheckAbsent(diag::ProxyLine(kind), "forged", "proxy line");
  }

  CheckLine(diag::ProxyLine("manual-loopback"), "user=manual-loopback", "proxy: the line");
  CheckLine(diag::ProxyLine(""), "user=unknown", "proxy: an app too old to send a kind");
  CheckLine(diag::ProxyLine("pac+manual-private"), "user=pac+manual-private", "proxy: a combination");
  for (const std::string_view bad :
       {"manual-private+pac", "pac+pac", "pac+", "+pac", "proxy.example:8080", "none+pac", "PAC",
        "manual-loopback\n[app][x] y"}) {
    CheckLine(diag::ProxyLine(bad), "user=unknown",
              "proxy: a kind the app cannot produce is unknown (\"" + std::string(bad) + "\")");
  }
}

// The [app][network-country] line.
void TestNetworkCountryLine() {
  CheckLine(diag::NetworkCountryLine("ru", "mobile-broadband"), "country=ru source=mobile-broadband",
            "network country: a country and its source");
  CheckLine(diag::NetworkCountryLine("", "not-mobile-broadband"), "country=none source=not-mobile-broadband",
            "network country: none, and why");
  CheckLine(diag::NetworkCountryLine("Russia\n[app][x]", "the locale"), "country=none source=unknown",
            "network country: a peer's garbage");
}

// Every tag survives the sdk's tag rule unchanged.
void TestTags() {
  for (const std::string_view tag : {diag::kTagService, diag::kTagAdapter, diag::kTagDns, diag::kTagProxy,
                                     diag::kTagNetworkCountry}) {
    // the sdk keeps [A-Za-z0-9._-], at most 32 of them (sdk app_log.go)
    bool kept = !tag.empty() && tag.size() <= 32;
    for (const char c : tag) {
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_'))
        kept = false;
    }
    Check(kept, "tags: \"" + std::string(tag) + "\" survives the sdk's tag rule unchanged");
  }
}

}  // namespace

int main() {
  TestServiceLine();
  TestAdapterLine();
  TestDnsLine();
  TestProxy();
  TestNetworkCountryLine();
  TestTags();
  std::cout << (gCases - gFailures) << "/" << gCases << " diagnostic line checks passed" << std::endl;
  return gFailures == 0 ? 0 : 1;
}
