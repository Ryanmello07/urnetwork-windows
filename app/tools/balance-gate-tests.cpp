// Executable spec for the insufficient-balance gate (Common/BalanceGate.h):
// the explicit connect button keeps a working Disconnect while the account is
// out of balance, the app never disconnects on its own, and the notice posts
// once per out-of-balance episode, never for a supporter or during a
// confirmation poll. Run against the SAME header the app compiles, on any host
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

// Counts what the reaction asked for.
struct FakeSinks {
  int notices = 0;
  int disconnects = 0;
  void Notice() { ++notices; }
  void Disconnect() { ++disconnects; }
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

}  // namespace

int main() {
  TestGate();
  TestConnectButton();
  TestNoAutoDisconnect();
  TestNotice();
  std::cout << (gCases - gFailures) << "/" << gCases << " balance gate checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
