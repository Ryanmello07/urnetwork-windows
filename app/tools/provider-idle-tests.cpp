// Executable spec for the idle reason under the Earnings provide mode row
// (App/ProviderIdleReason.h, support part P008): every rule in its order, the
// precedences between them, the control mode strings SdkHost returns, and the
// store key of each reason.
//
//   c++ -std=c++20 -I ../src/App provider-idle-tests.cpp -o /tmp/provider-idle-tests && /tmp/provider-idle-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <iostream>
#include <string>

#include "ProviderIdleReason.h"

using urnw::provideridle::kProvideModePublic;
using urnw::provideridle::ProvideControlMode;
using urnw::provideridle::ProvideControlModeFrom;
using urnw::provideridle::ProvideNetworkMode;
using urnw::provideridle::ProviderIdleReason;
using urnw::provideridle::ProviderIdleReasonFor;
using urnw::provideridle::ProviderIdleReasonKey;

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

const char* Name(ProviderIdleReason reason) {
  switch (reason) {
    case ProviderIdleReason::None: return "None";
    case ProviderIdleReason::AutoNotConnected: return "AutoNotConnected";
    case ProviderIdleReason::NetworkOnly: return "NetworkOnly";
    case ProviderIdleReason::PausedWifiOnly: return "PausedWifiOnly";
    case ProviderIdleReason::PausedNoNetwork: return "PausedNoNetwork";
    case ProviderIdleReason::NoTrafficYet: return "NoTrafficYet";
  }
  return "?";
}

void CheckReason(ProviderIdleReason want, ProviderIdleReason got, const std::string& what) {
  Check(want == got, what + ": want " + Name(want) + ", got " + Name(got));
}

// the sdk's ProvideMode values (urnet::ProvideMode*)
constexpr int64_t kNone = 0;
constexpr int64_t kNetwork = 1;
constexpr int64_t kFriendsAndFamily = 2;
constexpr int64_t kPublic = 3;

constexpr ProvideControlMode kAll[] = {ProvideControlMode::Unknown, ProvideControlMode::Never,
                                       ProvideControlMode::Always, ProvideControlMode::Network,
                                       ProvideControlMode::Auto};

ProviderIdleReason Reason(ProvideControlMode control, int64_t live, bool paused,
                          ProvideNetworkMode network, int64_t bytes) {
  return ProviderIdleReasonFor(control, live, paused, network, bytes);
}

}  // namespace

// a constant expression, like the rest of the header
static_assert(ProviderIdleReasonFor(ProvideControlMode::Auto, 1, false, ProvideNetworkMode::All,
                                    0) == ProviderIdleReason::AutoNotConnected);
static_assert(kProvideModePublic == kPublic);

