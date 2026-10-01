// Executable spec for the bundled network space identity
// (Common/Ids.h, Common/NetworkSpaceStartup.h): which host the official space
// is keyed by, which legacy key is re-keyed to it, how the env overrides
// resolve, and that the re-key runs BEFORE the space is written and bound -
// run against the SAME headers the app compiles, on any host with a C++20
// compiler (Ids.h needs a `GUID`, so point -I at a dir with a guiddef.h stub
// or at the mingw headers).
//
//   c++ -std=c++20 -I ../src/Common -I <dir with guiddef.h> network-space-tests.cpp
//       -o /tmp/network-space-tests
//   /tmp/network-space-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "Ids.h"
#include "NetworkSpaceStartup.h"

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

}  // namespace

int main() {
  OperatorHost();
  BundledSpaceResolution();
  StartupOrder();
  std::cout << gCases << " checks, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
