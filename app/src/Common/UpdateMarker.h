// While the in-app updater's installer runs, no launch starts the app (owner
// decision, 2026-10-05: launches during an update are refused, "protect the
// update to not be corrupted where possible").
//
// The danger. The updater starts the MSI (msiexec /i ... /passive /norestart,
// elevated) and quits, so the installer finds none of the app's files in use
// (UpdateChecker.h). A launch in the minute that follows started a new
// URnetwork.exe from the folder the installer is replacing: its redirect found
// the instance quitting for the installer, or no instance at all, and the app
// came up holding URnetwork.exe, the Windows App Runtime and URnetworkSdk.dll
// open. Windows Installer replaces a file in use only at the next reboot (the
// install returns 3010 under /norestart), and until then the folder mixes old
// files and new.
//
// The marker. When the installer has started, the updater records it before
// the app quits: the installer's process id and creation time, and when the
// record was written, in a file (Paths.h UpdateInProgressFile). Every launch
// asks first (instance::Launch). While that installer process runs, the launch
// exits with a short "URnetwork is updating" notice (an autostart exits
// without one) instead of redirecting, waiting or starting.
//
// It clears itself, so a crashed or failed update, a reboot in the middle of
// one, or an installer that never returns cannot refuse launches for good.
// Once the installer has ended (no process has its id, it has exited, or its
// id now names a process created at another time), or once the marker is
// older than kUpdateMarkerLifetime whatever the process does, or when the file
// does not parse, the marker is stale: the launch deletes it and starts as
// usual. The new version's first launch is usually the one that deletes it.
//
// Nothing relaunches the app after an install (Package.wxs), so no launch
// needs to get past a marker while its installer still runs.
//
// Pure, header-only and free of Windows headers: tools/update-marker-tests.cpp
// runs it on any host, and App/SingleInstance.cpp binds the file and the
// process.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace urnw::update {

// How long a marker refuses launches at most, whatever its installer does. An
// update installs in a minute or two; this bounds one that hangs, for example
// on another installation that holds Windows Installer's lock.
inline constexpr std::chrono::seconds kUpdateMarkerLifetime{20 * 60};

// How far in the future a marker may say it was written (a clock moved back
// since) and still be trusted.
inline constexpr std::chrono::seconds kUpdateMarkerClockSkew{5 * 60};

// The installer an update started, as the updater recorded it.
struct UpdateMarker {
  std::uint32_t installerProcessId = 0;
  // Its creation time (a FILETIME as one number), so that a process id reused
  // after the installer ended does not read as the installer.
  std::uint64_t installerCreationTime = 0;
  // When the marker was written, in seconds since the Unix epoch.
  std::int64_t writtenAt = 0;
};

// The file's text: "<process id> <creation time> <written at>\n".
inline std::string FormatUpdateMarker(const UpdateMarker& marker) {
  return std::to_string(marker.installerProcessId) + " " +
         std::to_string(marker.installerCreationTime) + " " +
         std::to_string(marker.writtenAt) + "\n";
}

// The marker a file holds, or nullopt when the text is not exactly one.
inline std::optional<UpdateMarker> ParseUpdateMarker(std::string_view text) {
  if (!text.empty() && text.back() == '\n') text.remove_suffix(1);
  UpdateMarker marker;
  const char* at = text.data();
  const char* const end = text.data() + text.size();
  const auto field = [&at, end](auto& value, bool last) {
    const auto [next, error] = std::from_chars(at, end, value);
    if (error != std::errc() || next == at) return false;
    at = next;
    if (last) return at == end;
    if (at == end || *at != ' ') return false;
    ++at;
    return true;
  };
  if (!field(marker.installerProcessId, false) || !field(marker.installerCreationTime, false) ||
      !field(marker.writtenAt, true)) {
    return std::nullopt;
  }
  if (marker.installerProcessId == 0 || marker.installerCreationTime == 0) return std::nullopt;
  return marker;
}

// What became of the installer a marker names.
enum class InstallerState {
  // That process still runs: its id, its creation time, not exited.
  Running,
  // It has ended: no process has its id, it has exited, or the id now names a
  // process created at another time.
  Ended,
  // It could not be looked at; the marker's lifetime decides.
  Unknown,
};

// What a launch makes of the marker.
enum class Verdict {
  // No marker: start as usual.
  None,
  // The installer still runs: exit without starting the app.
  Updating,
  // Left by an update that is over, unreadable, or too old: delete it, then
  // start as usual.
  Stale,
};

// For logs.
constexpr const char* ToString(Verdict verdict) {
  switch (verdict) {
    case Verdict::None: return "no update in progress";
    case Verdict::Updating: return "the updater's installer is running";
    case Verdict::Stale: return "a stale update marker";
  }
  return "unknown";
}

// A parsed marker's verdict at `now` (seconds since the Unix epoch).
inline Verdict Judge(const UpdateMarker& marker, InstallerState installer, std::int64_t now) {
  const std::int64_t age = now - marker.writtenAt;
  if (age > kUpdateMarkerLifetime.count() || age < -kUpdateMarkerClockSkew.count()) {
    return Verdict::Stale;
  }
  if (installer == InstallerState::Ended) return Verdict::Stale;
  return Verdict::Updating;
}

// The verdict for the marker file's text (nullopt: there is no file).
// `probe(marker)` looks at the installer, and is asked only for a marker that
// parses.
template <class Probe>
Verdict Check(const std::optional<std::string>& text, Probe&& probe, std::int64_t now) {
  if (!text) return Verdict::None;
  const std::optional<UpdateMarker> marker = ParseUpdateMarker(*text);
  if (!marker) return Verdict::Stale;
  return Judge(*marker, probe(*marker), now);
}

}  // namespace urnw::update
