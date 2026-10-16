// Executable spec for the connection health while the window is hidden
// (Common/ConnectionHealth.h, owner report 2026-10-16; Apple 2f00a56c and
// 64cd2c1b): with no presented controller ReadStats reports the device's
// location alone as DESTINATION_SET, and a set location over a tunnel that is
// up reads as connected, as connecting while the tunnel is still preparing;
// the window's evidence holds across a hide, the SDK's failed verdict stands
// while it lasts, and the window's own feed takes over when it shows again.
// Run on any host with a C++20 compiler:
//
//   c++ -std=c++20 -I ../src/Common hidden-tunnel-health-tests.cpp -o /tmp/hth && /tmp/hth
//
// ReadStats' hidden branch and its fold into the tracker are pinned by
// tests/hidden_tunnel_health_test.go. The tray reads the health this yields
// (AppController::OnStats, TrayConnectionState.h). The clock is a plain
// integer.
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <iostream>
#include <string>

#include "ConnectionHealth.h"

namespace health = urnw::health;
using health::Activity;
using health::CaptureSignals;
using health::Signals;
using health::State;
using health::Tracker;

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

std::string Name(State state) { return health::ToString(state); }

// A hidden snapshot: service up, no grid feed, the location set or not.
Signals Hidden(bool locationSet, bool windowFailed = false) {
  Signals s;
  s.serviceConnected = true;
  s.activity = locationSet ? Activity::Connecting : Activity::Inactive;
  s.gridKnown = false;
  s.statusFromLocation = locationSet;
  s.windowFailed = windowFailed;
  return s;
}

// A presented snapshot with a live grid.
Signals Shown(Activity activity, int64_t window, int64_t proven) {
  Signals s;
  s.serviceConnected = true;
  s.activity = activity;
  s.gridKnown = true;
  s.windowSize = window;
  s.provenCount = proven;
  return s;
}

constexpr CaptureSignals kTunnelUp{.serviceConnected = true, .preparing = false, .active = true,
                                   .failed = false};
constexpr CaptureSignals kPreparing{.serviceConnected = true, .preparing = true, .active = false,
                                    .failed = false};
constexpr CaptureSignals kTunnelDown{.serviceConnected = true, .preparing = false,
                                     .active = false, .failed = false};

void TestAReattachOverAnUpTunnelReadsConnected() {
  // an autostart, or a recovery, attaches to a session whose window never showed
  Tracker t;
  const State tracked = t.Update(Hidden(true), 0);
  Check(health::WithCapture(tracked, kTunnelUp) == State::Connected,
        "a location set while hidden reads connected once the tunnel is up: read " +
            Name(health::WithCapture(tracked, kTunnelUp)));
  Check(health::WithCapture(tracked, kPreparing) == State::Connecting,
        "and connecting while the tunnel is still preparing");
  Check(health::WithCapture(tracked, kTunnelDown) == State::Disconnected,
        "and not connected with the tunnel down");
}

void TestAConnectMadeWhileHiddenReadsConnectedOnceUp() {
  // the tray's Connect with the window hidden: a deliberate new attempt
  Tracker t;
  t.Update(Hidden(false), 0);
  t.NoteNewAttempt();
  const State tracked = t.Update(Hidden(true), 1000);
  Check(health::WithCapture(tracked, kPreparing) == State::Connecting,
        "a hidden connect reads connecting while its tunnel prepares");
  Check(health::WithCapture(t.Update(Hidden(true), 2000), kTunnelUp) == State::Connected,
        "and connected once the tunnel is up");
}

void TestAConnectInProgressWhenTheWindowHidReadsConnectedOnceUp() {
  Tracker t;
  t.Update(Shown(Activity::Connecting, 0, 0), 0);  // nothing proven yet
  Check(health::WithCapture(t.Update(Hidden(true), 1000), kTunnelUp) == State::Connected,
        "connect in the window, then hide: connected once the tunnel is up");
}

void TestTheWindowsEvidenceHoldsAcrossAHide() {
  Tracker t;
  t.Update(Shown(Activity::Active, 4, 0), 0);  // Evaluating
  Check(t.Update(Hidden(true), 1000) == State::Evaluating,
        "the window's last evidence-based claim holds while hidden");
  Tracker degraded;
  degraded.Update(Shown(Activity::Active, 4, 1), 0);
  degraded.Update(Shown(Activity::Active, 4, 0), 1000);
  degraded.Update(Shown(Activity::Active, 4, 0), 1000 + Tracker::kDegradeHoldMillis);
  Check(degraded.Update(Hidden(true), 20000) == State::Degraded,
        "a degraded connection stays degraded while hidden, not connected");
}

void TestAFailedAttemptStaysFailedWhileHidden() {
  Tracker t;
  Check(t.Update(Shown(Activity::Connecting, 0, 0), 0) == State::Connecting, "a connect begins");
  t.Update([] {
    Signals s = Shown(Activity::Connecting, 0, 0);
    s.windowFailed = true;
    return s;
  }(), 1000);
  const State hidden = t.Update(Hidden(true, /*windowFailed=*/true), 2000);
  Check(hidden == State::Failed,
        "the SDK's failed verdict stands while hidden, not connected: read " + Name(hidden));
  Check(t.Update(Hidden(true, /*windowFailed=*/false), 3000) == State::Connected,
        "once the SDK clears it, the set location reads as the connection again");
}

void TestNoLocationReadsDisconnected() {
  Tracker t;
  t.Update(Shown(Activity::Active, 4, 1), 0);
  Check(t.Update(Hidden(false), 1000) == State::Disconnected,
        "no location while hidden is disconnected");
}

void TestATransitionalStatusNotFromTheLocationStillConnects() {
  // the tracker's old rule for a status that is not the location's alone
  Tracker t;
  Signals s = Hidden(true);
  s.statusFromLocation = false;
  Check(t.Update(s, 0) == State::Connecting,
        "a transitional status no location stands for still reads connecting");
}

void TestTheWindowsFeedTakesOverWhenItShows() {
  Tracker t;
  t.Update(Hidden(true), 0);  // connected from the location
  Check(t.Update(Shown(Activity::Active, 3, 0), 1000) == State::Connected,
        "the feed opening onto an unproven window holds (grace), no flap");
  Check(t.Update(Shown(Activity::Active, 3, 0), 1000 + Tracker::kDegradeHoldMillis) ==
            State::Degraded,
        "a location claim the feed never proves becomes degraded, honestly");
  Tracker proven;
  proven.Update(Hidden(true), 0);
  Check(proven.Update(Shown(Activity::Active, 3, 1), 1000) == State::Connected,
        "and a proven window is connected on its own evidence");
}

}  // namespace

int main() {
  TestAReattachOverAnUpTunnelReadsConnected();
  TestAConnectMadeWhileHiddenReadsConnectedOnceUp();
  TestAConnectInProgressWhenTheWindowHidReadsConnectedOnceUp();
  TestTheWindowsEvidenceHoldsAcrossAHide();
  TestAFailedAttemptStaysFailedWhileHidden();
  TestNoLocationReadsDisconnected();
  TestATransitionalStatusNotFromTheLocationStillConnects();
  TestTheWindowsFeedTakesOverWhenItShows();
  std::cout << (gCases - gFailures) << "/" << gCases << " hidden tunnel health checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
