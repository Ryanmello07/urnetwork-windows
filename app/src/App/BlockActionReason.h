// What decided a block action, and what the split-rules activity row offers for
// it. Mirrors sdk.BlockAction.IsSecurity / RouteLocalOverridable (sdk/device.go,
// BlockActionReason*): the reason string crosses the C ABI as the BlockAction
// json field "Reason" and is empty for ordinary provider-routed traffic.
//
// Pure (no WinRT, no SDK header) so tools/block-action-reason-tests.cpp can run
// it on any host with a C++20 compiler.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string_view>

namespace urnw::block_action_reason {

// the sdk's BlockActionReason values
inline constexpr std::string_view kSecurityEncrypted = "security-encrypted";
inline constexpr std::string_view kSecurityBittorrent = "security-bittorrent";
inline constexpr std::string_view kSecurityPort = "security-port";
inline constexpr std::string_view kSecurityIp = "security-ip";
inline constexpr std::string_view kSecuritySmtp = "security-smtp";
inline constexpr std::string_view kSecurity = "security";
inline constexpr std::string_view kBlocker = "blocker";
inline constexpr std::string_view kOverride = "override";

// The URnetwork safety rules decided the action (sdk BlockAction.IsSecurity).
// The row shows the "Safety rule" chip for these.
inline bool IsSecurity(std::string_view reason) {
  return reason == kSecurityEncrypted || reason == kSecurityBittorrent ||
         reason == kSecurityPort || reason == kSecurityIp || reason == kSecuritySmtp ||
         reason == kSecurity;
}

// A route-local rule can make the traffic work outside the tunnel (sdk
// BlockAction.RouteLocalOverridable). BitTorrent and non-public destinations
// are never overridable.
inline bool RouteLocalOverridable(std::string_view reason) {
  return reason == kSecurityEncrypted || reason == kSecurityPort;
}

// Whether the activity row offers "Route locally": the reason is overridable
// and no user rule already decided the action (a decided action opens that
// rule instead, through the same editor).
inline bool OffersRouteLocal(std::string_view reason, bool hasOverride) {
  return !hasOverride && RouteLocalOverridable(reason);
}

}  // namespace urnw::block_action_reason
