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
#include "UpdateChannel.h"

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

// Whether the token AdminOnlyLocation judges for, one without admin rights,
// gets no write, append, delete, attribute or ownership right on `path`, a
// file or a folder (InstallLocation.h kWriteRights). `why` says what it gets,
// or what could not be read.
bool AdminOnlyPath(const std::filesystem::path& path, std::string& why);

// What `path` is, read without following a link: absent, its attributes and
// size, or unreadable (UpdateChannel.h PathFacts).
update::PathFacts ReadPathFacts(const std::filesystem::path& path);

// The owner and the DACL of `path` (UpdateChannel.h DescriptorFacts); `read`
// is false when they could not be read.
update::DescriptorFacts ReadDescriptorFacts(const std::filesystem::path& path);

// The update channel's marker of the install in `installFolder`,
// <installFolder>\updates\developer-channel: the one place its name is
// spelled.
std::filesystem::path ChannelMarkerPath(const std::filesystem::path& installFolder);

// That marker, its facts read in order and judged (UpdateChannel.h
// JudgeMarker). `why` says why one is not trusted.
// The tray app and the update helper both read the channel here and nowhere
// else, so the two follow one feed. It reads nothing but those two names'
// attributes and descriptors: no file's data, no registry, no environment.
update::ChannelMarker ReadChannelMarker(const std::filesystem::path& installFolder, std::string& why);

}  // namespace urnw::install
