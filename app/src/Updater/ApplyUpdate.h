// The elevated half of an update: fetch, check and install one release, and
// report how it went.
//
// The tray app starts this (URnetworkUpdate.exe --apply-update <tag>) with
// "runas" from the admin-only install folder, and waits on it. Nothing the
// tray app or any process of the user's can write decides what is installed:
// the only input is the tag, and it is checked against what this process
// fetches itself. In order:
//   1. Elevated, from an admin-only install location (InstallLocation.h), a
//      release build, one helper at a time.
//   2. The channel's feed (official until the opt-in developer channel adds a
//      machine-wide choice), never one from the arguments.
//   3. The release list at /repositories/<id>/releases, redirects refused,
//      status 200, its Date header kept.
//   4. SelectRelease on that list must offer exactly <tag>, above this
//      build's code, at the feed's own download URL. On the official feed
//      that is the newest release that had been out, unchanged, for a day
//      when the list's UTC day began (ReleaseSelection.h SoakStartOf,
//      HasSoaked), judged by the list's Date header and the times the list
//      gives the release alone: its published_at, its updated_at and its
//      package's updated_at. A tag the tray app was offered before a newer
//      release reached that line, or before the release was withdrawn or
//      changed, is refused (NotOffered), and the tray app checks again.
//   5. The download: one 302, followed only to GitHub's release-asset hosts,
//      then 200, at most 1 GiB, written to <install>\updates\<tag>\<asset>,
//      the file created new. The folders are no reparse points, and they and
//      every file the helper writes in them carry an admin-only ACL of their
//      own, owned by Administrators, not one they inherit.
//   6. SHA-256 of the bytes, read through a handle that shares neither write
//      nor delete and is held until msiexec has ended, equal to the digest
//      from the list.
//   7. The package's UpgradeCode and ProductVersion (MsiOpenDatabaseW, read
//      only) are this product's and the release code's.
//   8. The folders, the package and msiexec's log grant a token without admin
//      rights no write, delete or ownership right. This executable's own
//      image is moved out of the install folder, so the installer replaces
//      its file freely, and msiexec runs from System32 with /passive
//      /norestart, a verbose log beside the package, and UPDATE_RELAUNCH=1.
//   9. last-result.json gets the outcome. After 0 or 3010 the package stays
//      as the product's repair source and older downloads go; otherwise the
//      package goes, the log stays, and the image moves back.
//  10. After an install that failed, the relaunch the installer makes only
//      after one that took: this program again, through the user's shell.
// The process exits with msiexec's code, or with the step's refusal
// (UpdateResult.h).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string_view>

namespace urnw::updater {

int ApplyUpdate(std::wstring_view tag);

// The machine's update channel, set on an administrator's word:
// URnetworkUpdate.exe --set-update-channel <channel>. In order:
//   1. Elevated, from an admin-only install location, a release build, as for
//      an update.
//   2. <channel> is an id of the feed table (UpdateChannel.h
//      FeedOfChannelArgument): official, or developer while the table has
//      that row. A repository, a number or anything else is BadArguments,
//      before anything is shown or written.
//   3. The marker as it is decides what is to be done (UpdateChannel.h
//      PlanOf): nothing, a delete, or, for the developer channel, the
//      helper's own question and then the marker.
//   4. The question, for the developer channel alone, naming the row's
//      repository, with Cancel the default. Any answer but OK, or a window
//      station nobody sees, is Declined, and nothing is written.
//   5. The lock (TakeHelperLock), after the question, so a question left
//      open blocks no update.
//   6. The marker read again under the lock, and planned again.
//   7. Whatever lies under the marker's name goes (a file, an empty folder or
//      a link, nothing recursively), and for the developer channel the marker
//      is created new, with its own descriptor. Nothing is ever replaced.
//   8. The marker read back through the reader the tray app uses; a marker
//      that does not say what was asked is removed, and the verb is Staging.
// It exits 0 when the marker says what was asked, or with a refusal
// (UpdateResult.h). It writes no last-result.json, which is about a release.
int SetUpdateChannel(std::wstring_view channel);

}  // namespace urnw::updater
