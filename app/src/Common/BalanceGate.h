// The insufficient-balance gate: what the connect button offers while the
// account is out of balance, and when the app clears the connect request on
// its own.
//
// Out of balance is a billing state, not a dropped tunnel, but while the SDK
// still holds a destination the service keeps the capture routes in place and
// there is no exit to carry them, so the machine has no internet. The connect
// button used to be disabled for the whole state, including when it read
// Disconnect, which left the tray as the only way out. Here the way out is
// never gated by balance: only a connect or a retry is.
//
// Auto-disconnect follows the other apps (apple ConnectViewModel disconnects
// when the contract reports insufficient balance), with a grace period so a
// transient latch from a backend contract error does not drop a funded
// session. It never fires for a supporter or while a post-checkout
// confirmation poll is running (both excluded from the gate itself), and never
// with the kill switch on: a deliberate stop lifts the firewall policy, and the
// kill switch user asked to stay blocked rather than fall back to the open
// internet. They are still offered Disconnect.
//
// Pure and constexpr with no Windows headers, so tools/balance-gate-tests.cpp
// runs it on any host with a C++20 compiler.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>

namespace urnw::balance {

// How long the gate must hold before the connect request is cleared.
inline constexpr std::chrono::seconds kAutoDisconnectGrace{15};

// The existing gate: out of balance, not a supporter, and no confirmation poll
// bridging a just-made purchase.
inline constexpr bool OutOfBalance(bool insufficientBalance, bool supporter,
                                   bool confirming) {
  return insufficientBalance && !supporter && !confirming;
}

enum class ConnectButtonAction { Connect, Disconnect, Retry };

struct ConnectButton {
  ConnectButtonAction action = ConnectButtonAction::Connect;
  bool enabled = true;
};

// failed: the rendered health is the terminal connect failure.
// actionIsDisconnect: gesture::ActionIsDisconnect for this instant.
// confirming / outOfBalance: the balance states layered over the connection.
// transitional / watchdogFired: the connecting duplicate-press guard.
inline constexpr ConnectButton DecideConnectButton(bool failed, bool actionIsDisconnect,
                                                   bool confirming, bool outOfBalance,
                                                   bool transitional,
                                                   bool watchdogFired) {
  ConnectButton b;
  // a retry reconnects, which cannot succeed out of balance; offer the way out
  if (failed && !outOfBalance) {
    b.action = ConnectButtonAction::Retry;
  } else if (actionIsDisconnect) {
    b.action = ConnectButtonAction::Disconnect;
  } else {
    b.action = ConnectButtonAction::Connect;
  }
  const bool blocked = confirming || outOfBalance;
  if (blocked && b.action == ConnectButtonAction::Disconnect) {
    // the connecting guard exists to stop a duplicate connect, not a disconnect
    b.enabled = true;
  } else {
    b.enabled = !blocked && (!transitional || watchdogFired);
  }
  return b;
}

// heldFor: how long OutOfBalance has held continuously.
inline constexpr bool AutoDisconnectDue(bool outOfBalance, bool actionIsDisconnect,
                                        bool killSwitch,
                                        std::chrono::steady_clock::duration heldFor) {
  return outOfBalance && actionIsDisconnect && !killSwitch &&
         kAutoDisconnectGrace <= heldFor;
}

}  // namespace urnw::balance
