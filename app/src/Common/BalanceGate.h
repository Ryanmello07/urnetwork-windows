// The insufficient-balance gate: what the connect button offers while the
// account is out of balance, and how the app reacts to entering that state.
//
// Out of balance is a billing state, not a dropped tunnel. While the SDK still
// holds a destination the service keeps the capture routes in place with no
// exit to carry them, so traffic is held in the tunnel. That is kept on
// purpose: the app never disconnects on its own, because falling back to the
// open internet without the user knowing would leak traffic outside the
// tunnel. Instead the user is told once per episode (a tray notice and the
// in-app banner) and the explicit connect button keeps a working Disconnect.
// It used to be disabled with the connect, which left the tray as the only
// way out. The round hero button is unchanged.
//
// Pure, with no Windows headers or clocks, so tools/balance-gate-tests.cpp
// runs it on any host with a C++20 compiler.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace urnw::balance {

// Out of balance, not a supporter, and no confirmation poll bridging a
// just-made purchase.
inline constexpr bool OutOfBalance(bool insufficientBalance, bool supporter,
                                   bool confirming) {
  return insufficientBalance && !supporter && !confirming;
}

enum class ConnectButtonAction { Connect, Disconnect, Retry };

struct ConnectButton {
  ConnectButtonAction action = ConnectButtonAction::Connect;
  bool enabled = true;
};

// The explicit connect button (not the hero).
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

// Posts the out-of-balance notice once per episode. An episode is the span in
// which the contract reports insufficient balance; only its end re-arms the
// notice. A supporter or a running confirmation poll suppresses the notice
// until the gate actually holds within the same episode.
class GateNoticeTracker {
 public:
  // True exactly when the notice should be posted for this push.
  constexpr bool Update(bool insufficientBalance, bool supporter, bool confirming) {
    if (!insufficientBalance) {
      posted_ = false;
      return false;
    }
    if (posted_ || !OutOfBalance(insufficientBalance, supporter, confirming)) return false;
    posted_ = true;
    return true;
  }

 private:
  bool posted_ = false;
};

// The app's whole reaction to a balance or stats push. sinks provides
// Notice(), which posts the out-of-balance notice, and Disconnect(), the user
// Disconnect path. Disconnect is never called from here: capture stays until
// the user disconnects (see the file comment). It is part of the contract so
// the tests can pin that.
template <class Sinks>
constexpr void ReactToBalancePush(GateNoticeTracker& tracker, bool insufficientBalance,
                                  bool supporter, bool confirming, Sinks& sinks) {
  if (tracker.Update(insufficientBalance, supporter, confirming)) sinks.Notice();
}

}  // namespace urnw::balance
