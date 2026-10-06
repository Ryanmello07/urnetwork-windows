// The Win32 half of "Launch URnetwork on system startup"; the decisions are
// Common/StartupRegistration.h. Reads and writes the user's Run value and Task
// Manager's StartupApproved record of it, under HKEY_CURRENT_USER only.
//
// Plain Win32, no WinRT, so the .cpp builds without the precompiled header
// (App.vcxproj).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace urnw {

// The setting as Windows has it: registered, and not switched off in Task
// Manager.
bool LaunchAtStartupEnabled();

// Turns the registration on or off. True when the registry now says what was
// asked; the caller shows the real state otherwise.
bool SetLaunchAtStartup(bool enabled);

// At each launch: an existing registration is brought up to date (this exe,
// --autostart). Never creates one.
void RefreshLaunchAtStartup();

}  // namespace urnw
