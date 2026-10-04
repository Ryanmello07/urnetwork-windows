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
// Two decisions, kept apart (owner decision 2026-10-03: "connect with no
// balance should be blocked; however, connected and then runs out of balance
// should keep the connection active"):
//   * START CONNECT. Every connect gesture (the connect button and hero, the
//     tray's Connect, a location or peer row) is admitted only outside the
//     gate. Blocked, it starts nothing and shows the upgrade path instead.
//     SdkHost's connect entry points all pass through AdmitStartConnect, so a
//     new surface cannot skip it.
//   * ALREADY CONNECTED. A live session is never dropped for balance: the
//     reaction below never disconnects, and a reattach (app launch resuming a
//     running session, a network-server change, the service watchdog) is not
//     a connect gesture and is never gated. Disconnect is always admitted.
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

// The out-of-balance state the start-connect gate reads. The live signal,
// ContractStatus.InsufficientBalance, does not survive a Disconnect: the SDK
// resets the contract status to an empty one on every destination change, the
// user's Disconnect included. Read raw, "held out of balance -> Disconnect ->
// tray Connect" saw a funded account and started the tunnel again, so the gate
// only held while a session was up, where the tray offers Disconnect anyway.
// The latch keeps the state until there is evidence it is over: providers
// attach on a live session, the subscription balance rises above the lowest
// value seen since it latched, or the session ends (Reset on sign-in/out).
// Matches the linux OutOfBalanceLatch.
class OutOfBalanceLatch {
 public:
  struct Observation {
    // ContractStatus.InsufficientBalance from the latest stats push
    bool insufficientBalance = false;
    // a session is up and the connect controller reports providers attached
    bool providersConnected = false;
    // the subscription balance has been fetched, and its available bytes
    bool balanceKnown = false;
    long long availableBytes = 0;
  };

  constexpr void Observe(const Observation& o) {
    if (o.insufficientBalance) {
      latched_ = true;
      NoteBalance(o);
      return;
    }
    if (!latched_) return;
    if (o.providersConnected) {
      Reset();
      return;
    }
    if (o.balanceKnown && lowKnown_ && lowBytes_ < o.availableBytes) {
      Reset();
      return;
    }
    NoteBalance(o);
  }

  constexpr void Reset() {
    latched_ = false;
    lowKnown_ = false;
    lowBytes_ = 0;
  }

  constexpr bool InsufficientBalance() const { return latched_; }

 private:
  constexpr void NoteBalance(const Observation& o) {
    if (!o.balanceKnown) return;
    if (!lowKnown_ || o.availableBytes < lowBytes_) {
      lowKnown_ = true;
      lowBytes_ = o.availableBytes;
    }
  }

  bool latched_ = false;
  bool lowKnown_ = false;
  long long lowBytes_ = 0;
};

// What a connect/disconnect gesture does: the tray's single toggle, and the
// connect button and hero. actionIsDisconnect (gesture::ActionIsDisconnect)
// wins, so a session that ran out of balance keeps its way out.
enum class ConnectGestureAction { Connect, Disconnect, Upgrade };

inline constexpr ConnectGestureAction DecideConnectGesture(bool actionIsDisconnect,
                                                           bool outOfBalance) {
  if (actionIsDisconnect) return ConnectGestureAction::Disconnect;
  if (outOfBalance) return ConnectGestureAction::Upgrade;
  return ConnectGestureAction::Connect;
}

// The start-connect decision for an entry point that only connects. sinks
// provides Upgrade(), the upgrade path shown in place of a blocked connect.
// True when the connect may start.
template <class Sinks>
constexpr bool AdmitStartConnect(bool outOfBalance, Sinks& sinks) {
  if (outOfBalance) {
    sinks.Upgrade();
    return false;
  }
  return true;
}

// Routes a toggle gesture. sinks provides Connect(), Disconnect() and
// Upgrade().
template <class Sinks>
constexpr void RouteConnectGesture(bool actionIsDisconnect, bool outOfBalance, Sinks& sinks) {
  switch (DecideConnectGesture(actionIsDisconnect, outOfBalance)) {
    case ConnectGestureAction::Disconnect:
      sinks.Disconnect();
      break;
    case ConnectGestureAction::Upgrade:
      sinks.Upgrade();
      break;
    case ConnectGestureAction::Connect:
      sinks.Connect();
      break;
  }
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
