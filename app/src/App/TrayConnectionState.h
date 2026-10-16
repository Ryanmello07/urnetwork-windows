// What the tray says about the connection: the status in its tooltip, whether
// its icon shows connected, and its one Connect/Disconnect menu item. Read from
// the service's last tunnel status and the health the last stats push carried
// (AppController holds both).
//
// Connected, in words and in the icon, only with the tunnel up and the health
// connected. The health is held across a hide and dropped on every tunnel
// transition; with none since the last one (the stats feed is
// presentation-scoped) the tunnel's own claim stands. With the tunnel up the
// health says how far the connection got. With it down the machine's own state
// is the line (#41): routes still installed, the kill switch's armed policy,
// another policy left in force, then the service's own failsafe stop; anything
// else is the app name alone.
//
// The menu offers Disconnect whenever the rule the connect page's button shares
// says so (gesture::ActionIsDisconnect), and Connect otherwise.
//
// Pure: no Windows headers, no clock, no allocation. The build host runs it
// (tools/tray-connection-state-tests.cpp).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string_view>

#include "ConnectionHealth.h"

namespace urnw::tray {

// The service's tunnel, as its last status said (proto::TunnelState folded).
enum class Tunnel {
  // Stopped, or an rpc-only session: nothing is carried.
  Down,
  // Starting or Preparing: a connect is in progress.
  Starting,
  Up,
  // Stopping: a disconnect is in progress.
  Stopping,
  // Error: the last start failed.
  Failed,
};

// proto::TunnelState folded into Tunnel. A template over the enum, so this
// header needs no Protocol.h and its json; tests/tray_connection_state_test.go
// holds the enumerators named here to Protocol.h's.
template <class TunnelState>
inline constexpr Tunnel TunnelOf(TunnelState state) {
  if (state == TunnelState::Up) return Tunnel::Up;
  if (state == TunnelState::Starting || state == TunnelState::Preparing) return Tunnel::Starting;
  if (state == TunnelState::Stopping) return Tunnel::Stopping;
  if (state == TunnelState::Error) return Tunnel::Failed;
  return Tunnel::Down;
}

// Everything the tray decides on.
struct Facts {
  bool signedIn = false;
  // the service has told this process its tunnel's state, pushed or answered
  bool statusKnown = false;
  Tunnel tunnel = Tunnel::Down;
  bool routesInstalled = false;
  // "off" | "armed" | "connecting" | "connected"; empty reads as off
  std::string_view wfpState = "off";
  // the last teardown was the service's own failsafe stop
  bool failsafeStop = false;
  // the last stats push's health since the last tunnel transition
  std::optional<health::State> health;
  // gesture::ActionIsDisconnect over the same status and health, the rule the
  // connect page's button shares
  bool actionIsDisconnect = false;
};

// The tooltip's status after the app name. Each one is a line the connect page
// or the tray already shows; AppController::UpdateTray names their strings.
enum class Status {
  // the app name alone: disconnected, or nothing known yet
  None,
  Connected,
  FindingProviders,
  Degraded,
  Connecting,
  Failed,
  // the routes are still installed with no tunnel up
  Disconnecting,
  // the kill switch's armed policy holds the machine
  BlockedKillSwitch,
  // another firewall policy is in force with no tunnel up
  BlockedNoTunnel,
  // the service stopped the tunnel by itself
  FailsafeStopped,
};

// The menu's Connect/Disconnect item.
enum class Action { None, Connect, Disconnect };

struct View {
  Status status = Status::None;
  bool connectedIcon = false;
};

// The tooltip's status and the icon.
inline constexpr View Decide(const Facts& facts) {
  View view;
  // no stats since the tunnel came up: its own claim stands
  const health::State shown = facts.health.value_or(health::State::Connected);
  const bool up = facts.statusKnown && facts.tunnel == Tunnel::Up;
  view.connectedIcon = up && shown == health::State::Connected;
  if (!up && facts.statusKnown) {
    if (facts.routesInstalled) {
      view.status = Status::Disconnecting;
    } else if (facts.wfpState == "armed") {
      view.status = Status::BlockedKillSwitch;
    } else if (facts.wfpState != "off") {
      view.status = Status::BlockedNoTunnel;
    } else if (facts.failsafeStop) {
      view.status = Status::FailsafeStopped;
    }
    return view;
  }
  if (up) {
    switch (shown) {
      case health::State::Connected:
        view.status = Status::Connected;
        break;
      case health::State::Evaluating:
        view.status = Status::FindingProviders;
        break;
      case health::State::Degraded:
        view.status = Status::Degraded;
        break;
      case health::State::Connecting:
        view.status = Status::Connecting;
        break;
      case health::State::Failed:
        view.status = Status::Failed;
        break;
      case health::State::NoService:
      case health::State::Disconnected:
        // a transient disagreement between the two feeds: claim nothing more
        break;
    }
  }
  return view;
}

// The menu's item.
inline constexpr Action ActionFor(const Facts& facts) {
  return facts.actionIsDisconnect ? Action::Disconnect : Action::Connect;
}

}  // namespace urnw::tray
