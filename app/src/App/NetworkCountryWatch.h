// Follows the network country for the life of the app (Common/NetworkCountry.h,
// open bug P052).
//
// One thread reads the network country once at start and once per burst of
// default-route and interface changes, and reports each reading that differs
// from the last one it reported. The read is App/MobileBroadband.cpp's
// ReadNetworkCountry; the report is SdkHost::ApplyNetworkCountry, which hands
// the country to this process's sdk and to the service's.
//
//   * The first read is waited for, briefly. The sdk wants the country before
//     the network space manager builds the spaces, whose first extender dials
//     already use it, so SdkHost::Initialize waits for the first report
//     (WaitFirstReport) before it builds them. Only briefly: a read on a PC
//     whose default route is a mobile broadband adapter is a COM call into the
//     WWAN service, and a launch must not hang on it. A first report that
//     lands later still applies, in place, from each space's next extender
//     dial.
//   * The sink only records. The OS notifications (DefaultRouteChanges) arrive
//     on system threads whose unregistration waits for them, so the sink notes
//     the event in a coalescer and wakes this thread.
//   * One read per burst, by the coalescing rule the service's network
//     notifier uses (Service/NetworkChangeNotify.h NotifyCoalescer): a roam is
//     dozens of notifications in a second, and a read in the middle of one
//     sees a half-built route table.
//   * Cancel, then join within a bound. Destruction cancels and waits out a
//     report that is already running, which is the owner's and must not run
//     against an owner being destroyed. A read that is already running is
//     waited for up to the read join budget only: it is a COM call into the
//     WWAN service, and one that service never answers must not hold up the
//     app's exit. Past the budget the thread is left to finish that read on
//     its own, sharing nothing but the channel, and to drop what it read; so
//     the read must touch nothing of the owner (SdkHost's is
//     ReadNetworkCountry and nothing else). A read that finishes after the
//     cancel is not reported, and nothing is reported once destruction
//     returns.
//
// Pure C++ with no Windows or SDK header: the read and the report are
// functions, so tools/network-country-tests.cpp runs it on any host, and
// WaitSettled lets it wait for the thread instead of for the clock.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "../Service/NetworkChangeNotify.h"
#include "NetworkCountry.h"

namespace urnw {

// How long destruction waits for a read that is already running (see the
// header). A read answers in microseconds on a PC whose default route is not a
// mobile broadband adapter, and in tens of milliseconds on one whose is.
inline constexpr std::chrono::milliseconds kNetworkCountryReadJoinBudget{2000};

// One per app, owned by SdkHost; the rules are the header's.
class NetworkCountryWatch {
 public:
  using Read = std::function<netcountry::Reading()>;
  using Report = std::function<void(const netcountry::Reading&)>;

  // Starts the thread, which reads at once. `read` must touch nothing of the
  // owner, and `readJoinBudget` bounds how long destruction waits for it (see
  // the header).
  NetworkCountryWatch(Read read, Report report,
                      std::chrono::milliseconds readJoinBudget = kNetworkCountryReadJoinBudget)
      : channel_(std::make_shared<Channel>()), readJoinBudget_(readJoinBudget) {
    channel_->read = std::move(read);
    channel_->report = std::move(report);
    thread_ = std::thread([channel = channel_] {
      Run(channel);
      {
        std::scoped_lock lock(channel->mutex);
        channel->finished = true;
      }
      channel->wake.notify_all();
    });
  }

  // Cancels, then joins the thread, or leaves a read that is still running
  // past the budget to finish on its own: see the header.
  ~NetworkCountryWatch() {
    Cancel();
    if (!thread_.joinable()) return;
    const bool finished = [this] {
      std::unique_lock lock(channel_->mutex);
      return channel_->wake.wait_for(lock, readJoinBudget_, [this] { return channel_->finished; });
    }();
    if (finished) {
      thread_.join();
    } else {
      thread_.detach();
    }
  }

  NetworkCountryWatch(const NetworkCountryWatch&) = delete;
  NetworkCountryWatch& operator=(const NetworkCountryWatch&) = delete;

