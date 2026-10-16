// While the in-app updater's installer runs, no launch starts the app (owner
// decision, 2026-10-05: launches during an update are refused, "protect the
// update to not be corrupted where possible").
//
// The danger. The updater starts the elevated update helper
// (URnetworkUpdate.exe), which downloads, checks and installs the MSI
// (msiexec /i ... /passive /norestart); the MSI closes the app before it
// replaces the app's files (UpdateChecker.h). A launch while that runs started
// a new URnetwork.exe from the folder the installer is replacing: its redirect
// found the instance closing for the installer, or no instance at all, and the
// app came up holding URnetwork.exe, the Windows App Runtime and
// URnetworkSdk.dll open. Windows Installer replaces a file in use only at the
// next reboot (the install returns 3010 under /norestart), and until then the
// folder mixes old files and new.
//
// The marker. The app waits on the helper it started without quitting, and a
// launch meanwhile reaches it as usual. When the app begins to exit while the
// helper still runs (the installer closing it, or a quit), it records the
// helper before its process ends: the helper's process id and creation time,
// and when the record was written, in a file (Paths.h UpdateInProgressFile).
// The helper runs until msiexec has ended, so it is the "installer" below.
// Every launch asks first (instance::Launch). While that process runs, the
// launch exits with a short "URnetwork is updating" notice (an autostart exits
// without one) instead of redirecting, waiting or starting. A launch that
// reaches the app while it exits waits for its process to end and then finds
// the marker.
//
// It clears itself, so a crashed or failed update, a reboot in the middle of
// one, or an installer that never returns cannot refuse launches for good.
// Once the installer has ended (no process has its id, it has exited, or its
// id now names a process created at another time), or once the marker is
// older than kUpdateMarkerLifetime whatever the process does, or when the file
// does not parse, the marker is stale: the launch deletes it and starts as
// usual. The new version's first launch is usually the one that deletes it.
//
// The one launch that must not be turned away is the relaunch after an update
// (Package.wxs after one that took, the helper itself after one that failed).
// It can start while the helper still runs, carries
// instance::kAfterUpdateArgument, and waits for this marker to go stale before
// it asks (main.cpp), so it starts the app once the update has ended.
//
// The marker also says whether a VPN was connected at any moment from the
// update's hand-off until the marker was written, or that this is not known
// (VpnNote). The installer stops the VPN service, and the app does not connect
// by itself, so the report of the update tells the user to connect again. The
// launch that finds the update over keeps the marker it deletes for that
// report (SingleInstance.cpp TakeEndedUpdate), and the marker it took decides:
// the line stays unless the marker says no VPN was connected. A marker written
// before this field existed has three numbers. It still refuses launches as it
// did, and does not say whether a VPN was connected, so its report keeps the
// line, as before: not telling someone their VPN is off is worse than telling
// someone who had none. So does a marker of another update. The marker is
// taken once, so a later launch that shows the same report again took none,
// and leaves the line out: the report's first showing said what its marker
// said.
//
// The other way round does not hold. A build from before the VPN's field reads
// a marker of four numbers as none, deletes it, and does not refuse a launch
// while its installer runs. Only an in-app downgrade starts such a build during
// an update, from a build with this field to one without it, and the official
// feed never offers one.
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

// How long a marker refuses launches at most, whatever its installer does. It
// is written when the installer closes the app, after the helper's download,
// and an install takes a minute or two from there; this bounds one that hangs,
// for example on another installation that holds Windows Installer's lock.
inline constexpr std::chrono::seconds kUpdateMarkerLifetime{20 * 60};

// How far in the future a marker may say it was written (a clock moved back
// since) and still be trusted.
inline constexpr std::chrono::seconds kUpdateMarkerClockSkew{5 * 60};

// Whether a VPN was connected when an update began, as its record says it.
enum class VpnAtStart : std::uint8_t {
  // Not said: a record an older build wrote, or the record of another update.
  Unknown,
  NotConnected,
  Connected,
};

// The installer an update started, as the updater recorded it.
struct UpdateMarker {
  std::uint32_t installerProcessId = 0;
  // Its creation time (a FILETIME as one number), so that a process id reused
  // after the installer ended does not read as the installer.
  std::uint64_t installerCreationTime = 0;
  // When the marker was written, in seconds since the Unix epoch.
  std::int64_t writtenAt = 0;
  // Whether a VPN was connected when the update began.
  VpnAtStart vpn = VpnAtStart::Unknown;
};

