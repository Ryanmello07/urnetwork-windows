// Executable spec for the insufficient-balance gate (Common/BalanceGate.h):
// the explicit connect button keeps a working Disconnect while the account is
// out of balance, the app never disconnects on its own, and the notice posts
// once per out-of-balance episode, never for a supporter or during a
// confirmation poll. A connect gesture from any entry point (the tray's
// Connect, a location row, the connect button) starts nothing out of balance
// and shows the upgrade path; a session already connected is never dropped. Run against the SAME header the app compiles, on any host
// with a C++20 compiler. No clocks: every input is an explicit push.
//
//   c++ -std=c++20 -I ../src/Common balance-gate-tests.cpp \
//       -o /tmp/balance-gate-tests && /tmp/balance-gate-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>

#include "BalanceGate.h"

using namespace urnw::balance;

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

const char* Name(ConnectButtonAction a) {
  switch (a) {
    case ConnectButtonAction::Connect:
      return "connect";
    case ConnectButtonAction::Disconnect:
      return "disconnect";
    case ConnectButtonAction::Retry:
      return "retry";
  }
  return "?";
}

// Counts what the reaction and the connect routing asked for.
struct FakeSinks {
  int notices = 0;
  int disconnects = 0;
  int connects = 0;
  int upgrades = 0;
  void Notice() { ++notices; }
  void Disconnect() { ++disconnects; }
  void Connect() { ++connects; }
  void Upgrade() { ++upgrades; }
};

// A stand-in for SdkHost's connect entry points: each asks AdmitStartConnect
// before it records a session request, exactly as the app's do.
struct FakeHost {
  FakeSinks& sinks;
  bool outOfBalance = false;
  bool live = false;  // a session is up
  int sessionRequests = 0;
  void ConnectFromRow() {
    if (!AdmitStartConnect(outOfBalance, sinks)) return;
    ++sessionRequests;
    live = true;
  }
  // a reattach (launch resume, server change, watchdog) is not a connect
  void EnsureSession() { ++sessionRequests; }
};

void TestGate() {
  Check(OutOfBalance(true, false, false), "gate: insufficient, basic, not polling is gated");
  Check(!OutOfBalance(true, true, false), "gate: a supporter is never gated");
  Check(!OutOfBalance(true, false, true), "gate: a confirmation poll suspends the gate");
  Check(!OutOfBalance(false, false, false), "gate: a funded account is not gated");
}

// (a) the explicit Disconnect is offered and enabled in the gate
void TestConnectButton() {
  struct Case {
    const char* what;
    bool failed, actionIsDisconnect, confirming, outOfBalance, transitional, watchdogFired;
    ConnectButtonAction action;
    bool enabled;
  };
  const Case cases[] = {
      {"button: out of balance, connected: disconnect enabled", false, true, false, true,
       false, false, ConnectButtonAction::Disconnect, true},
      {"button: out of balance, connecting: disconnect enabled", false, true, false, true,
       true, false, ConnectButtonAction::Disconnect, true},
      {"button: out of balance, failed: disconnect, not a retry that cannot succeed", true,
       true, false, true, false, false, ConnectButtonAction::Disconnect, true},
      {"button: out of balance, disconnected: connect stays blocked", false, false, false,
       true, false, false, ConnectButtonAction::Connect, false},
      {"button: confirming, connected: disconnect enabled", false, true, true, false, false,
       false, ConnectButtonAction::Disconnect, true},
      {"button: confirming, disconnected: connect stays blocked", false, false, true, false,
       false, false, ConnectButtonAction::Connect, false},
      {"button: confirming, failed: retry stays blocked", true, true, true, false, false,
       false, ConnectButtonAction::Retry, false},
      // unchanged outside the gate
      {"button: funded, disconnected: connect", false, false, false, false, false, false,
       ConnectButtonAction::Connect, true},
      {"button: funded, connected: disconnect", false, true, false, false, false, false,
       ConnectButtonAction::Disconnect, true},
      {"button: funded, connecting: guarded until the watchdog", false, true, false, false,
       true, false, ConnectButtonAction::Disconnect, false},
      {"button: funded, connecting past the watchdog: enabled", false, true, false, false,
       true, true, ConnectButtonAction::Disconnect, true},
      {"button: funded, failed: retry", true, true, false, false, false, false,
       ConnectButtonAction::Retry, true},
  };
  for (const auto& c : cases) {
    const ConnectButton b = DecideConnectButton(c.failed, c.actionIsDisconnect, c.confirming,
                                                c.outOfBalance, c.transitional,
                                                c.watchdogFired);
    Check(b.action == c.action,
          std::string(c.what) + ": action " + Name(b.action) + ", want " + Name(c.action));
    Check(b.enabled == c.enabled, std::string(c.what) + ": enabled " +
                                      (b.enabled ? "true" : "false") + ", want " +
                                      (c.enabled ? "true" : "false"));
  }
}

