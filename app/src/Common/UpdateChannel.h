// The update channel: which feed a PC takes its updates from, and what the
// app says about it, decided pure.
//
// A PC follows the official feed unless an administrator has turned the
// developer feed on for it. That choice is one file, the channel marker,
// <install>\updates\developer-channel. Its existence is the value: it holds
// no bytes and names no repository, and the feed it selects is the table's
// "developer" row, reached through FeedById (ReleaseSelection.h kFeeds) and
// never named anywhere else. Only the elevated update helper creates or
// deletes it (Updater/ApplyUpdate.cpp SetUpdateChannel), after Windows'
// consent prompt and the helper's own question.
//
// The tray app and the helper read the marker through one reader
// (InstallLocationWin32.h ReadChannelMarker), which gathers the facts below
// and asks JudgeMarker. The marker is trusted only on positive proof:
//   - updates\ is a plain folder with the descriptor the helper gives it:
//     owned by Administrators, a protected DACL, and exactly its three ACEs,
//     each inherited by files and folders: SYSTEM and Administrators full,
//     Users read and execute;
//   - the marker is a plain file of no bytes with its own descriptor: owned by
//     Administrators, a protected DACL, and exactly three ACEs, none inherited
//     or inheritable: SYSTEM and Administrators full, and for Users only
//     READ_CONTROL, SYNCHRONIZE and FILE_READ_ATTRIBUTES.
// A file a process without administrator rights creates is owned by its user,
// who cannot make Administrators its owner, so it cannot pass. Users get no
// data access to the marker, so no process of theirs can open it to hold it
// against a delete. The facts are compared with constants, and no token is
// consulted, so every account on the PC reads the same answer.
//
// A marker that is there and not trusted is the official channel, with the
// reason logged. Where updates\ is a plain folder, switching to official
// removes what lies under the marker's name, but for what the helper cannot
// remove (MarkerJudgement::present), and the surfaces say so (S14); anywhere
// else they say what the official channel says. Nothing here reads a clock,
// a file or the environment: tools/update-release-tests.cpp runs it on any
// host.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ReleaseSelection.h"
#include "UpdateResult.h"
#include "VersionGrammar.h"

