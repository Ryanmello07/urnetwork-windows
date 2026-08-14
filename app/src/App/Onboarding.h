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

  // The tray balloon is tracked SEPARATELY from the version above, because
  // its moment moved. It used to fire at tray-icon creation, which made
  // sense when the app started hidden in the tray; with open-on-launch the
  // window opens ON TOP of it, so it taught its lesson to nobody -- the
  // beta-1 user reported never seeing any onboarding at all. Its lesson
  // ("closing did not quit the app; it lives in the tray") belongs at the
  // first hide-to-tray, which can happen in any session, long after the
  // version latch above has flipped. One balloon, ever, at the moment the
  // user has just done the thing the balloon explains.
  static bool ShouldShowTrayBalloon();
  static void MarkTrayBalloonShown();
};

}  // namespace urnw