  // Nothing is read or reported from here on, and a report that is already
  // running has returned when this does. A read already running finishes and
  // is dropped. Never from the report, which it would wait for.
  void Cancel() {
    std::unique_lock lock(channel_->mutex);
    channel_->cancelled = true;
    channel_->wake.notify_all();
    channel_->wake.wait(lock, [this] { return !channel_->reporting; });
  }

  // True once the first reading has been read and reported, waiting up to
  // `timeout` for it.
  bool WaitFirstReport(std::chrono::milliseconds timeout) {
    std::unique_lock lock(channel_->mutex);
    channel_->wake.wait_for(lock, timeout, [this] {
      return channel_->firstReported || channel_->cancelled;
    });
    return channel_->firstReported;
  }

  // True once everything observed so far has been read and reported and the
  // thread waits for the next change, waiting up to `timeout` for it.
  bool WaitSettled(std::chrono::milliseconds timeout) {
    std::unique_lock lock(channel_->mutex);
    return channel_->wake.wait_for(lock, timeout, [this] {
      return channel_->parked && !channel_->network.pending();
    });
  }

  // For DefaultRouteChanges: an OS observation. Records and wakes; it never
  // reads or reports, and after the watch is gone it records into a cancelled
  // channel and returns.
  std::function<void()> NetworkEventSink() const {
    return [channel = channel_] {
      {
        std::scoped_lock lock(channel->mutex);
        if (channel->cancelled) return;
        channel->network.Observe(NowMillis());
      }
      channel->wake.notify_all();
    };
  }

 private:
  // What the thread and the sinks share, so a sink that outlives the watch
  // finds a cancelled channel rather than a destroyed object.
  struct Channel {
    std::mutex mutex;
    std::condition_variable wake;
    bool cancelled = false;
    bool firstReported = false;
    // the thread waits for a change with nothing pending (WaitSettled)
    bool parked = false;
    // the thread is in the report (Cancel waits it out)
    bool reporting = false;
    // the thread has returned (destruction joins it)
    bool finished = false;
    NotifyCoalescer network;
    Read read;
    Report report;
  };

  // The coalescer's clock.
  static int64_t NowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  // Asleep until a burst's window closes; false when the watch was cancelled.
  static bool WaitForBurst(Channel& channel) {
    std::unique_lock lock(channel.mutex);
    for (;;) {
      if (channel.cancelled) return false;
      const int64_t now = NowMillis();
      if (channel.network.TakeDue(now)) {
        channel.parked = false;
        return true;
      }
      if (channel.network.pending()) {
        channel.parked = false;
        channel.wake.wait_for(lock, std::chrono::milliseconds(channel.network.deadlineMillis() - now));
      } else {
        if (!channel.parked) {
          channel.parked = true;
          channel.wake.notify_all();
        }
        channel.wake.wait(lock);
      }
    }
  }

  // The thread: the first read at once, then one read per burst, each reported
  // when it differs from the last report.
  static void Run(const std::shared_ptr<Channel>& channel) {
    std::optional<netcountry::Reading> reported;
    for (bool first = true;; first = false) {
      if (!first && !WaitForBurst(*channel)) return;
      // Outside the lock: the read and the report can block, and a sink must
      // never wait behind them. Neither may end this thread (an escape from a
      // std::thread terminates the process): a read that throws is a reading
      // of its own, and a report that throws is the owner's to log.
      netcountry::Reading reading;
      try {
        reading = channel->read();
      } catch (...) {
        reading = netcountry::Reading{.code = {},
                                      .source = std::string(netcountry::kSourceUnreadable)};
      }
      // A cancel that came first drops the reading; one that comes later waits
      // for the report to return.
      {
        std::scoped_lock lock(channel->mutex);
        if (channel->cancelled) return;
        channel->reporting = true;
      }
      if (!reported || *reported != reading) {
        try {
          channel->report(reading);
        } catch (...) {
        }
        reported = reading;
      }
      {
        std::scoped_lock lock(channel->mutex);
        channel->reporting = false;
        if (first) channel->firstReported = true;
      }
      channel->wake.notify_all();
    }
  }

  std::shared_ptr<Channel> channel_;
  std::chrono::milliseconds readJoinBudget_;
  std::thread thread_;
};

}  // namespace urnw
