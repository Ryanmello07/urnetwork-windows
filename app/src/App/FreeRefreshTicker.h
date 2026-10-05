// Keeps a "free data refreshes in {time}" line current (DataInfo.h): the
// out-of-balance banner, the "About your data" sheet and the upgrade sheet
// opened by a blocked connect each show one.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <string>

#include <winrt/Microsoft.UI.Dispatching.h>

namespace urnw {

// The time left until the next 00:00 UTC as a compact duration ("5h 12m"),
// with the provider_connected_duration strings.
std::wstring FreeRefreshCountdownText();

// Calls `apply` now and again each time the displayed countdown changes, on
// the UI thread's dispatcher, until Stop. Start and Stop on the UI thread.
class FreeRefreshTicker {
 public:
  FreeRefreshTicker() = default;
  FreeRefreshTicker(const FreeRefreshTicker&) = delete;
  FreeRefreshTicker& operator=(const FreeRefreshTicker&) = delete;
  ~FreeRefreshTicker() { Stop(); }

  void Start(std::function<void()> apply);
  void Stop();
  bool Running() const { return static_cast<bool>(apply_); }

 private:
  void Arm();

  std::function<void()> apply_;
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer_{nullptr};
};

}  // namespace urnw
