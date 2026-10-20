// Executable spec for telling a device the network moved
// (Service/NetworkChangeNotify.h): the coalescer's fixed window, and the
// NetworkChangeNotifier the provider-only device uses in place of the tunnel
// watchdog's sampler — one call per burst, a quality change dropped inside a
// network pass, a window that a flapping link cannot starve, no call once
// destroyed, a destructor that waits out a call already running (the device
// must outlive it), sinks that outlive the notifier, and a call that throws
// without ending the thread. The notifier runs on a test clock that moves only
// when a case moves it. After each move a case waits for the thread to have
// read the new time and gone back to sleep, and a call that must still be
// running is held on a barrier. No case sleeps or races the real 750 ms window,
// so a loaded host changes how long a run takes, never its verdict. Also the
// pure rules beside it: the watchdog sampler's sleep (a burst that begins
// mid-sleep ends it) and what counts as the network moving (EgressChangeFilter:
// one switch is one change, churn that moves nothing is none, a link lost under
// a retained index is one).
//
//   c++ -std=c++20 -pthread -I ../src/Service network-change-notify-tests.cpp -o /tmp/network-change-notify-tests && /tmp/network-change-notify-tests
//
// With --hold-call-until-destroyed it runs only the join case and holds the
// call until the destructor returns, which a destructor that waits never does:
// the negative control for one that does not (tests/network_change_notify_test.go).
//
// SPDX-License-Identifier: MPL-2.0

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include "NetworkChangeNotify.h"

using namespace urnw;

namespace {

// The defaults the service runs with, read from the settings rather than
// restated, so a case follows them when they move.
constexpr int64_t kWindow = NetworkNotifySettings{}.windowMillis;

std::mutex gMutex;
int gFailures = 0;
int gCases = 0;
// The step now running, for the hang guard, named as its failure reads.
std::condition_variable gStepped;
std::string gStep = "start";
int64_t gSteps = 0;
bool gFinished = false;

void Check(bool condition, const std::string& what) {
  std::scoped_lock lock(gMutex);
  ++gCases;
  if (!condition) {
    ++gFailures;
    // flushed: a negative control may end the process before main returns
    std::cout << "  FAIL " << what << std::endl;
  }
}

void Step(std::string step) {
  {
    std::scoped_lock lock(gMutex);
    gStep = std::move(step);
    ++gSteps;
  }
  gStepped.notify_all();
}

// Every wait in these cases is for an event, so a rule that breaks can show up
// as a wait that never ends. This names the step that did not finish and ends
// the run instead of leaving it hung. No verdict depends on it.
void HangGuard() {
  constexpr std::chrono::seconds kLimit{60};
  std::unique_lock lock(gMutex);
  while (!gFinished) {
    const int64_t steps = gSteps;
    if (!gStepped.wait_for(lock, kLimit, [&] { return gFinished || gSteps != steps; })) {
      std::cout << "  FAIL " << gStep << " (still waiting after " << kLimit.count() << " s)"
                << std::endl;
      std::_Exit(1);
    }
  }
}

// The notifier's time in these cases: a clock that stands still until a case
// moves it, and a sleep that only a wake ends. The sleep records the reading
// the thread found nothing due at, so a case waits for the thread to have read
// the time it set, by when every call due has returned, instead of sleeping
// past a window. Shared with the notifier, so a thread or a sink that outlives
// its case still finds it.
class TestClock {
 public:
  explicit TestClock(int64_t nowMillis) : nowMillis_(nowMillis) {}

  // What a notifier on `clock` is given: its readings, its sleep and a count of
  // its wakes.
  static NotifyClock For(const std::shared_ptr<TestClock>& clock) {
    return NotifyClock{
        .nowMillis = [clock] { return clock->Read(); },
        .sleep =
            [clock](std::unique_lock<std::mutex>& lock, std::condition_variable& wake,
                    int64_t nowMillis, int64_t waitMillis) {
              clock->Slept(nowMillis, waitMillis < 0 ? -1 : nowMillis + waitMillis);
              // only a wake ends it: this clock never moves by itself
              wake.wait(lock);
            },
        .woken = [clock] { clock->Woken(); },
    };
  }

