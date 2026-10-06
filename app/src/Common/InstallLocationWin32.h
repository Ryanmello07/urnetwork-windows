// The Windows half of InstallLocation.h: the facts its policy judges, read
// from the file system and the security descriptors, and the token they are
// judged for.
//
// Built into Common.lib for the tray app and compiled again into the update
// helper, which links no Common.lib (InstallLocationWin32.cpp uses nothing but
// Win32, so it builds under either CRT).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <filesystem>
#include <string>

#include "InstallLocation.h"

namespace urnw::install {

// What InstallLocation.h judges for `folder` and the executable `executable`
// in it, or false (with `error`) when a fact could not be read. A fact that
// cannot be read is never assumed: the caller treats the location as not
// admin-only.
bool QueryInstallLocation(const std::filesystem::path& folder,
                          const std::filesystem::path& executable, InstallPathInfo& path,
                          InstallRights& rights, std::string& error);

// The running executable's own path, unbounded by MAX_PATH; empty on failure.
std::filesystem::path OwnExecutablePath();

// Whether `executable` and the folder it sits in are an admin-only install
// location. `why` says what failed, or what was judged.
bool AdminOnlyLocation(const std::filesystem::path& executable, std::string& why);

}  // namespace urnw::install
