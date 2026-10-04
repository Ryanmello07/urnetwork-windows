// Executable spec for the insufficient-balance gate (Common/BalanceGate.h):
// the explicit connect button keeps a working Disconnect while the account is
// out of balance, the app never disconnects on its own, and the notice posts
// once per out-of-balance episode, never for a supporter or during a
// confirmation poll. A connect gesture from any entry point (the tray's
// Connect, a location row, the connect button) starts nothing out of balance
// and shows the upgrade path, including after the user's Disconnect resets the
// contract status (OutOfBalanceLatch); a session already connected is never
// dropped. The first connect after a launch on an empty account is blocked
// too, but only on a fresh balance: a stale one is fetched first, and a failed
// fetch never blocks. Run against the SAME header the app compiles, on any
// host with a C++20 compiler. No clocks: the time is an explicit input.
//
//   c++ -std=c++20 -I ../src/Common balance-gate-tests.cpp -o /tmp/balance-gate-tests && /tmp/balance-gate-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <functional>
#include <iostream>
#include <optional>
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

constexpr long long kNow = 10'000'000;

// A balance read at `at` with `available` bytes left and nothing held.
AccountBalance Read(long long available, long long at = kNow, long long open = 0,
                    bool pro = false) {
  AccountBalance b;
  b.known = true;
  b.pro = pro;
  b.availableBytes = available;
  b.openTransferBytes = open;
  b.fetchedAtMs = at;
  return b;
}

// A funded account read just now: what a gate with no balance concern sees.
StartConnectFacts Funded() {
  StartConnectFacts f;
  f.balance = Read(1'000'000);
  f.nowMs = kNow;
  return f;
}

