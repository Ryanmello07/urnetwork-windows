// Executable spec for the tunnel watchdog's settings
// (Service/TunnelWatchdogSettings.h) and the freeze rule that reads two of them
// (Service/NetworkChangeNotify.h FreezeResponseFor): the gaps the evaluator
// meets, at the defaults the service runs with, and settings other than the
// defaults honoured by the rule and by a CaptureReadiness built from the
// watchdog's capture part, as TunnelController builds every session's. Every
// case reads the defaults from the settings rather than restating them;
// tests/network_switch_wiring_test.go pins the values themselves.
//
//   c++ -std=c++20 -Wall -Wextra -Werror -pthread -I ../src/Service watchdog-settings-tests.cpp -o /tmp/watchdog-settings-tests && /tmp/watchdog-settings-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <iostream>
#include <string>

#include "TunnelWatchdogSettings.h"

using namespace urnw;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    // flushed: a negative control may end the process before main returns
    std::cout << "  FAIL " << what << std::endl;
  }
}

// The evaluator's two bars (FreezeResponseFor) at the default settings: a gap
// short of the freeze bar is nothing, a stall rebases the failsafe without
// telling the device, and a gap long enough to have been a sleep does both.
void TestFreezeResponse() {
  const TunnelWatchdogSettings defaults;
  const int64_t frozen = defaults.frozenMillis;
  const int64_t sleep = defaults.freezeNetworkChangeMillis;
  const auto at = [&](int64_t gap) { return FreezeResponseFor(gap, frozen, sleep); };
  Check(!at(frozen - 100).rebase && !at(frozen - 100).networkChanged,
        "freeze: a 4.9 s gap is scheduling, neither rebased nor a network change");
  Check(at(frozen).rebase && !at(frozen).networkChanged,
        "freeze: a 5 s stall rebases and is not a network change");
  Check(at(sleep - 100).rebase && !at(sleep - 100).networkChanged,
        "freeze: a 14.9 s stall rebases and is not a network change");
  Check(at(sleep).rebase && at(sleep).networkChanged,
        "freeze: a 15 s gap rebases and is a network change");
  Check(at(60000).rebase && at(60000).networkChanged,
        "freeze: a 60 s sleep rebases and is a network change");
}

// A bar other than the default is the one the rule keeps.
void TestFreezeResponseHonoursItsSettings() {
  TunnelWatchdogSettings longer;
  longer.freezeNetworkChangeMillis = 2 * TunnelWatchdogSettings{}.freezeNetworkChangeMillis;
  const FreezeResponse held = FreezeResponseFor(TunnelWatchdogSettings{}.freezeNetworkChangeMillis,
                                                longer.frozenMillis,
                                                longer.freezeNetworkChangeMillis);
  Check(held.rebase && !held.networkChanged,
        "settings: a longer network-change bar is the one the rule keeps");
  TunnelWatchdogSettings eager;
  eager.frozenMillis = 1000;
  Check(FreezeResponseFor(1000, eager.frozenMillis, eager.freezeNetworkChangeMillis).rebase &&
            !FreezeResponseFor(1000, TunnelWatchdogSettings{}.frozenMillis,
                               TunnelWatchdogSettings{}.freezeNetworkChangeMillis)
                 .rebase,
        "settings: a shorter freeze bar is the one the rule keeps");
}

// The parts the watchdog is built from carry their own components' defaults:
// one default, one place.
void TestDefaultsAreTheComponentsOwn() {
  const TunnelWatchdogSettings watchdog;
  Check(watchdog.networkNotify.windowMillis == NetworkNotifySettings{}.windowMillis,
        "defaults: the watchdog's window is the notification default");
  const CaptureReadinessSettings capture;
  Check(watchdog.capture.probeAgeResolutionMillis == capture.probeAgeResolutionMillis &&
            watchdog.capture.freshnessMillis == capture.freshnessMillis &&
            watchdog.capture.sampleStallMillis == capture.sampleStallMillis,
        "defaults: the watchdog's capture timing is the readiness default");
}

// A readiness built from the watchdog's capture part, as the controller builds
// each session's, keeps that part's timing.
void TestReadinessTakesTheWatchdogsCaptureTiming() {
  TunnelWatchdogSettings watchdog;
  watchdog.capture.freshnessMillis = 2000;
  int64_t now = 1000;
  CaptureReadiness readiness([&] { return now; }, watchdog.capture);
  const CaptureWindow window{.generation = 1, .added = 1};
  readiness.CompleteSample(readiness.BeginSample(), window, window, 1);
  Check(readiness.Ready().has_value(), "capture: a fresh sample qualifies");
  now += watchdog.capture.freshnessMillis + 500;
  Check(now - 1000 < CaptureReadinessSettings{}.freshnessMillis && !readiness.Ready(),
        "capture: a readiness built from the watchdog's capture timing keeps it");
}

}  // namespace

int main() {
  TestFreezeResponse();
  TestFreezeResponseHonoursItsSettings();
  TestDefaultsAreTheComponentsOwn();
  TestReadinessTakesTheWatchdogsCaptureTiming();
  std::cout << (gCases - gFailures) << "/" << gCases << " watchdog settings checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