// (b) no automatic disconnect, however long the gate holds
void TestNoAutoDisconnect() {
  GateNoticeTracker tracker;
  FakeSinks sinks;
  for (int push = 0; push < 1000; ++push) {
    ReactToBalancePush(tracker, true, false, false, sinks);
  }
  Check(sinks.disconnects == 0,
        "no auto-disconnect: 1000 out-of-balance pushes disconnected " +
            std::to_string(sinks.disconnects) + " times, want 0");
  // the gate clearing and re-entering does not disconnect either
  ReactToBalancePush(tracker, false, false, false, sinks);
  ReactToBalancePush(tracker, true, false, false, sinks);
  ReactToBalancePush(tracker, true, true, false, sinks);
  ReactToBalancePush(tracker, true, false, true, sinks);
  Check(sinks.disconnects == 0, "no auto-disconnect: across episodes, plan and poll changes");
}

// (c) the notice posts once per entry, re-arms after the episode ends, and
// never for a supporter or during a confirmation poll
void TestNotice() {
  {
    GateNoticeTracker tracker;
    FakeSinks sinks;
    ReactToBalancePush(tracker, true, false, false, sinks);
    Check(sinks.notices == 1, "notice: posted on entering the gate");
    for (int push = 0; push < 100; ++push) ReactToBalancePush(tracker, true, false, false, sinks);
    Check(sinks.notices == 1, "notice: never repeated within the episode");
    // a poll or a plan flip within the episode does not re-arm it
    ReactToBalancePush(tracker, true, false, true, sinks);
    ReactToBalancePush(tracker, true, false, false, sinks);
    ReactToBalancePush(tracker, true, true, false, sinks);
    ReactToBalancePush(tracker, true, false, false, sinks);
    Check(sinks.notices == 1, "notice: poll or plan changes within the episode do not repeat it");
    ReactToBalancePush(tracker, false, false, false, sinks);
    Check(sinks.notices == 1, "notice: not posted when the episode ends");
    ReactToBalancePush(tracker, true, false, false, sinks);
    Check(sinks.notices == 2, "notice: re-armed by the end of the episode");
  }
  {
    GateNoticeTracker tracker;
    FakeSinks sinks;
    for (int push = 0; push < 10; ++push) ReactToBalancePush(tracker, true, true, false, sinks);
    Check(sinks.notices == 0, "notice: never for a supporter");
    for (int push = 0; push < 10; ++push) ReactToBalancePush(tracker, true, false, true, sinks);
    Check(sinks.notices == 0, "notice: never during a confirmation poll");
    ReactToBalancePush(tracker, true, false, false, sinks);
    Check(sinks.notices == 1, "notice: posted once the gate holds within the same episode");
  }
  {
    GateNoticeTracker tracker;
    FakeSinks sinks;
    ReactToBalancePush(tracker, false, false, false, sinks);
    ReactToBalancePush(tracker, false, true, true, sinks);
    Check(sinks.notices == 0, "notice: never for a funded account");
  }
}