  // Move to `nowMillis`, later than now, and wait until the thread has read it
  // and gone back to sleep.
  void Advance(NetworkChangeNotifier& notifier, int64_t nowMillis) {
    Set(nowMillis);
    notifier.Wake();
    AwaitSleep([&](int64_t sleptAtMillis, int64_t) { return sleptAtMillis == nowMillis; });
  }

  // Move without waiting for the thread: it is held inside a call, or gone.
  void Set(int64_t nowMillis) {
    std::scoped_lock lock(mutex_);
    nowMillis_ = nowMillis;
  }

  // Wait until the thread goes to sleep with `done(sleptAtMillis, wakeAtMillis)`
  // true for its reading and the close of its soonest window (-1: none open),
  // and return that close.
  int64_t AwaitSleep(const std::function<bool(int64_t, int64_t)>& done) {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return done(sleptAtMillis_, wakeAtMillis_); });
    return wakeAtMillis_;
  }

  // Wait until something has woken the thread `wakes` times in all.
  void AwaitWakes(int64_t wakes) {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return wakes_ >= wakes; });
  }

  int64_t Reads() {
    std::scoped_lock lock(mutex_);
    return reads_;
  }

  int64_t Wakes() {
    std::scoped_lock lock(mutex_);
    return wakes_;
  }

 private:
  int64_t Read() {
    std::scoped_lock lock(mutex_);
    ++reads_;
    return nowMillis_;
  }

  void Slept(int64_t nowMillis, int64_t wakeAtMillis) {
    {
      std::scoped_lock lock(mutex_);
      sleptAtMillis_ = nowMillis;
      wakeAtMillis_ = wakeAtMillis;
    }
    changed_.notify_all();
  }

  void Woken() {
    {
      std::scoped_lock lock(mutex_);
      ++wakes_;
    }
    changed_.notify_all();
  }

  std::mutex mutex_;
  std::condition_variable changed_;
  int64_t nowMillis_;
  int64_t reads_ = 0;
  int64_t wakes_ = 0;
  int64_t sleptAtMillis_ = -1;
  int64_t wakeAtMillis_ = -1;
};

void TestCoalescer() {
  NotifyCoalescer c;
  Check(!c.pending() && !c.TakeDue(0), "coalescer: nothing observed, nothing due");
  c.Observe(1000);
  c.Observe(1100);
  c.Observe(1700);
  Check(c.pending() && c.deadlineMillis() == 1000 + kWindow,
        "coalescer: the first observation sets the window");
  Check(!c.TakeDue(1000 + kWindow - 1), "coalescer: not due inside the window");
  Check(c.TakeDue(1000 + kWindow), "coalescer: due when the window closes");
  Check(c.lastBurstSize() == 3, "coalescer: one notification folds the burst");
  Check(!c.pending() && !c.TakeDue(100000), "coalescer: a burst fires once");
}

// A window other than the default is the one a coalescer keeps: the window is
// its settings', not a constant's.
void TestCoalescerHonoursItsSettings() {
  constexpr int64_t kShort = 100;
  static_assert(kShort != kWindow, "the case needs a window other than the default");
  NotifyCoalescer c(NetworkNotifySettings{.windowMillis = kShort});
  c.Observe(1000);
  Check(c.deadlineMillis() == 1000 + kShort && !c.TakeDue(1000 + kShort - 1),
        "settings: a coalescer keeps the window it is given");
  Check(c.TakeDue(1000 + kShort), "settings: and is due when that window closes");
}

