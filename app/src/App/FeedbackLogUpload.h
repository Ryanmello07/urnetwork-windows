// The app's half of "send feedback with logs" (support inbox 2090), off the UI
// thread and owned by SdkHost.
//
// One request at a time, on a thread of its own: ask the service to upload its
// logs for the feedback (Protocol.h upload_logs), and, when it does not take
// it, upload through the DeviceRemote as the app did before the verb existed
// (logupload::AppStepAfterService). The service answers once it admitted the
// upload; the outcome of one it took comes later, in the status it pushes
// (SdkHost::FollowServiceLogUpload).
//
//   * The steps that use the host run only until Cancel, and Cancel waits out
//     the one that is running: the ask (a pipe call, bounded by the pipe's own
//     timeout) and the read of what the old path needs (the DeviceRemote's
//     handle, under the host's lock).
//   * The old path's upload itself goes through the c abi by that handle and
//     touches nothing of the host. With a service that predates the verb it is
//     a device rpc that answers only after the device's zip, so destruction
//     waits for it up to the call join budget only, by the rule
//     NetworkCountryWatch follows for its read: past it the thread is left to
//     finish that call on its own, sharing nothing with the host but the
//     channel, so the app's exit is never held up by a disk.
//
// Pure C++ with no Windows or SDK header: the steps are functions, so
// tools/log-upload-tests.cpp runs it on any host.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "LogUpload.h"

namespace urnw {

// How long destruction waits for the old path's upload call (see the header).
inline constexpr std::chrono::milliseconds kFeedbackLogUploadCallJoinBudget{2000};

// One per app, owned by SdkHost; the rules are the header's.
class FeedbackLogUpload {
 public:
  // The host's steps. `ask` asks the service and answers how it took it.
  // `prepare` reads what the old path needs and returns its call, which must
  // touch nothing of the host, or an empty one when nothing can carry it (no
  // DeviceRemote bound).
  using Ask = std::function<logupload::ServiceAnswer(const std::string& feedbackId)>;
  using Prepare = std::function<std::function<void()>(const std::string& feedbackId)>;

  FeedbackLogUpload(Ask ask, Prepare prepare,
                    std::chrono::milliseconds callJoinBudget = kFeedbackLogUploadCallJoinBudget)
      : channel_(std::make_shared<Channel>()), callJoinBudget_(callJoinBudget) {
    channel_->ask = std::move(ask);
    channel_->prepare = std::move(prepare);
  }

  // Cancels, then joins the thread, or leaves the old path's call that is
  // still running past the budget to finish on its own: see the header.
  ~FeedbackLogUpload() {
    Cancel();
    if (!thread_.joinable()) return;
    const bool finished = [this] {
      std::unique_lock lock(channel_->mutex);
      return channel_->wake.wait_for(lock, callJoinBudget_, [this] { return !channel_->running; });
    }();
    if (finished) {
      thread_.join();
    } else {
      thread_.detach();
    }
  }

  FeedbackLogUpload(const FeedbackLogUpload&) = delete;
  FeedbackLogUpload& operator=(const FeedbackLogUpload&) = delete;

  // Starts the request for `feedbackId` on its own thread and returns at once;
  // false while one is still running, or after Cancel.
  bool Send(std::string feedbackId) {
    {
      std::scoped_lock lock(channel_->mutex);
      if (channel_->cancelled || channel_->running) return false;
      channel_->running = true;
    }
    // the last request's thread has ended its work; this only reaps it
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread([channel = channel_, feedbackId = std::move(feedbackId)] {
      Run(channel, feedbackId);
      {
        std::scoped_lock lock(channel->mutex);
        channel->running = false;
      }
      channel->wake.notify_all();
    });
    return true;
  }

  // No step that uses the host runs from here on, and one that is running has
  // returned when this does. The old path's call may still be running.
  void Cancel() {
    std::unique_lock lock(channel_->mutex);
    channel_->cancelled = true;
    channel_->wake.notify_all();
    channel_->wake.wait(lock, [this] { return !channel_->hostStep; });
  }

  // True once no request is running, waiting up to `timeout`. For tests.
  bool WaitIdle(std::chrono::milliseconds timeout) {
    std::unique_lock lock(channel_->mutex);
    return channel_->wake.wait_for(lock, timeout, [this] { return !channel_->running; });
  }

 private:
  // What the thread shares with the owner, and outlives it when left behind.
  struct Channel {
    std::mutex mutex;
    std::condition_variable wake;
    bool cancelled = false;
    // a step that uses the host is running
    bool hostStep = false;
    // a request is running
    bool running = false;
    Ask ask;
    Prepare prepare;
  };

  // A host step begins, unless the owner has cancelled.
  static bool EnterHostStep(const std::shared_ptr<Channel>& channel) {
    std::scoped_lock lock(channel->mutex);
    if (channel->cancelled) return false;
    channel->hostStep = true;
    return true;
  }

  static void LeaveHostStep(const std::shared_ptr<Channel>& channel) {
    {
      std::scoped_lock lock(channel->mutex);
      channel->hostStep = false;
    }
    channel->wake.notify_all();
  }

  // One request: the ask, then the old path when the service did not take it.
  static void Run(const std::shared_ptr<Channel>& channel, const std::string& feedbackId) {
    if (!EnterHostStep(channel)) return;
    logupload::ServiceAnswer answer = logupload::ServiceAnswer::NotTaken;
    try {
      answer = channel->ask(feedbackId);
    } catch (...) {
      answer = logupload::ServiceAnswer::NotTaken;
    }
    LeaveHostStep(channel);
    // a DeviceRemote may carry it; prepare says whether one does
    if (logupload::AppStepAfterService(answer, /*deviceRemoteBound=*/true) !=
        logupload::AppStep::DeviceRemote) {
      return;
    }

    if (!EnterHostStep(channel)) return;
    std::function<void()> call;
    try {
      call = channel->prepare(feedbackId);
    } catch (...) {
      call = nullptr;
    }
    LeaveHostStep(channel);
    if (!call) return;
    try {
      call();
    } catch (...) {
    }
  }

  std::shared_ptr<Channel> channel_;
  std::chrono::milliseconds callJoinBudget_;
  std::thread thread_;
};

}  // namespace urnw
