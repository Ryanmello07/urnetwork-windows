// Executable spec for a tap's state held until the pass that ran it has
// published (App/TapHold.h, owner report 2026-10-16): a connect tap shows
// connecting and a disconnect tap shows disconnected at once; a push read
// before the session worker ran the tap cannot flip the view back; the push
// whose read began after the tap settled replaces it with what the tap left;
// and a tap replaced, cancelled or dropped never holds the view forever. Run
// on any host with a C++20 compiler:
//
//   c++ -std=c++20 -I ../src/App tap-hold-tests.cpp -o /tmp/tap-hold && /tmp/tap-hold
//
// The host is modeled as SdkHost runs it: RequestSession numbers the gesture it
// records in the one last-request-wins slot and tells the views at once; the
// worker takes the slot's request into its pass and ends it before the pass's
// final publish; ReadStats stamps each snapshot with SettledThrough when its
// read begins; a re-clicked row, a sign-out and a quit empty the slot. Those
// lines are pinned in SdkHost.cpp, AppController.cpp and ConnectPage.cpp by
// tests/tap_hold_test.go. Nothing here waits on a clock or a thread.
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "TapHold.h"

namespace taphold = urnw::taphold;
using taphold::Hold;
using taphold::Kind;

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

// What a view shows, and the sdk's statuses a push carries.
enum class Status { Disconnected, Connecting, Connected };

const char* Name(Status s) {
  switch (s) {
    case Status::Disconnected: return "disconnected";
    case Status::Connecting: return "connecting";
    case Status::Connected: return "connected";
  }
  return "?";
}

// A stats snapshot: the status the read saw and where the gestures had settled
// when it began.
struct Snapshot {
  Status status = Status::Disconnected;
  std::uint64_t settled = 0;
};

// The session slot and worker, as SdkHost keeps them.
struct Host {
  std::uint64_t issued = 0;
  bool pendingRequested = false;
  std::uint64_t pendingGesture = 0;  // 0: an ensure or a reconcile
  std::uint64_t inFlight = 0;
  // the sdk's status, as the pass that runs a gesture leaves it
  Status sdk = Status::Disconnected;
  Status pendingStart = Status::Disconnected;
  Status inFlightStart = Status::Disconnected;

  // RequestSession: the newest request replaces the slot's (last wins); a
  // gesture gets the next number. Returns it, 0 for no gesture.
  std::uint64_t Request(bool gesture, Status start) {
    pendingRequested = true;
    pendingGesture = gesture ? ++issued : 0;
    pendingStart = start;
    return pendingGesture;
  }

  // The worker takes the slot's request into its pass.
  void Take() {
    inFlight = pendingRequested ? pendingGesture : 0;
    inFlightStart = pendingStart;
    pendingRequested = false;
    pendingGesture = 0;
  }

  // The pass ran its sdk call and ends, before its final publish.
  void EndPass() {
    sdk = inFlightStart;
    inFlight = 0;
  }

  // CancelPendingRowConnect, Logout's or Quit's clear of the slot.
  void ClearSlot() {
    pendingRequested = false;
    pendingGesture = 0;
  }

  // ReadStats: the stamp first, then the status.
  Snapshot Read() const {
    return Snapshot{.status = sdk,
                    .settled = taphold::SettledThrough(issued, pendingRequested ? pendingGesture : 0,
                                                       inFlight)};
  }
};

// The connect page and the tray, as they apply holds and pushes.
struct View {
  std::optional<Hold> hold;
  bool startWaiting = false;
  Status reported = Status::Disconnected;

  void Tap(std::uint64_t gesture, Kind kind) {
    if (gesture == 0) return;
    hold = taphold::AfterTap(hold, Hold{.gesture = gesture, .kind = kind});
  }

  void Push(const Snapshot& snapshot) {
    reported = snapshot.status;
    hold = taphold::AfterSnapshot(hold, snapshot.settled);
  }

  Status Shown() const {
    return taphold::Shown(startWaiting, hold, reported, Status::Connecting, Status::Disconnected);
  }
};

void TestAConnectTapHoldsAgainstAnOlderPush() {
  Host host;
  View view;
  const Snapshot before = host.Read();  // read before the tap
  view.Tap(host.Request(true, Status::Connecting), Kind::Connect);
  Check(view.Shown() == Status::Connecting, "a connect tap shows connecting at once");
  view.Push(before);
  Check(view.Shown() == Status::Connecting,
        std::string("a push read before the tap cannot flip it back: shows ") +
            Name(view.Shown()));
  host.Take();
  view.Push(host.Read());  // read while the pass runs
  Check(view.Shown() == Status::Connecting,
        std::string("a push read while the pass runs keeps it: shows ") + Name(view.Shown()));
  host.EndPass();
  host.sdk = Status::Connected;
  view.Push(host.Read());  // the pass's final publish, and later
  Check(view.Shown() == Status::Connected,
        std::string("the push read after the pass replaces it with what the tap left: shows ") +
            Name(view.Shown()));
  Check(!view.hold, "and the hold is over");
}

void TestADisconnectTapShowsAtOnceAndHolds() {
  Host host;
  host.sdk = Status::Connected;
  View view;
  view.Push(host.Read());
  Check(view.Shown() == Status::Connected, "connected before the tap");
  view.Tap(host.Request(true, Status::Disconnected), Kind::Disconnect);
  Check(view.Shown() == Status::Disconnected,
        std::string("a disconnect tap shows disconnected at once: shows ") + Name(view.Shown()));
  view.Push(host.Read());  // a push read before the worker ran it says connected
  Check(view.Shown() == Status::Disconnected,
        std::string("an older connected push cannot flip it back: shows ") + Name(view.Shown()));
  host.Take();
  host.EndPass();
  view.Push(host.Read());
  Check(view.Shown() == Status::Disconnected && !view.hold,
        "the pass's publish ends the hold on its own disconnected status");
}

