// "The network moved", told to a device at most once per burst.
//
// The tunnel session's DeviceLocal hears about an IP, route or Wi-Fi quality
// change from TunnelWatchdog's sampler thread, which EgressMonitor feeds. The
// coalescing rule both use is here, and so is the notification half on its own
// (NetworkChangeNotifier) for the provider-only device, which has no watchdog
// because there is no tunnel to judge (TunnelController::
// WatchProviderNetworkLocked). Without it that device was never told about a
// Wi-Fi or Ethernet change and recovered only through its transports' timeouts.
//
// What counts as the network moving is here too (EgressChangeFilter): the
// monitor reports an observation only when it moved what the SDK's sockets ride.
// So is the watchdog sampler's sleep rule (PlanSamplerSleep), which decides how
// soon after a burst begins the device is told.
//
// Pure C++ with no Windows or SDK header, so tools/network-change-notify-tests.cpp
// runs it on any host with a C++20 compiler, as CaptureReadiness.h's harness
// does. The notifier reads and sleeps on an injectable clock (NotifyClock), so
// that harness moves time by hand instead of waiting out the real window.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace urnw {

// The network-change notification's timing. Handed to whatever coalesces
// notifications rather than read from a constant, so a test or a caller can
// move it; the defaults are the service's.
struct NetworkNotifySettings {
  // One SDK network-change notification per this many milliseconds.
  //
  // A roam produces dozens of OS notifications in a second. Below this two
  // notifications never describe different states; above it the burst of a
  // single roam would be split into several kicks.
  int64_t windowMillis = 750;
};

// ---------------------------------------------------------------------------
// the network-change coalescer (pure)
// ---------------------------------------------------------------------------
//
// A roam produces dozens of OS notifications inside a second, and each one
// would otherwise become a cgo call into the SDK that kicks every transport in
// the process. This folds a burst into exactly one notification.
//
// Trailing fire on a fixed window, not a re-extending debounce: the deadline is
// set by the first observation of a burst and never pushed out. A re-extending
// debounce can be starved indefinitely by a link that keeps flapping, which is
// precisely the condition in which the SDK most needs to be told.
class NotifyCoalescer {
 public:
  // The window is the settings' (NetworkNotifySettings::windowMillis).
  explicit NotifyCoalescer(const NetworkNotifySettings& settings = {})
      : windowMillis_(settings.windowMillis) {}

  // An observation arrived. Never notifies anything; it only records.
  void Observe(int64_t nowMillis) {
    ++coalesced_;
    if (pending_) return;
    pending_ = true;
    deadlineMillis_ = nowMillis + windowMillis_;
  }

  // Is a notification due? Consumes the pending burst when it says yes, so a
  // caller that fires on true cannot fire twice for one burst.
  bool TakeDue(int64_t nowMillis) {
    if (!pending_ || nowMillis < deadlineMillis_) return false;
    pending_ = false;
    lastBurstSize_ = coalesced_;
    coalesced_ = 0;
    return true;
  }

  bool pending() const { return pending_; }
  int64_t deadlineMillis() const { return deadlineMillis_; }
  // How many observations the notification just taken folded together. For the
  // log line, so a roam reads as one kick over N events rather than as a
  // suspiciously quiet single event.
  int64_t lastBurstSize() const { return lastBurstSize_; }

 private:
  int64_t windowMillis_;
  bool pending_ = false;
  int64_t deadlineMillis_ = 0;
  int64_t coalesced_ = 0;
  int64_t lastBurstSize_ = 0;
};

// ---------------------------------------------------------------------------
// the watchdog sampler's sleep (pure)
// ---------------------------------------------------------------------------
//
// TunnelWatchdog::RunSampler sleeps until its next exit sample or the close of
// an open window, whichever is sooner. A burst that begins during that sleep
// was not in the plan, and a sleep that ended only on cancellation held its
// notification until the next sample: by default the device heard about the
// network moving between 750 ms (the window) and 2 s (the sample interval)
// after it moved, depending on where in the sample period the burst began. So
// an observation that opens a window the
// plan did not include ends the sleep, and the sampler plans again with that
// window's deadline. Later observations of the same burst do not: the window
// is fixed (NotifyCoalescer), so they cannot move the deadline, and waking for
// each of them would only spin the thread.
//
// NetworkChangeNotifier needs none of this: its thread only waits for windows,
// so every wake plans again.
struct SamplerSleep {
  // How long to sleep. Zero or less: do not.
  int64_t waitMillis = 0;
  // The windows open when the sleep was planned.
  bool networkPending = false;
  bool qualityPending = false;
};

