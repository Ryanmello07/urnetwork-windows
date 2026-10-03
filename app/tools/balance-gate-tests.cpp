// Executable spec for the insufficient-balance gate (Common/BalanceGate.h):
// the connect button keeps a working Disconnect while the account is out of
// balance, and the app clears the connect request after the grace period
// unless the account is a supporter, a confirmation poll is running, or the
// kill switch is on. Run against the SAME header the app compiles, on any host
// with a C++20 compiler.
//
//   c++ -std=c++20 -I ../src/Common balance-gate-tests.cpp \
//       -o /tmp/balance-gate-tests && /tmp/balance-gate-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <chrono>
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

void TestGate() {
  Check(OutOfBalance(true, false, false), "insufficient, basic, not polling is gated");
  Check(!OutOfBalance(true, true, false), "a supporter is never gated");
  Check(!OutOfBalance(true, false, true), "a confirmation poll suspends the gate");
  Check(!OutOfBalance(false, false, false), "a funded account is not gated");
}

void TestConnectButton() {
  struct Case {
    const char* what;
    bool failed, actionIsDisconnect, confirming, outOfBalance, transitional, watchdogFired;
    ConnectButtonAction action;
    bool enabled;
  };
  const Case cases[] = {
      // the reported dead end: out of balance with a session up
      {"out of balance, connected: disconnect stays usable", false, true, false, true,
       false, false, ConnectButtonAction::Disconnect, true},
      {"out of balance, connecting: disconnect stays usable", false, true, false, true,
       true, false, ConnectButtonAction::Disconnect, true},
      {"out of balance, failed: disconnect, not a retry that cannot succeed", true, true,
       false, true, false, false, ConnectButtonAction::Disconnect, true},
      {"out of balance, disconnected: connect stays blocked", false, false, false, true,
       false, false, ConnectButtonAction::Connect, false},
      {"confirming, connected: disconnect stays usable", false, true, true, false, false,
       false, ConnectButtonAction::Disconnect, true},
      {"confirming, disconnected: connect stays blocked", false, false, true, false, false,
       false, ConnectButtonAction::Connect, false},
      {"confirming, failed: retry stays blocked", true, true, true, false, false, false,
       ConnectButtonAction::Retry, false},
      // unchanged outside the gate
      {"funded, disconnected: connect", false, false, false, false, false, false,
       ConnectButtonAction::Connect, true},
      {"funded, connected: disconnect", false, true, false, false, false, false,
       ConnectButtonAction::Disconnect, true},
      {"funded, connecting: guarded until the watchdog", false, true, false, false, true,
       false, ConnectButtonAction::Disconnect, false},
      {"funded, connecting past the watchdog: enabled", false, true, false, false, true,
       true, ConnectButtonAction::Disconnect, true},
      {"funded, failed: retry", true, true, false, false, false, false,
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

void TestAutoDisconnect() {
  using std::chrono::milliseconds;
  using std::chrono::seconds;
  const auto grace = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      kAutoDisconnectGrace);
  const auto justShort = grace - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                     milliseconds(1));
  Check(10 <= kAutoDisconnectGrace.count() && kAutoDisconnectGrace.count() <= 30,
        "the grace rides out a transient latch without stranding the user");
  Check(AutoDisconnectDue(true, true, false, grace), "due once the grace has elapsed");
  Check(!AutoDisconnectDue(true, true, false, justShort), "not due before the grace");
  Check(!AutoDisconnectDue(true, true, true, grace + seconds(60)),
        "kill switch on: keep capture, never auto-disconnect");
  Check(!AutoDisconnectDue(OutOfBalance(true, true, false), true, false, grace),
        "a supporter is never auto-disconnected");
  Check(!AutoDisconnectDue(OutOfBalance(true, false, true), true, false, grace),
        "never while a confirmation poll is running");
  Check(!AutoDisconnectDue(true, false, false, grace),
        "nothing to disconnect: no action");
  Check(!AutoDisconnectDue(false, true, false, grace), "a funded account: no action");
}

}  // namespace

int main() {
  TestGate();
  TestConnectButton();
  TestAutoDisconnect();
  std::cout << (gCases - gFailures) << "/" << gCases << " balance gate checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