// The file's text: "<process id> <creation time> <written at> <vpn>\n", the
// last 1 when a VPN was connected and 0 when not. A marker that does not say
// has the first three alone, as older builds wrote it.
inline std::string FormatUpdateMarker(const UpdateMarker& marker) {
  std::string text = std::to_string(marker.installerProcessId) + " " +
                     std::to_string(marker.installerCreationTime) + " " +
                     std::to_string(marker.writtenAt);
  if (marker.vpn == VpnAtStart::Connected) text += " 1";
  if (marker.vpn == VpnAtStart::NotConnected) text += " 0";
  return text + "\n";
}

// The marker a file holds, or nullopt when the text is not exactly one. The
// three numbers of a marker written before the VPN's field are still one, and
// do not say whether a VPN was connected.
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
  if (!field(marker.installerProcessId, false) || !field(marker.installerCreationTime, false)) {
    return std::nullopt;
  }
  const char* const written = at;
  if (!field(marker.writtenAt, true)) {
    // the VPN's field, after the time and a space: exactly 0 or 1, and the
    // end. No time at all is no marker, as before the VPN's field.
    if (at == written || at == end || *at != ' ') return std::nullopt;
    ++at;
    if (end - at != 1 || (*at != '0' && *at != '1')) return std::nullopt;
    marker.vpn = *at == '1' ? VpnAtStart::Connected : VpnAtStart::NotConnected;
  }
  if (marker.installerProcessId == 0 || marker.installerCreationTime == 0) return std::nullopt;
  return marker;
}

// What the record this launch took says of the VPN, for the report of an
// update that finished at `finished` (seconds since the Unix epoch): the
// record's own word when the report finished after the record was written, so
// that both are of one update. Unknown when the report has no time, or
// finished before the record and so is another update's. nullopt when this
// launch took no record: the record is taken once, by the first launch after
// its update, so a launch without one shows the report again.
inline std::optional<VpnAtStart> VpnOfReport(const std::optional<UpdateMarker>& record,
                                             std::optional<std::int64_t> finished) {
  if (!record) return std::nullopt;
  if (!finished || *finished < record->writtenAt) return VpnAtStart::Unknown;
  return record->vpn;
}

// Whether the report of an update says the VPN was disconnected for it. The
// record this launch took decides: the line stays unless it says no VPN was
// connected, since not telling someone their VPN is off is worse than telling
// someone who had none, and a VPN that is not known says it, as every build
// before the record's field did. Without a record the report is shown again,
// and leaves the line out: its first showing said what its record said. (A
// first showing whose record was never written leaves it out too.)
inline bool SaysVpnDisconnected(std::optional<VpnAtStart> vpn) {
  return vpn.has_value() && *vpn != VpnAtStart::NotConnected;
}

// Where the app's note of the VPN starts from at an update's hand-off.
enum class VpnSource : std::uint8_t {
  // the last tunnel status pushed: the service's, or one the app makes itself
  // (UpdateChecker::SetVpnConnected)
  Pushed,
  // none pushed: the state the service answered when the app asked
  Answered,
  // neither: the service has told the app nothing yet
  Unknown,
};

// The app's note of the VPN for an update (the lead's G10 and G10a). The
// service pushes a tunnel status on every change, and the app makes a few of
// its own; both come to StatusPushed. The service also answers when it is
// asked, and a copy that is signed out may have had no push at all. At the
// hand-off to the update helper (HandOff), the note starts from the last
// push, else the service's answer, else nothing. A VPN up at any moment from
// there counts, and nothing clears it: the note is connected if one was up,
// not connected only if the state was known at the hand-off and never up
// since, and unknown otherwise (Note), which is written as three numbers. Not
// safe across threads by itself: UpdateChecker guards it.
struct VpnNote {
  // the last status pushed, up or not
  std::optional<bool> pushed;
  // where the hand-off's state came from
  VpnSource source = VpnSource::Unknown;
  // a VPN was up at the hand-off, or at any moment since
  bool upSinceHandOff = false;

  void StatusPushed(bool up) {
    pushed = up;
    upSinceHandOff = upSinceHandOff || up;
  }

  // `answered`: the tunnel's state as the service answered it, up or not, or
  // nullopt when it has answered nothing.
  void HandOff(std::optional<bool> answered) {
    source = pushed ? VpnSource::Pushed : answered ? VpnSource::Answered : VpnSource::Unknown;
    const std::optional<bool> state = pushed ? pushed : answered;
    upSinceHandOff = state.value_or(false);
  }

  VpnAtStart Note() const {
    if (upSinceHandOff) return VpnAtStart::Connected;
    return source == VpnSource::Unknown ? VpnAtStart::Unknown : VpnAtStart::NotConnected;
  }
};

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