inline SamplerSleep PlanSamplerSleep(int64_t nowMillis, int64_t nextSampleMillis,
                                     const NotifyCoalescer& network,
                                     const NotifyCoalescer& quality) {
  SamplerSleep plan;
  plan.waitMillis = nextSampleMillis - nowMillis;
  plan.networkPending = network.pending();
  plan.qualityPending = quality.pending();
  if (plan.networkPending && network.deadlineMillis() - nowMillis < plan.waitMillis)
    plan.waitMillis = network.deadlineMillis() - nowMillis;
  if (plan.qualityPending && quality.deadlineMillis() - nowMillis < plan.waitMillis)
    plan.waitMillis = quality.deadlineMillis() - nowMillis;
  return plan;
}

// Beside cancellation, what ends a planned sleep: a window it did not include.
inline bool SamplerSleepInterrupted(const SamplerSleep& plan, const NotifyCoalescer& network,
                                    const NotifyCoalescer& quality) {
  return (!plan.networkPending && network.pending()) ||
         (!plan.qualityPending && quality.pending());
}

// ---------------------------------------------------------------------------
// when a freeze is a network change (pure)
// ---------------------------------------------------------------------------
//
// TunnelWatchdog::RunEvaluator measures the gap between two of its ticks, and
// answers it at two bars, both its settings' (TunnelWatchdogSettings,
// TunnelWatchdogSettings.h) and passed in here so the rule is tested on its own:
//
//   the freeze bar (frozenMillis): the process did not run, so every failsafe
//     window starts again, and proof sampled before the gap no longer
//     qualifies a capture;
//   the network-change bar (freezeNetworkChangeMillis): long enough to have
//     been a sleep — modern standby, a hibernate, a suspended vm — so the device
//     is also told the network moved, which kicks every transport. Below it the
//     gap was a stall, and the connections across it still work: a kick would
//     only cancel their dials.
struct FreezeResponse {
  bool rebase = false;
  bool networkChanged = false;
};

inline constexpr FreezeResponse FreezeResponseFor(int64_t tickGapMillis, int64_t frozenMillis,
                                                  int64_t networkChangeMillis) {
  FreezeResponse response;
  response.rebase = tickGapMillis >= frozenMillis;
  response.networkChanged = response.rebase && tickGapMillis >= networkChangeMillis;
  return response;
}

// ---------------------------------------------------------------------------
// what counts as the network moving (pure)
// ---------------------------------------------------------------------------
//
// Every IP helper notification used to be a network change: a parameter change
// on any interface, a default route on any adapter. Each one kicked every
// transport in the process, which cancels a dial in flight, for things that move
// nothing the SDK's sockets ride on (a Hyper-V or WSL adapter coming up, the
// stale default route of a NIC already left behind, a lease renewed with the
// same address), and one switch became a kick for each of its steps that came
// more than a window apart: link up, then the DHCP lease, then the router
// advertisement.
//
// So EgressMonitor reads what a socket of each family would ride
// (NetworkConfig::ObserveEgress fills an EgressSignature) and this compares each
// reading with the last. Two questions, because two consumers want different
// answers:
//
//   network  the SDK's sockets ride a different network, or the one they ride
//            came or went: the bound interface, whether it can carry a socket
//            at all, the network it is on (the v4 address; the v6 /64, so a
//            temporary address rotating inside it is not a new network) and its
//            gateway. This is the kick, and the capture proof's invalidation.
//   binding  something pinned to the physical interface has to follow it: the
//            interface, or the exact address a socket on it leaves from, which
//            the split-tunnel driver rebinds excluded apps to.
//
// `usable` carries the case that every observation used to be reported for:
// the cable comes out, the adapter keeps its default route for a while and the
// monitor keeps its index bound on purpose (EgressMonitor::Refresh), so nothing
// else moves; what moved is that the interface can no longer carry a socket.
struct EgressFamilySignature {
  // The interface this family's sockets are bound to (the election, after the
  // monitor's retention). 0 for none.
  uint32_t ifIndex = 0;
  // It is connected, holds a default route of this family and has a preferred
  // address of this family.
  bool usable = false;
  // The address a socket on it leaves from, network order (v4 in the first four
  // bytes): the one the split-tunnel driver is given. Zero for none.
  std::array<uint8_t, 16> source{};
  // The network it is on: the v4 source, or the lowest /64 among the
  // interface's preferred v6 addresses. Zero for none.
  std::array<uint8_t, 16> network{};
  // Its default route's next hop. Zero for none.
  std::array<uint8_t, 16> gateway{};