// The watchdog sampler's sleep (PlanSamplerSleep): a burst that begins while it
// sleeps towards its next sample ends the sleep, the next plan sleeps only to
// that burst's window, and the rest of the burst does not wake it again.
void TestSamplerSleep() {
  NotifyCoalescer network;
  NotifyCoalescer quality;
  const SamplerSleep idle = PlanSamplerSleep(1000, 3000, network, quality);
  Check(idle.waitMillis == 2000 && !idle.networkPending && !idle.qualityPending,
        "sampler sleep: with no window open it sleeps until the next sample");
  Check(!SamplerSleepInterrupted(idle, network, quality),
        "sampler sleep: nothing observed, nothing ends it");
  network.Observe(1010);
  Check(SamplerSleepInterrupted(idle, network, quality),
        "sampler sleep: a burst that begins mid-sleep ends the sleep");
  const SamplerSleep burst = PlanSamplerSleep(1010, 3000, network, quality);
  Check(burst.waitMillis == kWindow && burst.networkPending,
        "sampler sleep: planned again, it sleeps to the window's close, not the next sample");
  network.Observe(1100);
  network.Observe(1500);
  Check(!SamplerSleepInterrupted(burst, network, quality),
        "sampler sleep: the rest of a burst does not end a sleep planned around it");
  quality.Observe(1200);
  Check(SamplerSleepInterrupted(burst, network, quality),
        "sampler sleep: a quality window the plan did not include ends it");
  const SamplerSleep both = PlanSamplerSleep(1200, 3000, network, quality);
  Check(both.waitMillis == 1010 + kWindow - 1200, "sampler sleep: the sooner of two windows");
  Check(PlanSamplerSleep(1200, 1300, network, quality).waitMillis == 100,
        "sampler sleep: a sample due first is not put off by a window");
  Check(network.TakeDue(1010 + kWindow), "sampler sleep: the burst is taken when its window closes");
  const SamplerSleep after = PlanSamplerSleep(1010 + kWindow, 3000, network, quality);
  Check(!after.networkPending, "sampler sleep: a taken window is out of the next plan");
  network.Observe(2000);
  Check(SamplerSleepInterrupted(after, network, quality),
        "sampler sleep: the next burst ends the sleep too");
}

// One family of an egress signature, each address standing for itself by its
// last byte: bound to `ifIndex`, leaving from `source` on `network` via
// `gateway`.
EgressFamilySignature Family(uint32_t ifIndex, bool usable, uint8_t source, uint8_t network,
                             uint8_t gateway) {
  EgressFamilySignature family;
  family.ifIndex = ifIndex;
  family.usable = usable;
  family.source[15] = source;
  family.network[15] = network;
  family.gateway[15] = gateway;
  return family;
}

EgressSignature V4Only(EgressFamilySignature v4) {
  EgressSignature signature;
  signature.v4 = v4;
  return signature;
}

// What moves nothing the sockets ride is no change: the reading watching
// starts from, and churn elsewhere (a Hyper-V or WSL adapter, an ethernet link
// with no route yet, a lease renewed with the same address), which reads the
// same.
void TestEgressFilterIgnoresWhatMovesNothing() {
  EgressChangeFilter filter;
  const EgressSignature wifi = V4Only(Family(12, true, 23, 23, 1));
  filter.Seed(wifi);
  int changes = 0;
  for (int i = 0; i < 20; ++i) {
    const EgressChange change = filter.Observe(wifi);
    changes += (change.network || change.binding) ? 1 : 0;
  }
  Check(changes == 0, "egress: churn that moved nothing is no change, got " + std::to_string(changes));
}

// Wi-Fi to ethernet, as the OS reports it: the ethernet link comes up with no
// default route (still on Wi-Fi), the lease arrives (ethernet), then the stale
// Wi-Fi route goes. One change, however far apart the steps.
void TestEgressFilterOneSwitchIsOneChange() {
  EgressChangeFilter filter;
  const EgressSignature wifi = V4Only(Family(12, true, 23, 23, 1));
  const EgressSignature ethernet = V4Only(Family(7, true, 50, 50, 2));
  filter.Seed(wifi);
  int network = 0;
  int binding = 0;
  for (const EgressSignature& reading : {wifi, wifi, ethernet, ethernet, ethernet}) {
    const EgressChange change = filter.Observe(reading);
    network += change.network ? 1 : 0;
    binding += change.binding ? 1 : 0;
  }
  Check(network == 1 && binding == 1,
        "egress: one switch is one change, got " + std::to_string(network) + " network and " +
            std::to_string(binding) + " binding");
}

// The cable out: the monitor keeps the index bound on purpose and nothing else
// moves, except that the interface can no longer carry a socket.
void TestEgressFilterCableOutUnderARetainedIndex() {
  EgressChangeFilter filter;
  const EgressSignature up = V4Only(Family(7, true, 50, 50, 2));
  const EgressSignature down = V4Only(Family(7, false, 50, 50, 2));
  filter.Seed(up);
  const EgressChange out = filter.Observe(down);
  Check(out.network, "egress: losing the link under a retained index is a change");
  Check(!out.binding, "egress: losing the link moves nothing pinned to the interface");
  Check(filter.Observe(up).network, "egress: the link coming back is a change");
}

