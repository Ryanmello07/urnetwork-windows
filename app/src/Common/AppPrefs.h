// The app's own preferences: a single small JSON file (AppPrefsFile(), see
// Paths.h), read-modify-write so one caller's key never clobbers another's.
//
// Promoted here from two private, deliberately-duplicated 20-line copies —
// SdkHost.cpp's (advanced_mode) and UpdateChecker.cpp's
// (check_updates_automatically) — once a THIRD call site needed the same
// logic: Onboarding.cpp's onboarding_version_seen. UpdateChecker.cpp's own
// comment on its copy names this exact trigger ("a third preference site is
// the signal to promote this into Common/Paths"). The first two call sites
// are left on their own copies for this pass, to keep this change scoped to
// the file that actually needs it; a follow-up can point them at this one
// with no behavior change.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <nlohmann/json.hpp>

namespace urnw {

// The whole file, or an empty object if it does not exist yet or fails to
// parse. Never throws.
nlohmann::json LoadAppPrefs();

// Read-modify-write: loads the whole file, sets `key`, writes the whole file
// back. NEVER serialize just your own key — that deletes everyone else's.
void SaveAppPref(const char* key, const nlohmann::json& value);

}  // namespace urnw