void TestTheNewerTapWins() {
  // the Retry contract: disconnect, then connect; the slot keeps the connect
  Host host;
  host.sdk = Status::Connecting;
  View view;
  view.Tap(host.Request(true, Status::Disconnected), Kind::Disconnect);
  view.Tap(host.Request(true, Status::Connecting), Kind::Connect);
  Check(view.Shown() == Status::Connecting, "the newer tap's state shows");
  view.Push(host.Read());
  Check(view.hold.has_value(), "the replaced tap settled, the newer one still holds");
  host.Take();
  host.EndPass();
  view.Push(host.Read());
  Check(!view.hold && view.Shown() == Status::Connecting,
        "the connect's pass ends the hold with what it left");
  // a late notice of an older tap never replaces a newer hold
  std::optional<Hold> hold = Hold{.gesture = 5, .kind = Kind::Connect};
  hold = taphold::AfterTap(hold, Hold{.gesture = 4, .kind = Kind::Disconnect});
  Check(hold && hold->gesture == 5 && hold->kind == Kind::Connect,
        "an older tap's notice does not replace a newer hold");
}

void TestAPassInFlightHoldsANewerPendingTap() {
  Host host;
  View view;
  view.Tap(host.Request(true, Status::Connecting), Kind::Connect);
  host.Take();  // gesture 1 runs
  view.Tap(host.Request(true, Status::Disconnected), Kind::Disconnect);  // 2 waits
  host.EndPass();
  host.sdk = Status::Connecting;
  view.Push(host.Read());  // after 1's pass, 2 still waits
  Check(view.hold.has_value() && view.Shown() == Status::Disconnected,
        std::string("the tap still waiting holds past the older pass: shows ") +
            Name(view.Shown()));
  host.Take();
  host.EndPass();
  view.Push(host.Read());
  Check(!view.hold && view.Shown() == Status::Disconnected, "its own pass ends it");
}

void TestATapThatGoesWithoutRunningNeverHoldsForever() {
  // a re-clicked row cancels the settling one; a sign-out or a quit empties
  // the slot: the next read ends the hold
  for (const char* why : {"a cancelled row click", "a sign-out", "a quit"}) {
    Host host;
    host.sdk = Status::Connected;
    View view;
    view.Tap(host.Request(true, Status::Connecting), Kind::Connect);
    host.ClearSlot();
    view.Push(host.Read());
    Check(!view.hold && view.Shown() == Status::Connected,
          std::string(why) + ": the next push ends the hold, nothing waits forever");
  }
}

void TestAnEnsureHoldsNothing() {
  Host host;
  View view;
  view.Tap(host.Request(false, Status::Disconnected), Kind::Connect);
  Check(!view.hold, "an ensure or a reconcile is no tap: nothing holds");
  // and one waiting in the slot does not hold back a tap's release
  view.Tap(host.Request(true, Status::Connecting), Kind::Connect);
  host.Take();
  host.EndPass();
  host.Request(false, Status::Disconnected);
  view.Push(host.Read());
  Check(!view.hold, "an ensure queued after the tap's pass does not keep the hold");
}

void TestAnAdmittedStartHandsOverWithNoStepBack() {
  Host host;
  View view;
  view.startWaiting = true;  // a start waiting on its balance shows connecting
  Check(view.Shown() == Status::Connecting, "waiting on the balance shows connecting");
  // admitted: its connect is recorded inside the gesture, then the wait ends
  view.Tap(host.Request(true, Status::Connecting), Kind::Connect);
  view.startWaiting = false;
  Check(view.Shown() == Status::Connecting,
        std::string("the admitted start keeps connecting into its connect: shows ") +
            Name(view.Shown()));
  view.Push(Snapshot{.status = Status::Disconnected, .settled = 0});
  Check(view.Shown() == Status::Connecting, "and an older push cannot step it back");
}

void TestSettledThroughIsTheOldestOutstandingGesture() {
  Check(taphold::SettledThrough(0, 0, 0) == 0, "no gestures: none settled");
  Check(taphold::SettledThrough(7, 0, 0) == 7, "nothing outstanding: all settled");
  Check(taphold::SettledThrough(7, 7, 0) == 6, "the one waiting is not");
  Check(taphold::SettledThrough(7, 0, 6) == 5, "nor the one running");
  Check(taphold::SettledThrough(7, 7, 6) == 5, "the older of the two bounds it");
  Check(taphold::Releases(Hold{.gesture = 5}, 5), "a read at the tap's number ends it");
  Check(!taphold::Releases(Hold{.gesture = 5}, 4), "a read before it does not");
}

}  // namespace

int main() {
  TestAConnectTapHoldsAgainstAnOlderPush();
  TestADisconnectTapShowsAtOnceAndHolds();
  TestTheNewerTapWins();
  TestAPassInFlightHoldsANewerPendingTap();
  TestATapThatGoesWithoutRunningNeverHoldsForever();
  TestAnEnsureHoldsNothing();
  TestAnAdmittedStartHandsOverWithNoStepBack();
  TestSettledThroughIsTheOldestOutstandingGesture();
  std::cout << (gCases - gFailures) << "/" << gCases << " tap hold checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
