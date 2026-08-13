// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "Onboarding.h"

#include "AppPrefs.h"

namespace urnw {
namespace {
constexpr char kPrefKey[] = "onboarding_version_seen";
constexpr int kOnboardingVersion = 1;
}  // namespace

bool Onboarding::ShouldShow() {
  return LoadAppPrefs().value(kPrefKey, 0) < kOnboardingVersion;
}

void Onboarding::MarkShown() { SaveAppPref(kPrefKey, kOnboardingVersion); }

}  // namespace urnw
