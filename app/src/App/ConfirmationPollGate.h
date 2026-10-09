// The post-checkout confirmation poll's run gate and give-up budget (the
// SubscriptionBalanceStore half that needs no timers, so it is testable on any
// host: tools/confirmation-poll-gate-tests.cpp).
//
// The 5-second confirmation poll runs only while the user is looking at the
// app: the window shown and not minimized (visible) AND the app in the
// foreground (focused). A hosted checkout leaves the window visible BEHIND the
// browser, so a visibility-only gate kept spending the budget while the user
// typed card details and returned to a false "timed out" (UPGRADE.md D1). The
// SDK SubscriptionBalanceViewController pauses the same way on
// SetForeground(false).
//
// The budget counts running time only: it resumes when the gate opens and
// banks what is left when it closes, on a monotonic clock in milliseconds
// passed in by the caller.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cstdint>

namespace urnw {

// Give up confirming after this much ACTIVE polling (macOS maxPollingDuration).
inline constexpr int64_t kConfirmationBudgetMillis = 120 * 1000;

class ConfirmationPollGate {
 public:
  explicit ConfirmationPollGate(int64_t budgetMillis) : budgetMillis_(budgetMillis) {}

  // A fresh confirmation with the full budget. True when the poll runs now.
  bool Start(int64_t nowMillis) {
    confirming_ = true;
    remainingMillis_ = budgetMillis_;
    running_ = false;
    return Apply(nowMillis);
  }

  // The confirmation ended (confirmed, gave up, or the session stopped).
  void Stop() {
    confirming_ = false;
    running_ = false;
    remainingMillis_ = 0;
  }

  // Each returns true on a paused -> running transition: the caller re-arms
  // its timer and polls immediately (a payment that landed while paused
  // confirms on the first frame back).
  bool SetVisible(bool visible, int64_t nowMillis) {
    visible_ = visible;
    return Apply(nowMillis);
  }
  bool SetFocused(bool focused, int64_t nowMillis) {
    focused_ = focused;
    return Apply(nowMillis);
  }

  bool Confirming() const { return confirming_; }
  // confirming, and the poll should be ticking
  bool Running() const { return running_; }
  bool Visible() const { return visible_; }
  bool Focused() const { return focused_; }

  int64_t RemainingAt(int64_t nowMillis) const {
    if (!running_) return remainingMillis_;
    return std::max<int64_t>(0, remainingMillis_ - std::max<int64_t>(0, nowMillis - runningSinceMillis_));
  }
  bool ExpiredAt(int64_t nowMillis) const { return confirming_ && RemainingAt(nowMillis) <= 0; }

 private:
  bool Apply(int64_t nowMillis) {
    const bool shouldRun = confirming_ && visible_ && focused_;
    if (shouldRun == running_) return false;
    if (shouldRun) {
      runningSinceMillis_ = nowMillis;
      running_ = true;
      return true;
    }
    // bank the unspent budget: it only burns while the poll runs
    remainingMillis_ = RemainingAt(nowMillis);
    running_ = false;
    return false;
  }

  int64_t budgetMillis_;
  bool confirming_ = false;
  bool visible_ = false;
  // Focus starts true: the window is activated when it is first shown, and a
  // missing activation event must never strand a confirmation.
  bool focused_ = true;
  bool running_ = false;
  int64_t remainingMillis_ = 0;
  int64_t runningSinceMillis_ = 0;
};

}  // namespace urnw
