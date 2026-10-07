// The sign-up network-name availability check, as the create form sees it.
// Pure, header-only and free of WinRT so tools/network-name-check-tests.cpp
// runs it on any host with a C++20 compiler. The flow never reads a clock: the
// debounce and retry delays are timers the owner injects (DispatcherQueue
// timers in LoginPage, a recording fake in the tests), so every decision is
// deterministic. UI thread only.
//
// The check is advisory: the server validates the name again when the network
// is created and answers a taken name with an error the create step shows. So
// a check that could not run or did not answer (api not ready, transport
// error) is not a verdict on the name. It must not read as "taken", must not
// keep Create disabled, and is retried a bounded number of times so the line
// can still turn into a real answer.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

namespace urnw {

enum class NetworkNameCheck {
  TooShort,   // below the minimum length; nothing to check
  Checking,   // debounce or request in flight
  Available,
  Taken,
  Failed,     // the check errored; the server decides at create
};

// retries of a failed check per edit of the name
inline constexpr int kMaxNetworkNameCheckRetries = 3;

// `ok` is false when the check did not produce an answer
inline NetworkNameCheck NetworkNameCheckResult(bool ok, bool available) {
  if (!ok) return NetworkNameCheck::Failed;
  return available ? NetworkNameCheck::Available : NetworkNameCheck::Taken;
}

inline bool NetworkNameAllowsCreate(NetworkNameCheck check) {
  return check == NetworkNameCheck::Available || check == NetworkNameCheck::Failed;
}

// `retries` counts the retries already made since the name was last edited
inline bool NetworkNameCheckShouldRetry(NetworkNameCheck check, int retries) {
  return check == NetworkNameCheck::Failed && retries < kMaxNetworkNameCheckRetries;
}

// One-shot timers the flow starts and stops; each calls the owner back, and
// the owner calls Fire, when it elapses.
struct NetworkNameCheckTimers {
  std::function<void()> startDebounce;
  std::function<void()> stopDebounce;
  std::function<void()> startRetry;
  std::function<void()> stopRetry;
};

// The create step's name state across edits, debounce, the online check and
// its retries. A generation drops answers an edit has superseded.
class NetworkNameCheckFlow {
 public:
  explicit NetworkNameCheckFlow(NetworkNameCheckTimers timers) : timers_(std::move(timers)) {}

  NetworkNameCheck state() const { return state_; }
  bool AllowsCreate() const { return NetworkNameAllowsCreate(state_); }

  // the create step opened with an empty name
  void Reset() {
    ++generation_;
    retries_ = 0;
    state_ = NetworkNameCheck::TooShort;
    Run(timers_.stopDebounce);
    Run(timers_.stopRetry);
  }

  // the name was edited; `checkable` is whether it meets the minimum length
  void Edited(bool checkable) {
    ++generation_;  // drop any check still in flight
    retries_ = 0;
    Run(timers_.stopDebounce);
    Run(timers_.stopRetry);
    if (checkable) {
      state_ = NetworkNameCheck::Checking;
      Run(timers_.startDebounce);
    } else {
      state_ = NetworkNameCheck::TooShort;
    }
  }

  // The debounce or retry timer elapsed. Returns the generation to run the
  // online check for, or nullopt when nothing is sent: a name that is too
  // short, or an api that is not ready, which is applied as a failed check.
  std::optional<uint32_t> Fire(bool checkable, bool apiReady) {
    if (!checkable) return std::nullopt;
    if (!apiReady) {
      Apply(generation_, false, false);
      return std::nullopt;
    }
    return generation_;
  }

  // An answer for `generation` arrived. Returns false when a later edit
  // superseded it (nothing changed).
  bool Apply(uint32_t generation, bool ok, bool available) {
    if (generation != generation_) return false;
    state_ = NetworkNameCheckResult(ok, available);
    if (NetworkNameCheckShouldRetry(state_, retries_)) {
      ++retries_;
      Run(timers_.startRetry);
    }
    return true;
  }

 private:
  static void Run(std::function<void()> const& timerAction) {
    if (timerAction) timerAction();
  }

  NetworkNameCheckTimers timers_;
  NetworkNameCheck state_ = NetworkNameCheck::TooShort;
  uint32_t generation_ = 0;
  int retries_ = 0;  // since the last edit of the name
};

}  // namespace urnw
