// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "FreeRefreshTicker.h"

#include <chrono>
#include <cstdint>

#include "DataInfo.h"
#include "Localization.h"

namespace urnw {
namespace {

// The wall clock in epoch milliseconds: the refresh is a UTC midnight, which a
// steady clock does not know.
int64_t WallClockMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

std::wstring FreeRefreshCountdownText() {
  return datainfo::FormatRefreshCountdown(
      datainfo::FreeRefreshCountdown(WallClockMillis()),
      [](int64_t hours, int64_t minutes) {
        return Format("provider_connected_duration_hours", hours, minutes);
      },
      [](int64_t minutes) { return Format("provider_connected_duration_minutes", minutes); });
}

void FreeRefreshTicker::Start(std::function<void()> apply) {
  apply_ = std::move(apply);
  if (!timer_) {
    timer_ = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();
    timer_.IsRepeating(false);
    // the timer is this object's and is stopped with it, so `this` outlives it
    timer_.Tick([this](auto const&, auto const&) {
      if (!apply_) return;
      apply_();
      Arm();
    });
  }
  apply_();
  Arm();
}

void FreeRefreshTicker::Stop() {
  apply_ = nullptr;
  if (timer_) timer_.Stop();
}

// one shot, re-armed at each change: the countdown moves once per displayed
// minute, so the line never shows a stale minute and the clock never spins
void FreeRefreshTicker::Arm() {
  timer_.Interval(
      std::chrono::milliseconds(datainfo::MillisUntilCountdownChanges(WallClockMillis())));
  timer_.Start();
}

}  // namespace urnw