  bool operator==(const EgressFamilySignature&) const = default;
};

struct EgressSignature {
  EgressFamilySignature v4;
  EgressFamilySignature v6;

  bool operator==(const EgressSignature&) const = default;
};

struct EgressChange {
  // Tell the device the network moved (the kick), and invalidate capture proof.
  bool network = false;
  // Re-point what follows the physical interface (EgressMonitor::SetOnChange).
  bool binding = false;
};

// Each reading against the last. Not synchronised: the monitor calls it under
// its own lock.
class EgressChangeFilter {
 public:
  // The reading watching starts from. Never a change: starting to watch is not
  // the network moving.
  void Seed(const EgressSignature& signature) {
    last_ = signature;
    seeded_ = true;
  }

  EgressChange Observe(const EgressSignature& signature) {
    EgressChange change;
    if (!seeded_) {
      // nothing to compare with: everything moved, rather than nothing
      change.network = true;
      change.binding = true;
    } else {
      change.network = NetworkMoved(last_.v4, signature.v4) || NetworkMoved(last_.v6, signature.v6);
      change.binding = BindingMoved(last_.v4, signature.v4) || BindingMoved(last_.v6, signature.v6);
    }
    Seed(signature);
    return change;
  }

  bool seeded() const { return seeded_; }
  const EgressSignature& last() const { return last_; }

 private:
  static bool NetworkMoved(const EgressFamilySignature& before, const EgressFamilySignature& after) {
    return before.ifIndex != after.ifIndex || before.usable != after.usable ||
           before.network != after.network || before.gateway != after.gateway;
  }

  static bool BindingMoved(const EgressFamilySignature& before, const EgressFamilySignature& after) {
    return before.ifIndex != after.ifIndex || before.source != after.source;
  }

  EgressSignature last_;
  bool seeded_ = false;
};

// ---------------------------------------------------------------------------
// the notifier's clock
// ---------------------------------------------------------------------------
//
// The time NetworkChangeNotifier's thread reads, the sleep it takes until the
// soonest window closes, and a hook told whenever something wakes it. The
// defaults are the service's: the steady clock, a sleep that ends when the
// window closes or an event or the cancel arrives, and no hook. The harness
// passes a clock that moves only when a test moves it, a sleep that only a wake
// ends, and a hook that counts the wakes, so a test waits for the thread to have
// looked at the time it set instead of sleeping past a window.
struct NotifyClock {
  // Milliseconds on a monotonic clock. Read under the notifier's lock.
  std::function<int64_t()> nowMillis = [] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };
  // Sleep on `wake` until it is notified or `waitMillis` have passed (-1: until
  // notified). `lock` is held on entry and on return, as for
  // std::condition_variable::wait. Nothing was due at `nowMillis`.
  std::function<void(std::unique_lock<std::mutex>& lock, std::condition_variable& wake,
                     int64_t nowMillis, int64_t waitMillis)>
      sleep = [](std::unique_lock<std::mutex>& lock, std::condition_variable& wake, int64_t,
                 int64_t waitMillis) {
        if (waitMillis < 0) {
          wake.wait(lock);
        } else {
          wake.wait_for(lock, std::chrono::milliseconds(waitMillis));
        }
      };
  // Told after an event, the cancel or NetworkChangeNotifier::Wake woke the
  // thread, outside the notifier's lock.
  std::function<void()> woken;
};

// ---------------------------------------------------------------------------
// the notification half, for a device with no watchdog
// ---------------------------------------------------------------------------
//
// TunnelWatchdog::RunSampler's notification rules, on a thread of its own:
//
//   * The sinks only record. EgressMonitor invokes them on system worker
//     threads that its Stop() waits for, so a sink that could block would wedge
//     the teardown (EgressMonitor.h). They take a short lock, note the event in
//     a coalescer and wake the thread.
//   * One thread calls the device, once per burst: `networkChanged` when a
//     network burst's window closes, and `networkQualityChanged` for a Wi-Fi
//     signal change — unless a network change fires in the same pass, which
//     already makes every transport remeasure.
//   * Destruction cancels and joins. It waits out a call into the device that is
//     already running, and makes no call after it returns. So the owner keeps
//     the device alive until then and destroys this where waiting is bounded:
//     TunnelController hands it, with the device, to the bounded teardown
//     worker, which ends it before it closes the device.
//
// The sinks and the thread share a block the notifier does not own alone, so a
// sink that fires after the notifier is gone records into a cancelled block and
// returns, rather than using a destroyed object.
class NetworkChangeNotifier {
 public:
  using Call = std::function<void()>;