// Another network on the same adapter: the index stays, the address and the
// gateway do not.
void TestEgressFilterNewNetworkOnTheSameAdapter() {
  EgressChangeFilter filter;
  filter.Seed(V4Only(Family(12, true, 23, 23, 1)));
  const EgressChange other = filter.Observe(V4Only(Family(12, true, 57, 57, 9)));
  Check(other.network && other.binding,
        "egress: a new network on the same adapter is a change, and the split tunnel follows its address");
  const EgressChange gateway = filter.Observe(V4Only(Family(12, true, 57, 57, 10)));
  Check(gateway.network && !gateway.binding, "egress: a new gateway is a new network");
}

// IPv6: a temporary address rotating inside its /64 moves the split tunnel's
// address and is not a new network; a new /64, or the family going away, is.
void TestEgressFilterIpv6() {
  EgressChangeFilter filter;
  EgressSignature before;
  before.v4 = Family(12, true, 23, 23, 1);
  before.v6 = Family(12, true, 0xA1, 0x20, 0xFE);
  filter.Seed(before);
  EgressSignature rotated = before;
  rotated.v6.source[15] = 0xA2;
  const EgressChange rotation = filter.Observe(rotated);
  Check(!rotation.network, "egress: a temporary address rotating inside its /64 is not a new network");
  Check(rotation.binding, "egress: the split tunnel follows the rotated address");
  EgressSignature renumbered = rotated;
  renumbered.v6.network[15] = 0x21;
  Check(filter.Observe(renumbered).network, "egress: a new /64 is a new network");
  EgressSignature gone = renumbered;
  gone.v6 = EgressFamilySignature{};
  Check(filter.Observe(gone).network, "egress: a family's egress going away is a change");
}

// A filter never seeded has nothing to compare with, and says everything moved.
void TestEgressFilterUnseeded() {
  EgressChangeFilter filter;
  const EgressChange first = filter.Observe(V4Only(Family(12, true, 23, 23, 1)));
  Check(first.network && first.binding && filter.seeded(),
        "egress: with nothing to compare with, everything moved");
}

void TestBurstIsOneCall() {
  Step("burst: a roam inside one window");
  auto clock = std::make_shared<TestClock>(1000);
  std::atomic<int> network{0};
  std::atomic<int> quality{0};
  NetworkChangeNotifier notifier([&] { ++network; }, [&] { ++quality; }, TestClock::For(clock));
  const auto sink = notifier.NetworkEventSink();
  // a roam: two dozen observations over half a second, inside one window
  for (int64_t at = 1000; at < 1500; at += 20) {
    sink();
    clock->Advance(notifier, at + 20);
  }
  Check(network == 0, "burst: nothing is told inside the window, got " + std::to_string(network.load()));
  Step("burst: the window closes");
  clock->Advance(notifier, 1000 + kWindow);
  Check(network == 1, "burst: the device is told when the window closes");
  clock->Advance(notifier, 10000);
  Check(network == 1, "burst: a burst is one notification, got " + std::to_string(network.load()));
  Check(quality == 0, "burst: a network burst is not a quality change");
  // a burst after the window closed is a new one
  Step("burst: a later burst");
  for (int i = 0; i < 5; ++i) sink();
  clock->Advance(notifier, 10000 + kWindow);
  Check(network == 2, "burst: a later burst is told too");
  clock->Advance(notifier, 20000);
  Check(network == 2, "burst: and only once");
  Step("burst: destruction wakes the sleeping thread");
}

// A notifier given a window other than the default tells the device when that
// window closes: its timing is its settings', not a constant's.
void TestNotifierHonoursItsSettings() {
  Step("settings: a notifier on a short window");
  constexpr int64_t kShort = 200;
  static_assert(kShort < kWindow, "the case needs a window shorter than the default");
  auto clock = std::make_shared<TestClock>(1000);
  std::atomic<int> network{0};
  NetworkChangeNotifier notifier([&] { ++network; }, [] {}, TestClock::For(clock),
                                 NetworkNotifySettings{.windowMillis = kShort});
  notifier.NetworkEventSink()();
  clock->Advance(notifier, 1000 + kShort - 1);
  Check(network == 0, "settings: nothing is told inside the notifier's own window");
  clock->Advance(notifier, 1000 + kShort);
  Check(network == 1, "settings: the notifier tells the device when its own window closes, "
                      "not the default's");
  Step("settings: destruction wakes the sleeping thread");
}

