// Phase E5: onboarding for a first run. A separate flow from the window
// reveal — it shares only the tray icon's rect, not motion — three steps,
// zero new full-screen UI:
//   1. a tray balloon at icon creation (AppController::Start)
//   2. the existing ServiceSetup banner, already focal when there is
//      anything for it to say (MainWindow) — nothing new to build here, see
//      the note on MainWindow's onboarding trigger
//   3. a TeachingTip on the Connect button (MainWindow), skipped when step 2
//      is showing something, so the two do not compete for attention
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace urnw {

class Onboarding {
 public:
  // True if this run should show the first-run sequence: the persisted
  // "onboarding_version_seen" pref (app_prefs.json, via Common/AppPrefs.h)
  // is behind the current onboarding version. An int, not a bool, so a
  // future revision of the sequence can bump the version and re-show it.
  //
  // Call ONCE per process and cache the answer — MarkShown() flips the
  // persisted value, so a second call mid-flow would answer differently.
  static bool ShouldShow();

  // Record that onboarding has been shown, for this version. Called the
  // moment the FIRST step (the tray balloon) actually shows, not when the
  // three-step sequence completes — an abandoned first run (closed at step
  // 1, or signed out before reaching Connect) must not replay every launch
  // after that.
  static void MarkShown();
};

}  // namespace urnw
