// The elevated update helper's report: what it did with one release, written
// as <install folder>\updates\last-result.json and read by the tray app.
//
// The helper writes the file when it ends, whether msiexec ran or the helper
// refused first, replacing the previous one atomically. It sits in the
// admin-only install folder (InstallLocation.h): only an administrator can
// write it, and every user can read it. The tray app reads it when the helper
// it waited on ends, and on its next launch, since the installer usually
// closes the app before the helper finishes.
//
//   {"tag":"v2026.10.1-1060587890","code":1060587890,"exitCode":0,
//    "finishedUtc":"2026-10-01T14:51:17Z"}
//
// exitCode is msiexec's own exit code when msiexec ran, and otherwise one of
// the helper's refusals (Refusal), which set the Win32 customer bit no msiexec
// code carries. It is also the helper's process exit code.
//
// Pure, header-only and free of JSON: tools/update-release-tests.cpp runs it
// on any host. UpdateResultJson.h reads and writes the file.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "VersionGrammar.h"

namespace urnw::update {

// The helper's own refusals, before or instead of msiexec.
enum class Refusal : std::uint32_t {
  // Started without administrator rights.
  NotElevated = 0x20000001,
  // Its own executable is not in an admin-only install location.
  NotInstalled = 0x20000002,
  // A dev build (code 0), which never updates itself.
  DevBuild = 0x20000003,
  // The arguments are not --apply-update and a tag of the channel's feed.
  BadArguments = 0x20000004,
  // Another update helper is running.
  Busy = 0x20000005,
  // The release list could not be fetched or read.
  ReleaseList = 0x20000006,
  // The release list does not offer the tag it was asked for, above this
  // build's own code.
  NotOffered = 0x20000007,
  // The release's download URL, its redirect, or the download itself failed.
  Download = 0x20000008,
  // The download does not match GitHub's SHA-256 for it.
  Digest = 0x20000009,
  // The package's UpgradeCode or ProductVersion is not the release's.
  Package = 0x2000000A,
  // The download folder could not be prepared, or something there was a
  // reparse point.
  Staging = 0x2000000B,
  // msiexec could not be started.
  InstallerNotStarted = 0x2000000C,
};

// The Win32 customer bit: set on every Refusal, never on a msiexec code.
inline constexpr std::int64_t kRefusalBit = 0x20000000;

inline constexpr bool IsRefusal(std::int64_t exitCode) {
  return exitCode > 0 && exitCode <= 0xFFFFFFFF && (exitCode & kRefusalBit) != 0;
}

// ERROR_SUCCESS_REBOOT_INITIATED and ERROR_SUCCESS_REBOOT_REQUIRED.
inline constexpr std::int64_t kRebootInitiated = 1641;
inline constexpr std::int64_t kRebootRequired = 3010;

// What an update's exit code means for the user.
enum class Outcome {
  // Installed; the app that reads this is the new version.
  Installed,
  // Installed, but files that were in use are replaced at the next restart:
  // the new product is registered and its service runs, while the old files
  // stay on disk until then. Not a failure.
  RestartRequired,
  // msiexec failed; the previous version is still installed.
  Failed,
  // The helper refused before or instead of msiexec; nothing was installed.
  Refused,
};

inline constexpr Outcome OutcomeOf(std::int64_t exitCode) {
  if (exitCode == 0) return Outcome::Installed;
  if (exitCode == kRebootRequired || exitCode == kRebootInitiated) return Outcome::RestartRequired;
  if (IsRefusal(exitCode)) return Outcome::Refused;
  return Outcome::Failed;
}

// Whether the helper keeps the package as the product's repair source: after
// an install that took, and only then.
inline constexpr bool KeepsPackage(std::int64_t exitCode) {
  const Outcome outcome = OutcomeOf(exitCode);
  return outcome == Outcome::Installed || outcome == Outcome::RestartRequired;
}

struct UpdateResult {
  std::string tag;
  std::uint64_t code = 0;
  std::int64_t exitCode = 0;
  // ISO 8601 UTC, to the second: 2026-10-01T14:51:17Z
  std::string finishedUtc;
};

// Whether `text` is an ISO 8601 UTC second, YYYY-MM-DDThh:mm:ssZ.
inline constexpr bool IsUtcSecond(std::string_view text) {
  constexpr std::string_view kShape = "0000-00-00T00:00:00Z";
  if (text.size() != kShape.size()) return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const bool digit = text[i] >= '0' && text[i] <= '9';
    if (kShape[i] == '0' ? !digit : text[i] != kShape[i]) return false;
  }
  return true;
}

// Whether a report read from the file is one the helper could have written: a
// tag the release grammar reads (after a runner test feed's prefix,
// `tagPrefix`) with that code, an exit code a process can return, and a UTC
// second.
inline bool IsWellFormed(const UpdateResult& result, std::string_view tagPrefix = {}) {
  std::string_view tag = result.tag;
  if (tag.substr(0, tagPrefix.size()) != tagPrefix) return false;
  tag.remove_prefix(tagPrefix.size());
  return result.code != 0 && version::ParseReleaseCode(tag) == result.code &&
         result.exitCode >= 0 && result.exitCode <= 0xFFFFFFFF && IsUtcSecond(result.finishedUtc);
}

}  // namespace urnw::update