void TestQuality() {
  Step("quality: a signal change");
  auto clock = std::make_shared<TestClock>(1000);
  std::atomic<int> network{0};
  std::atomic<int> quality{0};
  NetworkChangeNotifier notifier([&] { ++network; }, [&] { ++quality; }, TestClock::For(clock));
  notifier.NetworkQualitySink()();
  clock->Advance(notifier, 1000 + kWindow - 1);
  Check(quality == 0, "quality: nothing is told inside the window");
  clock->Advance(notifier, 1000 + kWindow);
  Check(quality == 1 && network == 0, "quality: a signal change remeasures, and never as a network change");
  // A network window that is open but not yet closed does not drop it.
  Step("quality: beside an open network window");
  notifier.NetworkQualitySink()();
  clock->Advance(notifier, 1800);
  notifier.NetworkEventSink()();
  clock->Advance(notifier, 1000 + 2 * kWindow);
  Check(quality == 2 && network == 0,
        "quality: told in a pass with no network change, though a network window is open");
  clock->Advance(notifier, 1800 + kWindow);
  Check(network == 1 && quality == 2, "quality: the network change follows in its own pass");
  Step("quality: destruction wakes the sleeping thread");
}

void TestQualityInsideNetworkPass() {
  // Both windows have closed when the thread looks, so one pass takes both and
  // the network change already makes every transport remeasure. They close a
  // millisecond apart, as two bursts observed back to back can.
  Step("quality in a network pass");
  auto clock = std::make_shared<TestClock>(1000);
  std::atomic<int> network{0};
  std::atomic<int> quality{0};
  NetworkChangeNotifier notifier([&] { ++network; }, [&] { ++quality; }, TestClock::For(clock));
  notifier.NetworkEventSink()();
  clock->Advance(notifier, 1001);
  notifier.NetworkQualitySink()();
  clock->Advance(notifier, 1001 + kWindow);
  Check(network == 1 && quality == 0,
        "quality in a network pass: dropped, the network change already remeasures (got " +
            std::to_string(quality.load()) + ")");
  clock->Advance(notifier, 10000);
  Check(network == 1 && quality == 0, "quality in a network pass: and not told later");
  Step("quality in a network pass: destruction wakes the sleeping thread");
}

void TestFlappingCannotStarve() {
  Step("flapping");
  auto clock = std::make_shared<TestClock>(1000);
  std::atomic<int> network{0};
  NetworkChangeNotifier notifier([&] { ++network; }, [] {}, TestClock::For(clock));
  const auto sink = notifier.NetworkEventSink();
  // A link flapping every 100 ms for three windows: a re-extending debounce
  // would tell the device nothing until it stopped.
  int64_t at = 1000;
  for (; at < 1000 + 3 * kWindow; at += 100) {
    sink();
    clock->Advance(notifier, at + 100);
  }
  const int told = network;
  Check(told >= 2, "flapping: told while it still flaps, got " + std::to_string(told));
  clock->Advance(notifier, at + kWindow);
  Check(network == told + 1, "flapping: the burst still open is told once the link settles");
  Step("flapping: destruction wakes the sleeping thread");
}

void TestDestructionEndsIt() {
  auto clock = std::make_shared<TestClock>(1000);
  std::atomic<int> network{0};
  std::function<void()> sink;
  {
    NetworkChangeNotifier notifier([&] { ++network; }, [] {}, TestClock::For(clock));
    sink = notifier.NetworkEventSink();
    // The event's own wake, and no Wake(): the thread sleeps again having seen
    // it (or, were the window gone, having made the call).
    Step("destruction: an event wakes the thread");
    sink();
    const int64_t wakeAt = clock->AwaitSleep(
        [&](int64_t, int64_t wakeAtMillis) { return wakeAtMillis == 1000 + kWindow || network > 0; });
    Check(wakeAt == 1000 + kWindow && network == 0,
          "destruction: an event wakes the thread, which sleeps until its window closes");
    // Only the destructor's wake ends that sleep: this clock never reaches the window.
    Step("destruction: returns at once when no call runs, with a window open");
  }
  const int64_t reads = clock->Reads();
  sink();
  Check(clock->Reads() == reads, "destruction: a sink that outlives the notifier records into nothing");
  clock->Set(10000);
  Check(network == 0, "destruction: no call after the notifier is gone, not even for the open window");
}

