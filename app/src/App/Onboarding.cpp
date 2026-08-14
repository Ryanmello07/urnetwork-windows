// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "Onboarding.h"

#include "AppPrefs.h"

namespace urnw {
namespace {
constexpr char kPrefKey[] = "onboarding_version_seen";
// v2: v1 was consumed invisibly. Its tray balloon fired at icon creation and
// was immediately covered by the auto-opened window, and the banner/tip ran
// behind the AnchorPoint layout bug (content displaced off-screen), so every
// v1 user "saw" onboarding without seeing anything. The version int exists
// for exactly this: bump it and the sequence replays once.
constexpr int kOnboardingVersion = 2;
constexpr char kBalloonPrefKey[] = "onb_tray_balloon_seen";
}  // namespace

bool Onboarding::ShouldShow() {
  return LoadAppPrefs().value(kPrefKey, 0) < kOnboardingVersion;
}

void Onboarding::MarkShown() { SaveAppPref(kPrefKey, kOnboardingVersion); }

bool Onboarding::ShouldShowTrayBalloon() {
  return !LoadAppPrefs().value(kBalloonPrefKey, false);
}

void Onboarding::MarkTrayBalloonShown() { SaveAppPref(kBalloonPrefKey, true); }

}  // namespace urnw
