// Executable spec for the bundled network space identity
// (Common/Ids.h, Common/NetworkSpaceStartup.h): which host the official space
// is keyed by, which legacy key is re-keyed to it, how the env overrides
// resolve, that the re-key runs BEFORE the space is written and bound, and the
// values SdkHost's two whole-values writers put over what a space stores -
// run against the SAME headers the app compiles, on any host with a C++20
// compiler (Ids.h needs a `GUID`, so point -I at a dir with a guiddef.h stub
// or at the mingw headers).
//
//   c++ -std=c++20 -I ../src/Common -I <dir with guiddef.h> network-space-tests.cpp
//       -o /tmp/network-space-tests
//   /tmp/network-space-tests
//
// By default the value writers are instantiated with a stand-in that carries
// the fields of urnet::NetworkSpaceValues under the generated wrapper's names.
// With URNW_NETWORK_SPACE_TESTS_SDK they are built against the generated header
// itself, and a stored export goes through them by the header's own json
// conversions (it needs nlohmann/json; both are system includes because the
// generated code does not build with -Wextra -Werror):
//
//   c++ -std=c++20 -DURNW_NETWORK_SPACE_TESTS_SDK -I ../src/Common -I <guiddef dir>
//       -isystem <dir of urnetwork_sdk.hpp> -isystem <dir of nlohmann/> network-space-tests.cpp ...
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Ids.h"
#include "NetworkSpaceStartup.h"

#if defined(URNW_NETWORK_SPACE_TESTS_SDK)
#include <nlohmann/json.hpp>

#include "urnetwork_sdk.hpp"
#endif

using namespace urnw;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << "\n";
  }
}

void CheckEq(const std::string& expected, const std::string& actual, const std::string& what) {
  Check(expected == actual, what + ": expected \"" + expected + "\", got \"" + actual + "\"");
}

std::string Describe(const netspace::Key& key) { return key.hostName + "/" + key.envName; }

// The operator stays bringyour.com. The planned move to *.ur.network was
// cancelled, and a build bundling the official space under that key (or under
// anything but the operator host) would derive api.ur.network, a dead host.
void OperatorHost() {
  std::cout << "operator host\n";
  const std::string_view host(ids::kNetworkSpaceHostName);
  Check(host != std::string_view(ids::kLegacyNetworkSpaceHostName),
        "the bundled host is the legacy ur.network key of the cancelled migration, "
        "not the operator host");
  CheckEq("bringyour.com", std::string(host), "the operator host is bringyour.com");
  CheckEq("ur.network", ids::kLegacyNetworkSpaceHostName,
          "the legacy key earlier builds bundled under");
  CheckEq("main", ids::kNetworkSpaceEnvName, "the official env");

  CheckEq("bringyour.com/main", Describe(netspace::OfficialKey()), "official key");
  CheckEq("ur.network/main", Describe(netspace::LegacyOfficialKey()), "legacy official key");
  Check(!(netspace::OfficialKey() == netspace::LegacyOfficialKey()),
        "the re-key has somewhere to go (the SDK no-ops equal keys)");
}

// URNETWORK_NETWORK_HOST points the client elsewhere; URNETWORK_NETWORK_ENV
// only counts with it.
void BundledSpaceResolution() {
  std::cout << "bundled space resolution\n";
  {
    const auto space = netspace::ResolveBundledSpace("", "");
    CheckEq("bringyour.com/main", Describe(space.key), "no override: the official key");
    Check(space.official, "no override: official");
  }
  {
    const auto space = netspace::ResolveBundledSpace("", "beta");
    CheckEq("bringyour.com/main", Describe(space.key),
            "an env override without a host override is ignored: the official space is main");
    Check(space.official, "env override alone: still official");
  }
  {
    const auto space = netspace::ResolveBundledSpace("test.example", "");
    CheckEq("test.example/main", Describe(space.key), "host override: main env by default");
    Check(!space.official, "host override: not official");
  }
  {
    const auto space = netspace::ResolveBundledSpace("test.example", "beta");
    CheckEq("test.example/beta", Describe(space.key), "host + env override");
    Check(!space.official, "host + env override: not official");
  }
}

// The SDK contract (sdk network_space.go MigrateNetworkSpace): the legacy
// space is re-keyed before the bundled space is created, updated, made active
// or has a Device built on it, on every launch.
void StartupOrder() {
  std::cout << "startup order\n";
  std::vector<std::string> calls;
  const int bound = netspace::StartBundledSpace(
      [&](const netspace::Key& from, const netspace::Key& to) {
        calls.push_back("migrate " + Describe(from) + " -> " + Describe(to));
      },
      [&] {
        calls.push_back("bind bundled space");
        return 7;
      });
  Check(calls.size() == 2, "exactly the re-key and the bind: " + std::to_string(calls.size()) +
                               " calls");
  CheckEq("migrate ur.network/main -> bringyour.com/main", calls.empty() ? "" : calls[0],
          "the re-key runs first, legacy key -> operator key");
  CheckEq("bind bundled space", calls.size() < 2 ? "" : calls[1],
          "the bundled space is bound only after the re-key");
  Check(bound == 7, "the bound space is what the caller gets back");
}

