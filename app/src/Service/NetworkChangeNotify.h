// "THE NETWORK MOVED", TOLD TO A DEVICE AT MOST ONCE PER BURST.
//
// The tunnel session's DeviceLocal hears about an IP, route or Wi-Fi quality
// change from TunnelWatchdog's sampler thread, which EgressMonitor feeds. The
// coalescing rule both use is here, and so is the notification half on its own
// (NetworkChangeNotifier) for the PROVIDER-ONLY DEVICE, which has no watchdog
// because there is no tunnel to judge (TunnelController::
// WatchProviderNetworkLocked). Without it that device was never told about a
// Wi-Fi or Ethernet change and recovered only through its transports' timeouts.
//
// Pure C++ with no Windows or SDK header, so tools/network-change-notify-tests.cpp
// runs it on any host with a C++20 compiler, as CaptureReadiness.h's harness
// does.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace urnw {

// One SDK network-change notification per this many milliseconds.
//
// A roam produces dozens of OS notifications in a second. Below this two
// notifications never describe different states; above it the burst of a single
// roam would be split into several kicks.
inline constexpr int64_t kNetworkNotifyDebounceMillis = 750;

// ---------------------------------------------------------------------------
// the network-change coalescer (pure)
// ---------------------------------------------------------------------------
//
// A roam produces dozens of OS notifications inside a second, and each one
// would otherwise become a cgo call into the SDK that kicks every transport in
// the process. This folds a burst into exactly one notification.
//
// TRAILING FIRE ON A FIXED WINDOW, not a re-extending debounce: the deadline is
// set by the FIRST observation of a burst and never pushed out. A re-extending
// debounce can be starved indefinitely by a link that keeps flapping, which is
// precisely the condition in which the SDK most needs to be told.
class NotifyCoalescer {
 public:
  // An observation arrived. Never notifies anything; it only records.
  void Observe(int64_t nowMillis) {
    ++coalesced_;
    if (pending_) return;
    pending_ = true;
    deadlineMillis_ = nowMillis + kNetworkNotifyDebounceMillis;
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
  bool pending_ = false;
  int64_t deadlineMillis_ = 0;
  int64_t coalesced_ = 0;
  int64_t lastBurstSize_ = 0;
};

// ---------------------------------------------------------------------------
// the notification half, for a device with no watchdog
// ---------------------------------------------------------------------------
//
// TunnelWatchdog::RunSampler's notification rules, on a thread of its own:
//
//   * THE SINKS ONLY RECORD. EgressMonitor invokes them on system worker
//     threads that its Stop() waits for, so a sink that could block would wedge
//     the teardown (EgressMonitor.h). They take a short lock, note the event in
//     a coalescer and wake the thread.
//   * ONE THREAD CALLS THE DEVICE, once per burst: `networkChanged` when a
//     network burst's window closes, and `networkQualityChanged` for a Wi-Fi
//     signal change — unless a network change fires in the same pass, which
//     already makes every transport remeasure.
//   * DESTRUCTION CANCELS AND JOINS. It waits out a call into the device that is
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

  NetworkChangeNotifier(Call networkChanged, Call networkQualityChanged)
      : channel_(std::make_shared<Channel>()) {
    channel_->networkChanged = std::move(networkChanged);
    channel_->networkQualityChanged = std::move(networkQualityChanged);
    thread_ = std::thread([channel = channel_] { Run(channel); });
  }

  ~NetworkChangeNotifier() {
    {
      std::scoped_lock lock(channel_->mutex);
      channel_->cancelled = true;
    }
    channel_->wake.notify_all();
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

 private:
  struct Channel {
    std::mutex mutex;
    std::condition_variable wake;
    bool cancelled = false;
    NotifyCoalescer network;
    NotifyCoalescer quality;
    Call networkChanged;
    Call networkQualityChanged;
  };

  static int64_t NowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  static void Observe(Channel& channel, NotifyCoalescer Channel::*coalescer) {
    {
      std::scoped_lock lock(channel.mutex);
      if (channel.cancelled) return;
      (channel.*coalescer).Observe(NowMillis());
    }
    channel.wake.notify_all();
  }

  static void Run(const std::shared_ptr<Channel>& channel) {
    for (;;) {
      bool networkDue = false;
      bool qualityDue = false;
      {
        std::unique_lock lock(channel->mutex);
        for (;;) {
          if (channel->cancelled) return;
          const int64_t now = NowMillis();
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
          if (waitMillis < 0) {
            channel->wake.wait(lock);
          } else {
            channel->wake.wait_for(lock, std::chrono::milliseconds(waitMillis));
          }
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
