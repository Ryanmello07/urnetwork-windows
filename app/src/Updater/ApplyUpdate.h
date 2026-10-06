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
//      build's code, at the feed's own download URL.
//   5. The download: one 302, followed only to GitHub's release-asset hosts,
//      then 200, at most 1 GiB, written to <install>\updates\<tag>\<asset>
//      in folders that inherit the install folder's ACL and are no reparse
//      points, the file created new.
//   6. SHA-256 of the bytes, read through a handle that shares neither write
//      nor delete and is held until msiexec has ended, equal to the digest
//      from the list.
//   7. The package's UpgradeCode and ProductVersion (MsiOpenDatabaseW, read
//      only) are this product's and the release code's.
//   8. This executable's own image is moved out of the install folder, so
//      the installer replaces its file freely, and msiexec runs from System32
//      with /passive /norestart, a verbose log beside the package, and
//      UPDATE_RELAUNCH=1.
//   9. last-result.json gets the outcome. After 0 or 3010 the package stays
//      as the product's repair source and older downloads go; otherwise the
//      package goes, the log stays, and the image moves back.
// The process exits with msiexec's code, or with the step's refusal
// (UpdateResult.h).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string_view>

namespace urnw::updater {

int ApplyUpdate(std::wstring_view tag);

}  // namespace urnw::updater