#if !defined(URNW_NETWORK_SPACE_TESTS_SDK)
// The fields of urnet::NetworkSpaceValues, by the generated wrapper's names and
// types. The two nested settings are stood in for by what the checks compare:
// the private extender's address and secret, the VLESS server's address.
struct NetExtender {
  std::string ip;
  std::string secret;
  bool operator==(const NetExtender&) const = default;
};

struct SpaceValues {
  std::optional<std::string> env_secret;
  std::optional<bool> bundled;
  std::optional<bool> net_expose_server_ips;
  std::optional<bool> net_expose_server_host_names;
  std::optional<std::string> link_host_name;
  std::optional<std::string> migration_host_name;
  std::optional<std::string> store;
  std::optional<std::string> wallet;
  std::optional<bool> sso_google;
  std::optional<std::string> api_url;
  std::optional<std::string> platform_url;
  std::optional<std::string> alt_url;
  std::optional<NetExtender> net_extender;
  std::optional<std::string> extender_dns_name;
  std::optional<std::string> gossip_url;
  std::optional<std::vector<std::string>> extender_root_public_keys;
  std::optional<std::vector<std::string>> extender_hosts;
  std::optional<std::string> vless;
  std::optional<std::vector<std::string>> control_doh_urls_ipv4;
  std::optional<std::vector<std::string>> control_doh_urls_ipv6;
};

// What a user saves in a space on its own screens: the extender settings
// (Account > Extenders), the root keys an extender import took, the private
// extender, the VLESS server, the bootstrap DNS-over-HTTPS servers -- and an
// alt url nothing in this app writes.
SpaceValues Saved() {
  SpaceValues values;
  values.alt_url = "https://alt.example.test:8443";
  values.net_extender = NetExtender{"192.0.2.7", "s3cret"};
  values.extender_dns_name = "ext.example.test";
  values.gossip_url = "wss://gossip.example.test";
  values.extender_root_public_keys = std::vector<std::string>{"root-key-1"};
  values.extender_hosts = std::vector<std::string>{"ext1.example.test", "203.0.113.9"};
  values.vless = "vless.example.test";
  values.control_doh_urls_ipv4 = std::vector<std::string>{"https://223.5.5.5/dns-query"};
  values.control_doh_urls_ipv6 = std::vector<std::string>{"https://[2001:db8::53]/dns-query"};
  return values;
}

void CheckKeepsSaved(const SpaceValues& written, const std::string& writer) {
  const SpaceValues saved = Saved();
  Check(written.extender_dns_name == saved.extender_dns_name, writer + " keeps the extender dns name");
  Check(written.gossip_url == saved.gossip_url, writer + " keeps the gossip url");
  Check(written.extender_hosts == saved.extender_hosts, writer + " keeps the manual extender hosts");
  Check(written.extender_root_public_keys == saved.extender_root_public_keys,
        writer + " keeps the extender root keys");
  Check(written.net_extender == saved.net_extender, writer + " keeps the private extender");
  Check(written.vless == saved.vless, writer + " keeps the VLESS server");
  Check(written.control_doh_urls_ipv4 == saved.control_doh_urls_ipv4,
        writer + " keeps the v4 bootstrap DoH servers");
  Check(written.control_doh_urls_ipv6 == saved.control_doh_urls_ipv6,
        writer + " keeps the v6 bootstrap DoH servers");
  Check(written.alt_url == saved.alt_url, writer + " keeps a stored value nobody here owns");
}

// BuildNetworkSpace's write at every launch: the bundled space's values over
// what it stores. It used to be a fresh set that kept the VLESS server alone.
void BundledSpaceValues() {
  std::cout << "bundled space values\n";
  SpaceValues stored = Saved();
  stored.bundled = false;
  stored.link_host_name = "stale.example";
  // an earlier build bundled the space under ur.network with this migration host
  stored.migration_host_name = "bringyour.com";
  stored.store = "play";
  stored.wallet = "other";
  stored.sso_google = false;
  stored.env_secret = "stale";
  stored.api_url = "https://api.override.test";
  stored.platform_url = "wss://connect.override.test";
  const SpaceValues written = netspace::BundledSpaceValuesOver(stored);
  CheckKeepsSaved(written, "the launch");
  Check(written.bundled == std::optional<bool>(true), "the bundled space is bundled");
  Check(written.net_expose_server_ips == std::optional<bool>(true) &&
            written.net_expose_server_host_names == std::optional<bool>(true),
        "the bundled space exposes the server ips and host names");
  Check(written.link_host_name == std::optional<std::string>("ur.io"), "ur.io stays the link host");
  Check(!written.migration_host_name, "no migration host: ServiceUrl would prefer it");
  Check(written.store == std::optional<std::string>("") &&
            written.wallet == std::optional<std::string>("circle") &&
            written.sso_google == std::optional<bool>(true) &&
            written.env_secret == std::optional<std::string>(""),
        "the store, wallet, sso and env secret are the bundle's");
  Check(!written.api_url && !written.platform_url,
        "a bundled space carries no url overrides, so a stored one is cleared");

  const SpaceValues fresh = netspace::BundledSpaceValuesOver(SpaceValues{});
  Check(fresh.bundled == std::optional<bool>(true) &&
            fresh.link_host_name == std::optional<std::string>("ur.io") &&
            !fresh.extender_dns_name && !fresh.net_extender && !fresh.vless &&
            !fresh.control_doh_urls_ipv4,
        "a first launch writes the bundle's values and nothing else");
}

