// Executable spec for telling a device the network moved
// (Service/NetworkChangeNotify.h): the coalescer's fixed window, and the
// NetworkChangeNotifier the provider-only device uses in place of the tunnel
// watchdog's sampler — one call per burst, a quality change dropped inside a
// network pass, a window that a flapping link cannot starve, no call once
// destroyed, a destructor that waits out a call already running (the device
// must outlive it), sinks that outlive the notifier, and a call that throws
// without ending the thread. Real threads and the real 750 ms window, so it
// waits with generous margins rather than asserting exact times, and runs the
// independent cases side by side.
//
//   c++ -std=c++20 -pthread -I ../src/Service network-change-notify-tests.cpp -o /tmp/network-change-notify-tests && /tmp/network-change-notify-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "NetworkChangeNotify.h"

using namespace urnw;
using namespace std::chrono_literals;

namespace {

std::mutex gMutex;
int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  std::scoped_lock lock(gMutex);
  ++gCases;
  if (!condition) {
    ++gFailures;
    // flushed: a negative control may end the process before main returns
    std::cout << "  FAIL " << what << std::endl;
  }
}

// Wait until `count` reaches `want` or the budget runs out; true if it did.
bool WaitFor(const std::atomic<int>& count, int want,
             std::chrono::milliseconds budget = 5000ms) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (count.load() < want && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(5ms);
  }
  return count.load() >= want;
}

// Longer than a window, so a second call that was going to come has come.
constexpr auto kSettle = std::chrono::milliseconds(kNetworkNotifyDebounceMillis + 600);

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
  std::atomic<int> network{0};
  std::atomic<int> quality{0};
  NetworkChangeNotifier notifier([&] { ++network; }, [&] { ++quality; });
  const auto sink = notifier.NetworkEventSink();
  // a roam: two dozen observations over half a second, inside one window
  for (int i = 0; i < 25; ++i) {
    sink();
    std::this_thread::sleep_for(20ms);
  }
  Check(WaitFor(network, 1), "burst: the device is told");
  std::this_thread::sleep_for(kSettle);
  Check(network.load() == 1, "burst: a burst is one notification, got " +
                                 std::to_string(network.load()));
  Check(quality.load() == 0, "burst: a network burst is not a quality change");
  // a burst after the window closed is a new one
  for (int i = 0; i < 5; ++i) sink();
  Check(WaitFor(network, 2), "burst: a later burst is told too");
  std::this_thread::sleep_for(kSettle);
  Check(network.load() == 2, "burst: and only once");
}

void TestQuality() {
  std::atomic<int> network{0};
  std::atomic<int> quality{0};
  NetworkChangeNotifier notifier([&] { ++network; }, [&] { ++quality; });
  notifier.NetworkQualitySink()();
  Check(WaitFor(quality, 1), "quality: a signal change remeasures");
  std::this_thread::sleep_for(kSettle);
  Check(quality.load() == 1 && network.load() == 0,
        "quality: once, and never as a network change");
}

void TestQualityInsideNetworkPass() {
  // Both windows must have closed when the thread looks. Two bursts observed
  // back to back close a millisecond apart when the observations straddle a
  // millisecond (a slow or loaded host makes that likely), and a thread that
  // wakes on time then takes them in two passes. So both are observed while the
  // thread is inside an earlier call, and it is let go once both have closed.
  std::atomic<int> network{0};
  std::atomic<int> quality{0};
  std::promise<void> release;
  const std::shared_future<void> released = release.get_future().share();
  NetworkChangeNotifier notifier(
      [&] {
        ++network;
        released.wait();
      },
      [&] { ++quality; });
  notifier.NetworkEventSink()();
  Check(WaitFor(network, 1), "quality in a network pass: an earlier network change is told");
  notifier.NetworkEventSink()();
  notifier.NetworkQualitySink()();
  std::this_thread::sleep_for(kSettle);
  release.set_value();
  Check(WaitFor(network, 2), "quality in a network pass: the network change is told");
  std::this_thread::sleep_for(kSettle);
  Check(network.load() == 2 && quality.load() == 0,
        "quality in a network pass: dropped, the network change already remeasures (got " +
            std::to_string(quality.load()) + ")");
}

void TestFlappingCannotStarve() {
  std::atomic<int> network{0};
  NetworkChangeNotifier notifier([&] { ++network; }, [] {});
  const auto sink = notifier.NetworkEventSink();
  // A link flapping every 100 ms for over two windows: a re-extending debounce
  // would tell the device nothing until it stopped.
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(3 * kNetworkNotifyDebounceMillis);
  while (std::chrono::steady_clock::now() < until) {
    sink();
    std::this_thread::sleep_for(100ms);
  }
  Check(network.load() >= 2, "flapping: told while it still flaps, got " +
                                 std::to_string(network.load()));
}

void TestDestructionEndsIt() {
  std::atomic<int> network{0};
  std::function<void()> sink;
  const auto started = std::chrono::steady_clock::now();
  {
    NetworkChangeNotifier notifier([&] { ++network; }, [] {});
    sink = notifier.NetworkEventSink();
    sink();  // pending, not yet due
  }
  const auto destroyIn = std::chrono::steady_clock::now() - started;
  Check(destroyIn < 500ms, "destruction: returns at once when no call runs");
  sink();  // a sink that outlives the notifier records into nothing
  std::this_thread::sleep_for(kSettle);
  Check(network.load() == 0, "destruction: no call after the notifier is gone");
}

void TestDestructionWaitsOutTheCall() {
  // shared, so a destructor that failed to wait leaves the call nothing dead to touch
  auto started = std::make_shared<std::atomic<int>>(0);
  auto finished = std::make_shared<std::atomic<bool>>(false);
  bool finishedAtReturn = false;
  {
    NetworkChangeNotifier notifier(
        [started, finished] {
          ++*started;
          std::this_thread::sleep_for(400ms);  // a call into the device that takes a while
          *finished = true;
        },
        [] {});
    notifier.NetworkEventSink()();
    Check(WaitFor(*started, 1), "join: the call starts");
  }
  finishedAtReturn = finished->load();
  Check(finishedAtReturn,
        "join: destruction returns only after the call into the device has returned, "
        "so the owner may close the device next");
}

void TestThrowingCall() {
  std::atomic<int> calls{0};
  NetworkChangeNotifier notifier(
      [&] {
        if (++calls == 1) throw std::runtime_error("the sdk call failed");
      },
      [] {});
  const auto sink = notifier.NetworkEventSink();
  sink();
  Check(WaitFor(calls, 1), "throw: the first call runs");
  std::this_thread::sleep_for(kSettle);
  sink();
  Check(WaitFor(calls, 2), "throw: a call that throws does not end the notifier");
}

}  // namespace

int main() {
  TestCoalescer();
  std::vector<std::thread> cases;
  for (auto test : {TestBurstIsOneCall, TestQuality, TestQualityInsideNetworkPass,
                    TestFlappingCannotStarve, TestDestructionEndsIt,
                    TestDestructionWaitsOutTheCall, TestThrowingCall}) {
    cases.emplace_back(test);
  }
  for (auto& test : cases) test.join();
  std::cout << (gCases - gFailures) << "/" << gCases << " network change checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