int main() {
  constexpr auto kWiFi = ProvideNetworkMode::WiFi;
  constexpr auto kAllNetworks = ProvideNetworkMode::All;

  // ---- the control mode strings (SdkHost::CurrentProvideControlMode) ----
  {
    Check(ProvideControlModeFrom("never") == ProvideControlMode::Never, "never");
    Check(ProvideControlModeFrom("always") == ProvideControlMode::Always, "always");
    Check(ProvideControlModeFrom("network") == ProvideControlMode::Network, "network");
    Check(ProvideControlModeFrom("auto") == ProvideControlMode::Auto, "auto");
    // the sdk's manual is not exposed: unknown, like anything else
    Check(ProvideControlModeFrom("manual") == ProvideControlMode::Unknown, "manual is unknown");
    Check(ProvideControlModeFrom("") == ProvideControlMode::Unknown, "empty is unknown");
    Check(ProvideControlModeFrom("Always") == ProvideControlMode::Unknown, "the tokens are exact");
  }

  // ---- rule 1: Never and unknown modes get no line ----
  {
    for (ProvideControlMode control : {ProvideControlMode::Never, ProvideControlMode::Unknown}) {
      for (int64_t live : {kNone, kNetwork, kFriendsAndFamily, kPublic}) {
        for (bool paused : {false, true}) {
          for (ProvideNetworkMode network : {kWiFi, kAllNetworks}) {
            for (int64_t bytes : {int64_t{0}, int64_t{4096}}) {
              CheckReason(ProviderIdleReason::None, Reason(control, live, paused, network, bytes),
                          std::string(control == ProvideControlMode::Never ? "never" : "unknown") +
                              " live " + std::to_string(live) + (paused ? " paused" : "") +
                              " bytes " + std::to_string(bytes));
            }
          }
        }
      }
    }
    CheckReason(ProviderIdleReason::None,
                Reason(ProvideControlModeFrom("manual"), kPublic, false, kAllNetworks, 0),
                "manual, public, no bytes");
  }

  // ---- rule 2: Network shares only with the user's own devices ----
  {
    CheckReason(ProviderIdleReason::NetworkOnly,
                Reason(ProvideControlMode::Network, kNetwork, false, kAllNetworks, 0), "network");
    CheckReason(ProviderIdleReason::NetworkOnly,
                Reason(ProvideControlMode::Network, kNetwork, false, kAllNetworks, 1 << 20),
                "network with traffic from own devices");
    CheckReason(ProviderIdleReason::NetworkOnly,
                Reason(ProvideControlMode::Network, kPublic, false, kAllNetworks, 0),
                "network whatever the live mode says");
  }

  // ---- rule 3: Auto provides publicly only while connected ----
  {
    for (int64_t live : {kNone, kNetwork, kFriendsAndFamily}) {
      CheckReason(ProviderIdleReason::AutoNotConnected,
                  Reason(ProvideControlMode::Auto, live, false, kAllNetworks, 0),
                  "auto, live " + std::to_string(live));
      CheckReason(ProviderIdleReason::AutoNotConnected,
                  Reason(ProvideControlMode::Auto, live, false, kAllNetworks, 4096),
                  "auto, live " + std::to_string(live) + ", with bytes");
    }
    // connected: Auto is public and reads like Always
    CheckReason(ProviderIdleReason::NoTrafficYet,
                Reason(ProvideControlMode::Auto, kPublic, false, kAllNetworks, 0),
                "auto, connected, no bytes");
    CheckReason(ProviderIdleReason::None,
                Reason(ProvideControlMode::Auto, kPublic, false, kAllNetworks, 4096),
                "auto, connected, with bytes");
    CheckReason(ProviderIdleReason::PausedNoNetwork,
                Reason(ProvideControlMode::Auto, kPublic, true, kAllNetworks, 0),
                "auto, connected, paused");
  }

  // ---- rules 4 and 5: paused ----
  {
    CheckReason(ProviderIdleReason::PausedWifiOnly,
                Reason(ProvideControlMode::Always, kPublic, true, kWiFi, 0),
                "always, paused, Wi-Fi only");
    CheckReason(ProviderIdleReason::PausedNoNetwork,
                Reason(ProvideControlMode::Always, kPublic, true, kAllNetworks, 0),
                "always, paused, any network");
    // a pause wins over the bytes either way
    CheckReason(ProviderIdleReason::PausedWifiOnly,
                Reason(ProvideControlMode::Always, kPublic, true, kWiFi, 4096),
                "always, paused, Wi-Fi only, with bytes");
    CheckReason(ProviderIdleReason::PausedNoNetwork,
                Reason(ProvideControlMode::Always, kNetwork, true, kAllNetworks, 4096),
                "always, paused, any network, live network");
  }

  // ---- rules 6 and 7: public, with and without traffic ----
  {
    CheckReason(ProviderIdleReason::NoTrafficYet,
                Reason(ProvideControlMode::Always, kPublic, false, kAllNetworks, 0),
                "always, no bytes");
    CheckReason(ProviderIdleReason::None,
                Reason(ProvideControlMode::Always, kPublic, false, kAllNetworks, 1),
                "always, a byte");
    CheckReason(ProviderIdleReason::None,
                Reason(ProvideControlMode::Always, kPublic, false, kWiFi, 1 << 30),
                "always, traffic");
    // not public yet (the live mode is still on its way): nothing to explain
    CheckReason(ProviderIdleReason::None,
                Reason(ProvideControlMode::Always, kNetwork, false, kAllNetworks, 0),
                "always, live network");
    CheckReason(ProviderIdleReason::None,
                Reason(ProvideControlMode::Always, kNone, false, kAllNetworks, 0),
                "always, live none");
  }

  // ---- the precedences ----
  {
    CheckReason(ProviderIdleReason::AutoNotConnected,
                Reason(ProvideControlMode::Auto, kNetwork, true, kWiFi, 0),
                "auto + disconnected + paused");
    CheckReason(ProviderIdleReason::NetworkOnly,
                Reason(ProvideControlMode::Network, kNetwork, true, kWiFi, 0), "network + paused");
    CheckReason(ProviderIdleReason::None,
                Reason(ProvideControlMode::Never, kNone, true, kWiFi, 0), "never + paused");
    CheckReason(ProviderIdleReason::None,
                Reason(ProvideControlMode::Unknown, kPublic, true, kWiFi, 0), "unknown + paused");
  }

  // ---- the desktop passes All: never Wi-Fi only ----
  {
    for (ProvideControlMode control : kAll) {
      for (int64_t live : {kNone, kNetwork, kFriendsAndFamily, kPublic}) {
        for (bool paused : {false, true}) {
          for (int64_t bytes : {int64_t{0}, int64_t{4096}}) {
            Check(Reason(control, live, paused, kAllNetworks, bytes) !=
                      ProviderIdleReason::PausedWifiOnly,
                  "All never gives PausedWifiOnly (live " + std::to_string(live) + ")");
          }
        }
      }
    }
  }

  // ---- the store keys ----
  {
    Check(std::string(ProviderIdleReasonKey(ProviderIdleReason::None)).empty(), "None has no line");
    Check(std::string(ProviderIdleReasonKey(ProviderIdleReason::AutoNotConnected)) ==
              "provider_idle_auto_not_connected",
          "AutoNotConnected key");
    Check(std::string(ProviderIdleReasonKey(ProviderIdleReason::NetworkOnly)) ==
              "provider_idle_network_only",
          "NetworkOnly key");
    Check(std::string(ProviderIdleReasonKey(ProviderIdleReason::PausedWifiOnly)) ==
              "provider_idle_paused_wifi_only",
          "PausedWifiOnly key");
    Check(std::string(ProviderIdleReasonKey(ProviderIdleReason::PausedNoNetwork)) ==
              "provider_idle_paused_no_network",
          "PausedNoNetwork key");
    Check(std::string(ProviderIdleReasonKey(ProviderIdleReason::NoTrafficYet)) ==
              "provider_idle_no_traffic_yet",
          "NoTrafficYet key");
  }

  std::cout << (gCases - gFailures) << "/" << gCases << " provider idle reason checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