// ApplyNetworkServer's write: a server's values over what the space under its
// key stores.
void ServerSpaceValues() {
  std::cout << "server space values\n";
  SpaceValues stored = Saved();
  stored.api_url = "https://old-api.example.test";
  stored.migration_host_name = "stale.example";
  const SpaceValues custom = netspace::ServerSpaceValuesOver(stored, false, "example.test", "",
                                                             "wss://connect2.example.test");
  CheckKeepsSaved(custom, "applying a server");
  Check(custom.bundled == std::optional<bool>(false), "a custom deployment is not bundled");
  Check(custom.link_host_name == std::optional<std::string>("example.test"),
        "a custom deployment links to its own host");
  Check(!custom.migration_host_name, "no migration host for any space");
  Check(custom.api_url == std::optional<std::string>("") &&
            custom.platform_url == std::optional<std::string>("wss://connect2.example.test"),
        "the url overrides are the sheet's, an empty one deriving from the host");

  const SpaceValues official = netspace::ServerSpaceValuesOver(
      Saved(), true, std::string(ids::kNetworkSpaceHostName), "", "");
  CheckKeepsSaved(official, "applying the official server");
  Check(official.bundled == std::optional<bool>(true) &&
            official.link_host_name == std::optional<std::string>("ur.io"),
        "the official host with no overrides is the bundled space, linking to ur.io");
  const SpaceValues overridden = netspace::ServerSpaceValuesOver(
      SpaceValues{}, true, std::string(ids::kNetworkSpaceHostName), "https://api.example.test", "");
  Check(overridden.bundled == std::optional<bool>(false),
        "an override takes the official host out of the bundle's pinned endpoints");
}
#else
// The writers against the SDK's own type, a stored export read and written
// back by the header's json conversions.
void SpaceValuesThroughTheSdkJson() {
  std::cout << "space values through the sdk json\n";
  const nlohmann::json saved = nlohmann::json::parse(R"({
    "alt_url": "https://alt.example.test:8443",
    "net_extender": {"ip": "192.0.2.7", "secret": "s3cret"},
    "extender_dns_name": "ext.example.test",
    "gossip_url": "wss://gossip.example.test",
    "extender_root_public_keys": ["root-key-1"],
    "extender_hosts": ["ext1.example.test", "203.0.113.9"],
    "vless": {"enabled": true, "address": "vless.example.test", "port": 443},
    "control_doh_urls_ipv4": ["https://223.5.5.5/dns-query"],
    "control_doh_urls_ipv6": ["https://[2001:db8::53]/dns-query"]
  })");
  nlohmann::json storedJson = saved;
  storedJson["bundled"] = false;
  storedJson["link_host_name"] = "stale.example";
  storedJson["migration_host_name"] = "bringyour.com";
  storedJson["api_url"] = "https://api.override.test";
  storedJson["platform_url"] = "wss://connect.override.test";
  const urnet::NetworkSpaceValues stored = storedJson.get<urnet::NetworkSpaceValues>();

  const nlohmann::json launched = netspace::BundledSpaceValuesOver(stored);
  for (const auto& item : saved.items()) {
    Check(launched.contains(item.key()) && launched[item.key()] == item.value(),
          "the launch keeps " + item.key() + ": " + launched.dump());
  }
  Check(launched.value("bundled", false), "the bundled space is bundled");
  CheckEq("ur.io", launched.value("link_host_name", std::string()), "the bundled link host");
  Check(!launched.contains("migration_host_name") && !launched.contains("api_url") &&
            !launched.contains("platform_url"),
        "no migration host and no url overrides: " + launched.dump());

  const nlohmann::json applied = netspace::ServerSpaceValuesOver(stored, false, "example.test", "",
                                                                 "wss://connect2.example.test");
  for (const auto& item : saved.items()) {
    Check(applied.contains(item.key()) && applied[item.key()] == item.value(),
          "applying a server keeps " + item.key() + ": " + applied.dump());
  }
  CheckEq("example.test", applied.value("link_host_name", std::string()), "the server's link host");
  CheckEq("wss://connect2.example.test", applied.value("platform_url", std::string()),
          "the sheet's connect url");
  Check(!applied.contains("migration_host_name"), "no migration host: " + applied.dump());
}
#endif

}  // namespace

int main() {
  OperatorHost();
  BundledSpaceResolution();
  StartupOrder();
#if defined(URNW_NETWORK_SPACE_TESTS_SDK)
  SpaceValuesThroughTheSdkJson();
#else
  BundledSpaceValues();
  ServerSpaceValues();
#endif
  std::cout << gCases << " checks, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