// (d) start connect: the tray's Connect out of balance starts nothing and
// shows the upgrade path. It used to call ConnectBestAvailable regardless.
void TestTrayConnectBlockedOutOfBalance() {
  {
    FakeSinks sinks;
    RouteConnectGesture(/*actionIsDisconnect=*/false, OutOfBalance(true, false, false), sinks);
    Check(sinks.connects == 0, "tray: connect out of balance started the tunnel " +
                                   std::to_string(sinks.connects) + " times, want 0");
    Check(sinks.upgrades == 1, "tray: connect out of balance shows the upgrade path: " +
                                   std::to_string(sinks.upgrades) + ", want 1");
    Check(sinks.disconnects == 0, "tray: a blocked connect does not disconnect");
  }
  struct Case {
    const char* what;
    bool actionIsDisconnect, insufficient, supporter, confirming;
    ConnectGestureAction action;
  };
  const Case cases[] = {
      {"gesture: out of balance, disconnected: upgrade", false, true, false, false,
       ConnectGestureAction::Upgrade},
      {"gesture: out of balance, connected: disconnect still works", true, true, false, false,
       ConnectGestureAction::Disconnect},
      {"gesture: supporter: connect", false, true, true, false, ConnectGestureAction::Connect},
      {"gesture: confirmation poll: connect", false, true, false, true,
       ConnectGestureAction::Connect},
      {"gesture: funded: connect", false, false, false, false, ConnectGestureAction::Connect},
      {"gesture: funded, connected: disconnect", true, false, false, false,
       ConnectGestureAction::Disconnect},
  };
  for (const auto& c : cases) {
    const ConnectGestureAction a = DecideConnectGesture(
        c.actionIsDisconnect, OutOfBalance(c.insufficient, c.supporter, c.confirming));
    Check(a == c.action, c.what);
  }
}

// (e) start connect: the connect-only entry points (location and peer rows,
// the connect button) out of balance record no session request
void TestStartConnectBlockedOutOfBalance() {
  FakeSinks sinks;
  FakeHost host{sinks};
  host.outOfBalance = OutOfBalance(true, false, false);
  host.ConnectFromRow();
  Check(host.sessionRequests == 0, "row: connect out of balance requested a session " +
                                       std::to_string(host.sessionRequests) + " times, want 0");
  Check(!host.live, "row: connect out of balance started the tunnel");
  Check(sinks.upgrades == 1, "row: a blocked connect shows the upgrade path");
  host.outOfBalance = OutOfBalance(true, true, false);
  host.ConnectFromRow();
  Check(host.sessionRequests == 1 && host.live, "row: a supporter connects");
}

// (f) already connected: running out of balance never drops the session,
// a reattach is not gated, and only the user's Disconnect ends it
void TestAlreadyConnectedKept() {
  FakeSinks sinks;
  FakeHost host{sinks};
  host.ConnectFromRow();  // funded: connects
  Check(host.live, "connected: a funded connect starts");
  // the balance runs out while connected
  host.outOfBalance = true;
  GateNoticeTracker tracker;
  for (int push = 0; push < 1000; ++push) ReactToBalancePush(tracker, true, false, false, sinks);
  Check(sinks.disconnects == 0 && host.live,
        "connected: running out of balance dropped the session " +
            std::to_string(sinks.disconnects) + " times, want 0");
  // the app relaunches into the live session: a reattach, never blocked
  const int before = host.sessionRequests;
  host.EnsureSession();
  Check(host.sessionRequests == before + 1 && sinks.upgrades == 0,
        "connected: a reattach out of balance is not gated");
  // the tray offers Disconnect for the live session, not the upgrade path
  RouteConnectGesture(/*actionIsDisconnect=*/true, true, sinks);
  Check(sinks.disconnects == 1 && sinks.upgrades == 0,
        "connected: the user's Disconnect works out of balance");
}

}  // namespace

int main() {
  TestGate();
  TestConnectButton();
  TestNoAutoDisconnect();
  TestNotice();
  TestTrayConnectBlockedOutOfBalance();
  TestStartConnectBlockedOutOfBalance();
  TestAlreadyConnectedKept();
  std::cout << (gCases - gFailures) << "/" << gCases << " balance gate checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