namespace urnw::update {

// The id of the opt-in channel's row. Every surface and the helper reach that
// row through FeedById with this id and through nothing else, so taking the
// row out of the table takes the channel out with it.
inline constexpr std::string_view kDeveloperChannelId = "developer";

// ---- the marker's facts -----------------------------------------------------

// winnt.h's values, which a header that includes no Windows header cannot
// name (InstallLocationWin32.cpp checks that they match).
inline constexpr std::uint32_t kAttributeDirectory = 0x00000010;    // FILE_ATTRIBUTE_DIRECTORY
inline constexpr std::uint32_t kAttributeReparsePoint = 0x00000400; // FILE_ATTRIBUTE_REPARSE_POINT
inline constexpr std::uint8_t kAllowAceType = 0x00;                 // ACCESS_ALLOWED_ACE_TYPE
inline constexpr std::uint8_t kInheritedByFilesAndFolders = 0x03;   // OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE
inline constexpr std::uint32_t kFileAllAccess = 0x001F01FF;         // FILE_ALL_ACCESS, SDDL "FA"
inline constexpr std::uint32_t kFileReadAndExecute = 0x001200A9;    // FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, "FRFX"
// READ_CONTROL | SYNCHRONIZE | FILE_READ_ATTRIBUTES: Users may see that the
// marker is there and read its descriptor, and may not open it for data.
inline constexpr std::uint32_t kMarkerUsersAccess = 0x00120080;
inline constexpr std::string_view kLocalSystemSid = "S-1-5-18";
inline constexpr std::string_view kAdministratorsSid = "S-1-5-32-544";
inline constexpr std::string_view kUsersSid = "S-1-5-32-545";

// A file system object as the reader found it, without following a link.
struct PathFacts {
  enum class Found {
    Absent,      // the name is not there
    Present,     // `attributes` and `size` were read
    Unreadable,  // any other error: never taken for either
  };
  Found found = Found::Absent;
  std::uint32_t attributes = 0;
  std::uint64_t size = 0;
};

// One ACE of a DACL, as read.
struct AceFacts {
  std::uint8_t type = 0;
  std::uint8_t flags = 0;
  std::uint32_t mask = 0;
  std::string sid;  // in string form, S-1-5-18
};

// An object's owner and DACL, as read. The DACL's auto-inherited flag is not
// among them: Windows sets it as it pleases, and it grants nothing.
struct DescriptorFacts {
  bool read = false;  // false: could not be read, and is never trusted
  std::string owner;  // in string form
  bool daclPresent = false;
  bool daclProtected = false;  // SE_DACL_PROTECTED
  std::vector<AceFacts> aces;
};

namespace channel_detail {

inline bool IsAce(const AceFacts& ace, std::uint8_t flags, std::uint32_t mask, std::string_view sid) {
  return ace.type == kAllowAceType && ace.flags == flags && ace.mask == mask && ace.sid == sid;
}

// Owned by Administrators, a protected DACL, and exactly the three allow
// ACEs SYSTEM full, Administrators full and Users `usersMask`, in that order,
// each with `flags`.
inline bool HasHelpersDescriptor(const DescriptorFacts& d, std::uint8_t flags, std::uint32_t usersMask) {
  return d.read && d.owner == kAdministratorsSid && d.daclPresent && d.daclProtected &&
         d.aces.size() == 3 && IsAce(d.aces[0], flags, kFileAllAccess, kLocalSystemSid) &&
         IsAce(d.aces[1], flags, kFileAllAccess, kAdministratorsSid) &&
         IsAce(d.aces[2], flags, usersMask, kUsersSid);
}

}  // namespace channel_detail

// A folder that is no reparse point.
inline bool IsPlainFolder(const PathFacts& path) {
  return path.found == PathFacts::Found::Present && (path.attributes & kAttributeDirectory) &&
         !(path.attributes & kAttributeReparsePoint);
}

// A file of no bytes that is no folder and no reparse point.
inline bool IsPlainEmptyFile(const PathFacts& path) {
  return path.found == PathFacts::Found::Present &&
         !(path.attributes & (kAttributeDirectory | kAttributeReparsePoint)) && path.size == 0;
}

// The descriptor the helper gives updates\ (ApplyUpdate.cpp
// kAdminOnlyFolderSddl): its three ACEs inherited by files and folders, Users
// reading and executing.
inline bool HasFolderDescriptor(const DescriptorFacts& descriptor) {
  return channel_detail::HasHelpersDescriptor(descriptor, kInheritedByFilesAndFolders,
                                              kFileReadAndExecute);
}

// The descriptor the helper gives the marker (ApplyUpdate.cpp
// kChannelMarkerSddl): its three ACEs neither inherited nor inheritable, and
// no data access for Users.
inline bool HasMarkerDescriptor(const DescriptorFacts& descriptor) {
  return channel_detail::HasHelpersDescriptor(descriptor, 0, kMarkerUsersAccess);
}

// Whether updates\ is as the helper makes it, as it is found: what the
// helper notes before it gives the folder its security again.
inline bool IsHelpersFolder(const PathFacts& folder, const DescriptorFacts& descriptor) {
  return IsPlainFolder(folder) && HasFolderDescriptor(descriptor);
}

// ---- the judgement ----------------------------------------------------------

enum class ChannelMarker {
  Absent,    // no marker: the official channel
  Trusted,   // the helper's marker: the developer channel
  Rejected,  // something that is not the helper's: the official channel, said so
};

struct MarkerJudgement {
  ChannelMarker marker = ChannelMarker::Absent;
  // Why a marker was rejected, for the log; empty otherwise.
  std::string_view why;
  // Something lies under the marker's name in a plain updates\ folder,
  // trusted or not: what `--set-update-channel official` removes, except
  // whatever the helper cannot remove: a folder with something in it (it
  // removes nothing recursively), or an object someone holds open without
  // FILE_SHARE_DELETE. Either needs an updates\ that an administrator opened
  // to others, or an administrator's own act. The surfaces say S14 only
  // when this is set (LinesOf); the feed is the verdict's alone
  // (FeedOfMarker).
  bool present = false;
};

// What the facts say, judged in the reader's order: updates\, then the
// marker. A folder that is not the helper's makes whatever lies in it
// untrusted, a marker or none: only a folder that was never open to others
// can hold one.
inline MarkerJudgement JudgeMarker(const PathFacts& folder, const DescriptorFacts& folderDescriptor,
                                   const PathFacts& marker, const DescriptorFacts& markerDescriptor) {
  using Found = PathFacts::Found;
  if (folder.found == Found::Absent) return {ChannelMarker::Absent, {}};
  if (folder.found == Found::Unreadable) {
    return {ChannelMarker::Rejected, "updates\\ could not be read"};
  }
  if (!IsPlainFolder(folder)) return {ChannelMarker::Rejected, "updates\\ is not a plain folder"};
  // updates\ is a plain folder from here on: what lies under the marker's
  // name in it, the helper can remove
  const bool present = marker.found == Found::Present;
  if (!HasFolderDescriptor(folderDescriptor)) {
    return {ChannelMarker::Rejected, "updates\\ does not carry the update helper's security", present};
  }
  if (marker.found == Found::Absent) return {ChannelMarker::Absent, {}};
  if (marker.found == Found::Unreadable) {
    return {ChannelMarker::Rejected, "the channel marker could not be read"};
  }
  if (!IsPlainEmptyFile(marker)) {
    return {ChannelMarker::Rejected, "the channel marker is not an empty plain file", present};
  }
  if (!HasMarkerDescriptor(markerDescriptor)) {
    return {ChannelMarker::Rejected, "the channel marker does not carry the update helper's security", present};
  }
  return {ChannelMarker::Trusted, {}, present};
}

// The feed a marker selects: the developer row for a trusted one, while the
// table has that row, and the official feed for anything else.
inline const Feed& FeedOfMarker(ChannelMarker marker) {
  if (marker == ChannelMarker::Trusted) {
    if (const Feed* developer = FeedById(kDeveloperChannelId)) return *developer;
  }
  return kOfficialFeed;
}

// ---- the helper's argument --------------------------------------------------

// The longest channel argument read.
inline constexpr std::size_t kMaxChannelArgumentLength = 16;

// The row `--set-update-channel <text>` names, or null: plain printable ASCII,
// at most 16 characters, and the id of a row of the table, whole and in its
// case. A repository, a number, a URL or anything else is no channel, and the
// caller never names a repository.
inline const Feed* FeedOfChannelArgument(std::string_view text) {
  if (text.empty() || text.size() > kMaxChannelArgumentLength) return nullptr;
  for (const char c : text) {
    if (static_cast<unsigned char>(c) < 0x21 || static_cast<unsigned char>(c) > 0x7e) return nullptr;
  }
  return FeedById(text);
}

// ---- a change of channel ----------------------------------------------------

// What the helper does for a channel asked for, given the marker as read.
enum class ChannelPlan {
  Nothing,        // the marker already says it
  AskThenCreate,  // the helper's question first, then the marker
  Delete,         // whatever lies under the marker's name goes
};

// Official removes any marker, a rejected one included; the developer row
// makes one unless a trusted one is there.
inline ChannelPlan PlanOf(const Feed& asked, ChannelMarker marker) {
  if (asked.id == kOfficialFeed.id) {
    return marker == ChannelMarker::Absent ? ChannelPlan::Nothing : ChannelPlan::Delete;
  }
  return marker == ChannelMarker::Trusted ? ChannelPlan::Nothing : ChannelPlan::AskThenCreate;
}

// The Win32 errors a start of the helper can end with that the app tells
// apart (UpdateChecker.cpp checks that they match).
inline constexpr std::uint32_t kStartCancelled = 1223;  // ERROR_CANCELLED: Windows' prompt declined
inline constexpr std::uint32_t kStartUnsigned = 8235;   // ERROR_DS_REFERRAL: only signed programs elevate

// One request to change the channel, as it ended.
struct ChannelChange {
  const Feed* asked = nullptr;  // a row of the table
  // The marker read again once the helper ended (or never started).
  ChannelMarker markerAfter = ChannelMarker::Absent;
  // The Win32 error the start ended with; 0 when the helper started.
  std::uint32_t startError = 0;
  // The helper's exit code, when it started.
  std::int64_t exitCode = 0;
  // This app's own update was running, and the request was refused at once.
  bool ownUpdateRunning = false;
};

// The line said after a request, beside the channel's own line.
enum class ChangeLine {
  None,       // the change happened, or nothing is to be said
  Asking,     // Windows' prompt or the helper's question is up (S6)
  Unchanged,  // declined, at Windows' prompt or at the helper's question (S7)
  Unsigned,   // Windows here elevates only signed programs (S8)
  Busy,       // an update is in progress (S9)
  Failed,     // anything else, with its code (S10)
};

// What the app says after a request. The marker decides whether the change
// happened, never the exit code: an exit 0 with a marker that says otherwise
// is a failure, and a failure with a marker that says what was asked is not.
inline ChangeLine ChangeLineOf(const ChannelChange& change) {
  if (change.asked && PlanOf(*change.asked, change.markerAfter) == ChannelPlan::Nothing) {
    return ChangeLine::None;
  }
  // a request this app refused at once started nothing
  const bool started = change.startError == 0 && !change.ownUpdateRunning;
  if (change.startError == kStartCancelled ||
      (started && change.exitCode == static_cast<std::int64_t>(Refusal::Declined))) {
    return ChangeLine::Unchanged;
  }
  if (change.startError == kStartUnsigned) return ChangeLine::Unsigned;
  if (change.ownUpdateRunning ||
      (started && change.exitCode == static_cast<std::int64_t>(Refusal::Busy))) {
    return ChangeLine::Busy;
  }
  return ChangeLine::Failed;
}

// The code a failed request is reported with, as text: the start's Win32
// error, or the helper's exit code, in hex (0x2000000B).
inline std::string ChangeCodeText(const ChannelChange& change) {
  const std::uint64_t code =
      change.startError != 0 ? change.startError : static_cast<std::uint64_t>(change.exitCode);
  constexpr char kDigits[] = "0123456789ABCDEF";
  std::string hex;
  for (std::uint64_t rest = code;; rest /= 16) {
    hex.insert(hex.begin(), kDigits[rest % 16]);
    if (rest < 16) break;
  }
  return "0x" + hex;
}

// ---- what each surface says -------------------------------------------------

// Whether an update of `feed` may be handed to the user as an installer to
// run. Only the official feed's: a developer build installs only through the
// helper's own checks, and one the helper refused is never offered again as a
// file.
inline bool HandsOverInstaller(const Feed& feed) { return feed.id == kOfficialFeed.id; }

// Whether `version` (v-less, Version.h kString) is a developer build: a
// release version that ends -beta.
inline bool IsBetaVersion(std::string_view version) {
  return version::ParseReleaseCode(version) != 0 && selection_detail::EndsWith(version, "-beta");
}

// What the app knows when it says what channel it is on.
struct ChannelView {
  bool installed = false;       // an admin-only install (InstallLocation.h)
  bool codeZero = false;        // a dev build, code 0
  bool developerBuild = false;  // IsBetaVersion of this build
  // The marker as read. A copy reads one only when installed with a code
  // that is not 0; any other copy is on the official feed whatever this says.
  ChannelMarker marker = ChannelMarker::Absent;
  // ...and whether something lies under its name that switching to official
  // removes (MarkerJudgement::present).
  bool markerPresent = false;
  bool developerRow = false;    // FeedById(kDeveloperChannelId) resolves
  ChangeLine change = ChangeLine::None;
  // The last check found nothing newer than this build.
  bool foundNothingNewer = false;
  // ...and counted a release for this PC on its feed (Selection::newestCode).
  bool releaseCounts = false;
  // ...and a newer release is waiting out its day (UpdateChecker NamesWaiting).
  bool releaseWaiting = false;
};

// The channel's sentence: exactly one, first that holds.
enum class ChannelSentence {
  DevBuild,      // S12: code 0 has no channel
  NotInstalled,  // S11: a portable copy follows official releases
  Untrusted,     // S14: a marker there, not trusted, and the button that removes it
  Developer,     // S5: on the developer feed
  Official,      // S4: on the official feed
};

// What the developer screen's check line says after a check that found
// nothing newer, in place of "this build is current".
enum class CheckLine {
  AsToday,        // today's line (DeveloperPage.cpp ApplyUpdateCheck)
  NoneOfficial,   // S16: a developer build on the official feed
  NoneListed,     // S17: the developer feed counts no release for this PC
  NoneDeveloper,  // S24: nothing on the developer feed is newer
};

// The note of Settings' channel row.
enum class ChannelNote {
  Developer,  // S21
  Untrusted,  // S14
  Official,   // S23
};

struct ChannelLines {
  ChannelSentence channel = ChannelSentence::Official;
  // S15, after the channel's sentence: a developer build that follows
  // official releases.
  bool buildSentence = false;
  ChangeLine change = ChangeLine::None;
  CheckLine check = CheckLine::AsToday;
  // The developer screen's switch: built only while the table has the
  // developer row.
  bool switchShown = false;
  bool switchOn = false;
  bool switchEnabled = false;
  // "Switch to official releases" under the developer screen's line, for a
  // marker that is not trusted.
  bool repairButton = false;
  // The buttons that change the channel, while no change waits for approval.
  bool buttonsEnabled = false;
  // Settings, General: the channel row, its note, S15, S13 and its button.
  bool settingsRow = false;
  ChannelNote settingsNote = ChannelNote::Official;
  bool settingsBuildSentence = false;
  bool settingsWhere = false;
  bool settingsButton = false;
  // Whether About's "Update" row may say "up to date" after a check that
  // found nothing newer. Never on the developer feed, and never for a
  // developer build: it would be official releases' answer.
  bool aboutUpToDate = true;
};

// What every surface says about the channel, from what the app knows.
inline ChannelLines LinesOf(const ChannelView& view) {
  ChannelLines lines;
  const bool readsMarker = view.installed && !view.codeZero;
  const ChannelMarker marker = readsMarker ? view.marker : ChannelMarker::Absent;
  const bool onDeveloper = marker == ChannelMarker::Trusted && view.developerRow;
  // installed, a code that is not 0, and the developer row in the table
  const bool canChange = readsMarker && view.developerRow;

  if (view.codeZero) {
    lines.channel = ChannelSentence::DevBuild;
  } else if (!view.installed) {
    lines.channel = ChannelSentence::NotInstalled;
  } else if (marker == ChannelMarker::Rejected && view.markerPresent && canChange) {
    // S14 is said only where something not trusted lies under the marker's
    // name in a plain updates\ and the button that removes it is built, but
    // for what the helper cannot remove (MarkerJudgement::present). Any other
    // marker that is not trusted says what the official channel says, as
    // its feed is official's (the lead's G17).
    lines.channel = ChannelSentence::Untrusted;
  } else if (onDeveloper) {
    lines.channel = ChannelSentence::Developer;
  } else {
    lines.channel = ChannelSentence::Official;
  }
  lines.buildSentence = view.developerBuild && (lines.channel == ChannelSentence::Official ||
                                                lines.channel == ChannelSentence::NotInstalled ||
                                                lines.channel == ChannelSentence::Untrusted);
  lines.change = canChange ? view.change : ChangeLine::None;
  const bool asking = lines.change == ChangeLine::Asking;

  if (view.foundNothingNewer && onDeveloper) {
    lines.check = view.releaseCounts ? CheckLine::NoneDeveloper : CheckLine::NoneListed;
  } else if (view.foundNothingNewer && view.developerBuild) {
    lines.check = CheckLine::NoneOfficial;
  }

  lines.switchShown = view.developerRow;
  lines.switchOn = lines.channel == ChannelSentence::Developer;
  lines.switchEnabled = canChange && !asking;
  lines.repairButton = canChange && lines.channel == ChannelSentence::Untrusted;
  lines.buttonsEnabled = canChange && !asking;

  lines.settingsRow = lines.channel == ChannelSentence::Developer ||
                      lines.channel == ChannelSentence::Untrusted || view.developerBuild ||
                      lines.change != ChangeLine::None;
  lines.settingsNote = lines.channel == ChannelSentence::Developer   ? ChannelNote::Developer
                       : lines.channel == ChannelSentence::Untrusted ? ChannelNote::Untrusted
                                                                      : ChannelNote::Official;
  lines.settingsBuildSentence = lines.settingsRow && lines.buildSentence;
  lines.settingsWhere = lines.settingsBuildSentence && canChange;
  lines.settingsButton = lines.settingsRow && canChange &&
                         (lines.channel == ChannelSentence::Developer ||
                          lines.channel == ChannelSentence::Untrusted);
  lines.aboutUpToDate = !onDeveloper && !view.developerBuild;
  return lines;
}

}  // namespace urnw::update
