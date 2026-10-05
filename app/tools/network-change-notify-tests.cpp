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
// so a loaded host changes how long a run takes, never its verdict.
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

constexpr int64_t kWindow = kNetworkNotifyDebounceMillis;

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
  Check(c.pending() && c.deadlineMillis() == 1000 + kNetworkNotifyDebounceMillis,
        "coalescer: the first observation sets the window");
  Check(!c.TakeDue(1000 + kNetworkNotifyDebounceMillis - 1), "coalescer: not due inside the window");
  Check(c.TakeDue(1000 + kNetworkNotifyDebounceMillis), "coalescer: due when the window closes");
  Check(c.lastBurstSize() == 3, "coalescer: one notification folds the burst");
  Check(!c.pending() && !c.TakeDue(100000), "coalescer: a burst fires once");
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
    TestBurstIsOneCall();
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