// `holdUntilDestroyed` is the negative control's mode (see the file comment).
void TestDestructionWaitsOutTheCall(bool holdUntilDestroyed) {
  // shared, so a destructor that failed to wait leaves the call nothing dead to touch
  auto clock = std::make_shared<TestClock>(1000);
  auto running = std::make_shared<std::promise<void>>();
  auto release = std::make_shared<std::promise<void>>();
  auto finished = std::make_shared<std::atomic<bool>>(false);
  auto notifier = std::make_unique<NetworkChangeNotifier>(
      [running, released = release->get_future().share(), finished] {
        running->set_value();
        released.wait();  // a call into the device that has not returned yet
        *finished = true;
      },
      [] {}, TestClock::For(clock));
  Step("join: the call starts");
  notifier->NetworkEventSink()();
  clock->Set(1000 + kWindow);
  notifier->Wake();
  running->get_future().wait();
  const int64_t wakes = clock->Wakes();
  bool finishedAtReturn = false;
  std::thread destroyer([&] {
    notifier.reset();
    finishedAtReturn = finished->load();
  });
  if (holdUntilDestroyed) {
    Step("join: holding the call until the destructor returns, which one that waits never does");
    destroyer.join();
  } else {
    // the destructor's wake: it has cancelled, and waits for the call from here
    Step("join: the destructor cancels while the call runs");
    clock->AwaitWakes(wakes + 1);
  }
  Step("join: the destructor returns once the call has");
  release->set_value();
  if (destroyer.joinable()) destroyer.join();
  Check(finishedAtReturn,
        "join: destruction returns only after the call into the device has returned, "
        "so the owner may close the device next");
}

void TestThrowingCall() {
  Step("throw");
  auto clock = std::make_shared<TestClock>(1000);
  std::atomic<int> calls{0};
  NetworkChangeNotifier notifier(
      [&] {
        if (++calls == 1) throw std::runtime_error("the sdk call failed");
      },
      [] {}, TestClock::For(clock));
  const auto sink = notifier.NetworkEventSink();
  sink();
  // back asleep after the throw: the thread is still running
  clock->Advance(notifier, 1000 + kWindow);
  Check(calls == 1, "throw: the first call runs");
  sink();
  clock->Advance(notifier, 1000 + 2 * kWindow);
  Check(calls == 2, "throw: a call that throws does not end the notifier");
  Step("throw: destruction wakes the sleeping thread");
}

}  // namespace

int main(int argc, char** argv) {
  std::thread guard(HangGuard);
  if (argc > 1 && std::string_view(argv[1]) == "--hold-call-until-destroyed") {
    TestDestructionWaitsOutTheCall(true);
  } else {
    TestCoalescer();
    TestCoalescerHonoursItsSettings();
    TestSamplerSleep();
    TestEgressFilterIgnoresWhatMovesNothing();
    TestEgressFilterOneSwitchIsOneChange();
    TestEgressFilterCableOutUnderARetainedIndex();
    TestEgressFilterNewNetworkOnTheSameAdapter();
    TestEgressFilterIpv6();
    TestEgressFilterUnseeded();
    TestBurstIsOneCall();
    TestNotifierHonoursItsSettings();
    TestQuality();
    TestQualityInsideNetworkPass();
    TestFlappingCannotStarve();
    TestDestructionEndsIt();
    TestDestructionWaitsOutTheCall(false);
    TestThrowingCall();
  }
  {
    std::scoped_lock lock(gMutex);
    gFinished = true;
  }
  gStepped.notify_all();
  guard.join();
  std::cout << (gCases - gFailures) << "/" << gCases << " network change checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
