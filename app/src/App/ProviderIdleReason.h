// Why providing is enabled but idle: the line under the Earnings page's provide
// mode row (support part P008). A provider that was enabled and earned nothing
// saw an empty chart and no reason, and read it as "the app doesn't pay".
//
// Every input is local, so the line needs no server: the control mode the user
// picked, the device's live provide mode, whether providing is paused, and the
// provider bytes in the statistics window. Auto has its own case. In Auto the
// sdk provides publicly only while this device's VPN is connected
// (DeviceLocal.applyProvideControlModeWithLock); disconnected it provides only
// to the user's own devices, while the page still shows the plots enabled and
// the dot green, so an Auto user who never connects earns nothing and was told
// nothing.
//
// Pure and header-only, with no Windows headers, so
// tools/provider-idle-tests.cpp runs it on any host with a C++20 compiler;
// WalletPage.cpp passes in what it reads from SdkHost.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string_view>

namespace urnw::provideridle {

enum class ProviderIdleReason {
  None,
  AutoNotConnected,  // Auto, and the live mode is not public: the VPN is not connected
  NetworkOnly,       // the private provider: only the user's own devices
  PausedWifiOnly,    // paused, and providing is set to Wi-Fi only
  PausedNoNetwork,   // paused on a network it cannot provide on
  NoTrafficYet,      // public and running, with nothing carried in the window
};

// SdkHost::CurrentProvideControlMode's values. Anything else, the sdk's
// unexposed "manual" among them, is Unknown and gets no line.
enum class ProvideControlMode { Unknown, Never, Always, Network, Auto };

// Where providing may run. The mobile apps can limit it to Wi-Fi; the desktop
// provides on any network, so Windows passes All and PausedWifiOnly never
// occurs here.
enum class ProvideNetworkMode { WiFi, All };

// The live provide mode's public tier (urnet::ProvideModePublic).
inline constexpr int64_t kProvideModePublic = 3;

constexpr ProvideControlMode ProvideControlModeFrom(std::string_view mode) {
  if (mode == "never") return ProvideControlMode::Never;
  if (mode == "always") return ProvideControlMode::Always;
  if (mode == "network") return ProvideControlMode::Network;
  if (mode == "auto") return ProvideControlMode::Auto;
  return ProvideControlMode::Unknown;
}

// The first matching rule wins:
//   1. Never or Unknown: None. "Providing is disabled" already covers Never.
//   2. Network: NetworkOnly.
//   3. Auto while the live mode is not public: AutoNotConnected.
//   4. paused, with providing on Wi-Fi only: PausedWifiOnly.
//   5. paused: PausedNoNetwork.
//   6. public, with no provider bytes in the window: NoTrafficYet.
//   7. otherwise None.
constexpr ProviderIdleReason ProviderIdleReasonFor(ProvideControlMode controlMode,
                                                   int64_t liveProvideMode, bool providePaused,
                                                   ProvideNetworkMode provideNetworkMode,
                                                   int64_t recentProviderBytes) {
  switch (controlMode) {
    case ProvideControlMode::Unknown:
    case ProvideControlMode::Never:
      return ProviderIdleReason::None;
    case ProvideControlMode::Network:
      return ProviderIdleReason::NetworkOnly;
    case ProvideControlMode::Auto:
      if (liveProvideMode != kProvideModePublic) return ProviderIdleReason::AutoNotConnected;
      break;
    case ProvideControlMode::Always:
      break;
  }
  if (providePaused) {
    return provideNetworkMode == ProvideNetworkMode::WiFi ? ProviderIdleReason::PausedWifiOnly
                                                          : ProviderIdleReason::PausedNoNetwork;
  }
  if (liveProvideMode == kProvideModePublic && recentProviderBytes <= 0) {
    return ProviderIdleReason::NoTrafficYet;
  }
  return ProviderIdleReason::None;
}

// The store key of the line for `reason`, "" for None (no line).
constexpr const char* ProviderIdleReasonKey(ProviderIdleReason reason) {
  switch (reason) {
    case ProviderIdleReason::AutoNotConnected: return "provider_idle_auto_not_connected";
    case ProviderIdleReason::NetworkOnly: return "provider_idle_network_only";
    case ProviderIdleReason::PausedWifiOnly: return "provider_idle_paused_wifi_only";
    case ProviderIdleReason::PausedNoNetwork: return "provider_idle_paused_no_network";
    case ProviderIdleReason::NoTrafficYet: return "provider_idle_no_traffic_yet";
    case ProviderIdleReason::None: break;
  }
  return "";
}

}  // namespace urnw::provideridle