  // The service passes no clock and gets the steady one (NotifyClock), and no
  // settings and gets the defaults (NetworkNotifySettings).
  NetworkChangeNotifier(Call networkChanged, Call networkQualityChanged,
                        NotifyClock clock = {}, NetworkNotifySettings settings = {})
      : channel_(std::make_shared<Channel>()) {
    channel_->networkChanged = std::move(networkChanged);
    channel_->networkQualityChanged = std::move(networkQualityChanged);
    channel_->clock = std::move(clock);
    channel_->network = NotifyCoalescer(settings);
    channel_->quality = NotifyCoalescer(settings);
    thread_ = std::thread([channel = channel_] { Run(channel); });
  }

  ~NetworkChangeNotifier() {
    {
      std::scoped_lock lock(channel_->mutex);
      channel_->cancelled = true;
    }
    WakeThread(*channel_);
    if (thread_.joinable()) thread_.join();
  }

  NetworkChangeNotifier(const NetworkChangeNotifier&) = delete;
  NetworkChangeNotifier& operator=(const NetworkChangeNotifier&) = delete;

  // For EgressMonitor::SetOnNetworkEvent and SetOnNetworkQualityEvent.
  std::function<void()> NetworkEventSink() const {
    return [channel = channel_] { Observe(*channel, &Channel::network); };
  }
  std::function<void()> NetworkQualitySink() const {
    return [channel = channel_] { Observe(*channel, &Channel::quality); };
  }

  // Have the thread read the clock again. The steady clock needs no such call:
  // the thread's sleep ends when the soonest window closes. A clock that moves
  // only when told (the harness's) is followed by one after each move.
  void Wake() {
    // Taken and released first: a thread between its reading and its sleep
    // holds the lock, so it is asleep before the wake is sent.
    {
      std::scoped_lock lock(channel_->mutex);
    }
    WakeThread(*channel_);
  }

 private:
  struct Channel {
    std::mutex mutex;
    std::condition_variable wake;
    bool cancelled = false;
    NotifyCoalescer network;
    NotifyCoalescer quality;
    Call networkChanged;
    Call networkQualityChanged;
    NotifyClock clock;
  };

  // End the thread's sleep, and tell the clock's hook.
  static void WakeThread(Channel& channel) {
    channel.wake.notify_all();
    if (channel.clock.woken) channel.clock.woken();
  }

  static void Observe(Channel& channel, NotifyCoalescer Channel::*coalescer) {
    {
      std::scoped_lock lock(channel.mutex);
      if (channel.cancelled) return;
      (channel.*coalescer).Observe(channel.clock.nowMillis());
    }
    WakeThread(channel);
  }

  static void Run(const std::shared_ptr<Channel>& channel) {
    for (;;) {
      bool networkDue = false;
      bool qualityDue = false;
      {
        std::unique_lock lock(channel->mutex);
        for (;;) {
          if (channel->cancelled) return;
          const int64_t now = channel->clock.nowMillis();
          networkDue = channel->network.TakeDue(now);
          qualityDue = channel->quality.TakeDue(now);
          if (networkDue || qualityDue) break;
          // Asleep until the earliest window closes, or until an event or the
          // cancel arrives when none is open.
          int64_t waitMillis = -1;
          if (channel->network.pending()) waitMillis = channel->network.deadlineMillis() - now;
          if (channel->quality.pending()) {
            const int64_t untilQuality = channel->quality.deadlineMillis() - now;
            if (waitMillis < 0 || untilQuality < waitMillis) waitMillis = untilQuality;
          }
          channel->clock.sleep(lock, channel->wake, now, waitMillis);
        }
      }
      // Outside the lock: a call into the device can block, and a sink must
      // never wait behind it. A call that throws is the owner's to report; it
      // must not end this thread (an escape from a std::thread terminates the
      // process).
      try {
        if (networkDue && channel->networkChanged) channel->networkChanged();
        if (qualityDue && !networkDue && channel->networkQualityChanged) {
          channel->networkQualityChanged();
        }
      } catch (...) {
      }
    }
  }

  std::shared_ptr<Channel> channel_;
  std::thread thread_;
};

}  // namespace urnw