// A stand-in for SdkHost's connect entry points: each asks AdmitStartConnect
// before it records a session request, exactly as the app's do, and a
// FetchBalance holds the gesture until the test settles the fetch.
struct FakeHost {
  FakeSinks& sinks;
  StartConnectFacts facts = Funded();
  bool live = false;  // a session is up
  int sessionRequests = 0;
  int fetches = 0;
  std::function<void()> pending = nullptr;  // the gesture, asked again once the fetch settles
  void ConnectFromRow() {
    struct Gate {
      FakeHost& host;
      void Upgrade() { host.sinks.Upgrade(); }
      void FetchBalance() {
        ++host.fetches;
        host.pending = [h = &host] { h->ConnectFromRow(); };
      }
    } gate{*this};
    if (!AdmitStartConnect(facts, gate)) return;
    ++sessionRequests;
    live = true;
  }
  // The fetch settles now: a read balance, or nullopt when it failed or timed
  // out. Then the held gesture is asked again.
  void SettleFetch(std::optional<AccountBalance> read) {
    if (read) facts.balance = *read;
    facts.fetchSettled = true;
    facts.fetchSettledAtMs = facts.nowMs;
    auto again = std::move(pending);
    pending = nullptr;
    if (again) again();
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
  host.facts.latched = true;
  host.ConnectFromRow();
  Check(host.sessionRequests == 0, "row: connect out of balance requested a session " +
                                       std::to_string(host.sessionRequests) + " times, want 0");
  Check(!host.live, "row: connect out of balance started the tunnel");
  Check(sinks.upgrades == 1, "row: a blocked connect shows the upgrade path");
  host.facts.supporter = true;
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
  host.facts.latched = true;
  host.facts.balance = Read(0);
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

// (g) held out of balance -> the user's Disconnect (the SDK resets the
// contract status to an empty one) -> tray Connect. The gate read the raw
// contract status, so the Connect started the tunnel again.
void TestTrayConnectAfterDisconnectStaysBlocked() {
  using Obs = OutOfBalanceLatch::Observation;
  OutOfBalanceLatch latch;
  FakeSinks sinks;
  // connected and funded, providers attached
  latch.Observe(Obs{false, true, true, 1000});
  // the balance runs out: traffic is held
  latch.Observe(Obs{true, false, true, 0});
  // the user's Disconnect: the contract status is reset to empty
  latch.Observe(Obs{false, false, true, 0});
  RouteConnectGesture(/*actionIsDisconnect=*/false,
                      OutOfBalance(latch.InsufficientBalance(), false, false), sinks);
  Check(sinks.connects == 0, "latch: tray Connect after Disconnect out of balance started the "
                             "tunnel " + std::to_string(sinks.connects) + " times, want 0");
  Check(sinks.upgrades == 1, "latch: tray Connect after Disconnect shows the upgrade path");
  // before the balance has ever been fetched, the reset still keeps the state
  OutOfBalanceLatch unknown;
  unknown.Observe(Obs{true, false, false, 0});
  unknown.Observe(Obs{false, false, false, 0});
  Check(unknown.InsufficientBalance(), "latch: kept across the reset with no balance read");
}

// (h) the latch ends only on evidence that the state is over
void TestOutOfBalanceLatchClears() {
  using Obs = OutOfBalanceLatch::Observation;
  {
    OutOfBalanceLatch latch;
    latch.Observe(Obs{false, false, true, 0});
    Check(!latch.InsufficientBalance(), "latch: a funded account never latches");
  }
  {
    OutOfBalanceLatch latch;
    latch.Observe(Obs{true, false, true, 100});
    latch.Observe(Obs{false, false, true, 50});   // fell further
    latch.Observe(Obs{false, false, true, 100});  // rose above the low (50)
    Check(!latch.InsufficientBalance(), "latch: cleared by a balance above the lowest seen");
  }
  {
    OutOfBalanceLatch latch;
    latch.Observe(Obs{true, false, true, 100});
    latch.Observe(Obs{false, false, true, 100});
    latch.Observe(Obs{false, false, false, 0});  // an unknown balance is no evidence
    Check(latch.InsufficientBalance(), "latch: kept by an unchanged or unknown balance");
    latch.Observe(Obs{false, true, true, 100});
    Check(!latch.InsufficientBalance(), "latch: cleared by providers attached on a live session");
  }
  {
    OutOfBalanceLatch latch;
    latch.Observe(Obs{true, false, true, 0});
    latch.Observe(Obs{true, true, true, 0});  // still reported: providers do not clear it
    Check(latch.InsufficientBalance(), "latch: held while the contract status reports it");
    latch.Reset();  // sign-in or sign-out
    Check(!latch.InsufficientBalance(), "latch: cleared by a sign-in or sign-out");
  }
  {
    // a supporter is never gated, latched or not
    OutOfBalanceLatch latch;
    latch.Observe(Obs{true, false, true, 0});
    Check(!OutOfBalance(latch.InsufficientBalance(), true, false),
          "latch: a supporter is not gated");
  }
}

// (i) a fresh launch on an account that is already empty: no contract status
// exists before the first connect, so the latch is clear and only the
// subscription balance can tell. The connect used to start the tunnel.
void TestFreshStartEmptyAccountBlocked() {
  FakeSinks sinks;
  FakeHost host{sinks};
  host.facts.balance = Read(0, kNow - 1'000);
  host.ConnectFromRow();
  Check(host.sessionRequests == 0 && !host.live,
        "fresh start: connect on an empty account started the tunnel " +
            std::to_string(host.sessionRequests) + " times, want 0");
  Check(sinks.upgrades == 1, "fresh start: an empty account shows the upgrade path");
  Check(host.fetches == 0, "fresh start: a fresh balance is not fetched again");
}

// (j) only a fresh balance blocks; a stale one is fetched first
void TestStaleBalanceFetchedFirst() {
  {
    // stale and empty, the fetch fails: fail open
    FakeSinks sinks;
    FakeHost host{sinks};
    host.facts.balance = Read(0, kNow - kFreshBalanceMs - 1);
    host.ConnectFromRow();
    Check(host.fetches == 1 && host.sessionRequests == 0,
          "stale: a stale empty balance is fetched before deciding");
    host.SettleFetch(std::nullopt);
    Check(host.sessionRequests == 1 && sinks.upgrades == 0,
          "stale: a failed fetch does not block the connect");
    Check(host.fetches == 1, "stale: a settled fetch is not repeated");
  }
  {
    // stale and empty, the fetch finds a funded account: connects
    FakeSinks sinks;
    FakeHost host{sinks};
    host.facts.balance = Read(0, kNow - 30 * 60'000);
    host.ConnectFromRow();
    host.SettleFetch(Read(5'000));
    Check(host.sessionRequests == 1 && sinks.upgrades == 0,
          "stale: a funded user is not sent to upgrade on an old empty reading");
  }
  {
    // stale and funded, the fetch finds it empty: blocked
    FakeSinks sinks;
    FakeHost host{sinks};
    host.facts.balance = Read(5'000, kNow - kFreshBalanceMs - 1);
    host.ConnectFromRow();
    host.SettleFetch(Read(0));
    Check(host.sessionRequests == 0 && sinks.upgrades == 1,
          "stale: a fetch that reads an empty account blocks");
  }
  {
    // never read this session (launch before the first fetch lands)
    FakeSinks sinks;
    FakeHost host{sinks};
    host.facts.balance = AccountBalance{};
    host.ConnectFromRow();
    Check(host.fetches == 1 && host.sessionRequests == 0,
          "unknown: an unread balance is fetched before deciding");
    host.SettleFetch(Read(0));
    Check(host.sessionRequests == 0 && sinks.upgrades == 1,
          "unknown: the fetched empty balance blocks");
  }
  {
    // fresh and funded: connects at once
    FakeSinks sinks;
    FakeHost host{sinks};
    host.facts.balance = Read(5'000, kNow - kFreshBalanceMs);
    host.ConnectFromRow();
    Check(host.sessionRequests == 1 && host.fetches == 0 && sinks.upgrades == 0,
          "fresh: a funded balance read 60s ago connects without a fetch");
  }
  // a balance from the future (a clock reset) is not fresh
  {
    StartConnectFacts f = Funded();
    f.balance = Read(0, kNow + 1);
    Check(DecideStartConnect(f) == StartConnectStep::FetchBalance,
          "fresh: a reading stamped after now is fetched again");
  }
}

// (k) what an exhausted account is: nothing available and nothing held
void TestAccountBalanceExhausted() {
  Check(AccountBalanceExhausted(Read(0)), "exhausted: nothing available or held");
  Check(AccountBalanceExhausted(Read(-5)), "exhausted: a negative balance");
  Check(!AccountBalanceExhausted(Read(1)), "exhausted: one byte left is not exhausted");
  Check(!AccountBalanceExhausted(Read(0, kNow, 100)),
        "exhausted: bytes held in open contracts are not exhausted");
  Check(!AccountBalanceExhausted(Read(0, kNow, 0, true)), "exhausted: Pro never is");
  Check(!AccountBalanceExhausted(AccountBalance{}), "exhausted: an unknown balance never is");
  StartConnectFacts f = Funded();
  f.balance = Read(0);
  f.supporter = true;
  Check(DecideStartConnect(f) == StartConnectStep::Connect, "start: a supporter connects");
  f.supporter = false;
  f.confirming = true;
  Check(DecideStartConnect(f) == StartConnectStep::Connect,
        "start: a confirmation poll bridging a purchase connects");
  f.confirming = false;
  f.latched = true;
  f.balance = Read(5'000, kNow - kFreshBalanceMs - 1);
  Check(DecideStartConnect(f) == StartConnectStep::Upgrade,
        "start: the latch blocks without waiting for a fetch");
}

// (l) a session that is up when the balance reads empty is kept: the fresh
// balance only ever decides a start connect
void TestLiveSessionKeptOnEmptyBalance() {
  FakeSinks sinks;
  FakeHost host{sinks};
  host.ConnectFromRow();
  Check(host.live, "live: a funded connect starts");
  host.facts.balance = Read(0);
  GateNoticeTracker tracker;
  for (int push = 0; push < 100; ++push) ReactToBalancePush(tracker, true, false, false, sinks);
  host.EnsureSession();
  Check(host.live && sinks.disconnects == 0 && sinks.upgrades == 0 && host.sessionRequests == 2,
        "live: an empty balance dropped or gated the live session");
  RouteConnectGesture(/*actionIsDisconnect=*/true, true, sinks);
  Check(sinks.disconnects == 1, "live: the user's Disconnect works on an empty balance");
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
  TestTrayConnectAfterDisconnectStaysBlocked();
  TestOutOfBalanceLatchClears();
  TestFreshStartEmptyAccountBlocked();
  TestStaleBalanceFetchedFirst();
  TestAccountBalanceExhausted();
  TestLiveSessionKeptOnEmptyBalance();
  std::cout << (gCases - gFailures) << "/" << gCases << " balance gate checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
