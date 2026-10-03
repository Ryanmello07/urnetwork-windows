// Executable spec for the block action reason mapping (App/BlockActionReason.h):
// which split-rules activity rows carry the "Safety rule" chip and which offer
// "Route locally" - run against the SAME header the app compiles, on any host
// with a C++20 compiler. The expectations mirror sdk BlockAction.IsSecurity and
// BlockAction.RouteLocalOverridable (sdk/device.go).
//
//   c++ -std=c++20 -I ../src/App block-action-reason-tests.cpp \
//       -o /tmp/block-action-reason-tests && /tmp/block-action-reason-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>

#include "BlockActionReason.h"

using namespace urnw::block_action_reason;

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

struct Row {
  const char* reason;
  bool security;
  bool overridable;
};

}  // namespace

int main() {
  // the reason strings are the sdk's wire values, not app-local names
  Check(kSecurityEncrypted == "security-encrypted", "encrypted wire value");
  Check(kSecurityBittorrent == "security-bittorrent", "bittorrent wire value");
  Check(kSecurityPort == "security-port", "port wire value");
  Check(kSecurityIp == "security-ip", "ip wire value");
  Check(kSecuritySmtp == "security-smtp", "smtp wire value");
  Check(kSecurity == "security", "security wire value");
  Check(kBlocker == "blocker", "blocker wire value");
  Check(kOverride == "override", "override wire value");

  const Row rows[] = {
      {"security-encrypted", true, true},
      {"security-port", true, true},
      {"security-bittorrent", true, false},
      {"security-ip", true, false},
      {"security-smtp", true, false},
      {"security", true, false},
      {"blocker", false, false},
      {"override", false, false},
      // ordinary provider-routed traffic, and an sdk too old to send a reason
      {"", false, false},
      // unknown future reasons are neither: no chip, no offer
      {"security-something-new", false, false},
      {"SECURITY-ENCRYPTED", false, false},
  };
  for (const auto& row : rows) {
    const std::string r = row.reason;
    Check(IsSecurity(r) == row.security, "IsSecurity(\"" + r + "\")");
    Check(RouteLocalOverridable(r) == row.overridable,
          "RouteLocalOverridable(\"" + r + "\")");
    // no override: offered exactly when overridable
    Check(OffersRouteLocal(r, false) == row.overridable,
          "OffersRouteLocal(\"" + r + "\", no override)");
    // a rule already decided it: never offered (the row tap edits that rule)
    Check(!OffersRouteLocal(r, true), "OffersRouteLocal(\"" + r + "\", override)");
  }

  std::cout << (gFailures == 0 ? "PASS" : "FAIL") << " block-action-reason-tests: " << gCases
            << " checks, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
