// What the elevated update helper (app/src/Updater) decides before it lets
// msiexec run, decided pure.
//
// The helper is handed one argument it does not trust, the release tag. It
// fetches its feed's release list itself and installs only when all of these
// hold:
//   - the tag is one of its feed's (IsTagArgument);
//   - SelectRelease, run on the list it fetched, offers exactly that tag, above
//     the helper's own code (SelectionOffers);
//   - the download, hashed through the handle that keeps it from changing,
//     matches GitHub's SHA-256 from that same list (UpdateFormats.h);
//   - the package is this product's, at the ProductVersion the tag's code
//     derives (PackageMatches).
// Then it runs msiexec with MsiexecCommandLine and reports in last-result.json
// (UpdateResult.h). Also here: which environment variables are the app's
// overrides, which the elevated helper drops (IsAppOverrideName), and how the
// tray app's wait on the helper ends (AwaitHelper).
//
// Pure, header-only and free of Windows headers: tools/update-release-tests.cpp
// runs it on any host.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "ReleaseSelection.h"
#include "UpdateFormats.h"

namespace urnw::update {

// The product's UpgradeCode as the MSI's Property table holds it
// (installer/Package.wxs; update_apply_wiring_test.go keeps the two equal).
inline constexpr std::string_view kUpgradeCode = "{A7C1E2D3-4B5F-6081-9C2D-3E4F50617283}";

// The longest tag argument read: a test feed's prefix and the grammar's
// longest tag fit far inside it.
inline constexpr std::size_t kMaxTagLength = 96;

// Whether `tag` may name a release of `feed`: short, plain ASCII letters,
// digits, dots and dashes only (so it can name a folder and sit in a command
// line unquoted), and read by the feed's tag grammar.
inline bool IsTagArgument(const Feed& feed, std::string_view tag) {
  if (tag.empty() || tag.size() > kMaxTagLength) return false;
  for (const char c : tag) {
    const bool plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       c == '.' || c == '-';
    if (!plain) return false;
  }
  if (tag.front() == '.' || tag.front() == '-') return false;
  return ParseFeedTag(feed, tag).has_value();
}

// Whether the selection made from the helper's own fetch of the release list
// offers exactly `tag`, above `ownCode`, with an asset and digest to check.
inline bool SelectionOffers(const Selection& selection, std::string_view tag, std::uint64_t ownCode) {
  return selection.code != 0 && selection.tag == tag && selection.code > ownCode &&
         !selection.assetUrl.empty() && !selection.assetName.empty() &&
         selection.digestHex.size() == 64;
}

// Whether a package is this product's release `code`: its UpgradeCode is
// kUpgradeCode, and its ProductVersion is the one tools/UrVersion.ps1 derives
// from the code. A mis-stamped package, an old one re-uploaded under a newer
// tag, or another product's never passes.
inline bool PackageMatches(std::string_view upgradeCode, std::string_view productVersion,
                           std::uint64_t code) {
  const std::string want = UrMsiVersion(code);
  return EqualsAsciiCaseless(upgradeCode, kUpgradeCode) && !want.empty() &&
         productVersion == want;
}

// The command line the helper starts msiexec with, System32's msiexec named
// in full: install the package with progress and no questions, never
// restarting the machine, logging verbosely to `log`, and asking the package
// to start the app again when it is done (UPDATE_RELAUNCH, Package.wxs).
// Both paths sit in the admin-only install folder and come from the tag
// grammar, so neither holds a quote.
inline std::wstring MsiexecCommandLine(std::wstring_view msiexec, std::wstring_view package,
                                       std::wstring_view log) {
  std::wstring command = L"\"";
  command.append(msiexec);
  command.append(L"\" /i \"");
  command.append(package);
  command.append(L"\" /passive /norestart /l*v \"");
  command.append(log);
  command.append(L"\" UPDATE_RELAUNCH=1");
  return command;
}

// Whether an environment variable is one of the app's overrides
// (URNETWORK_APP_ROOT, URNETWORK_NETWORK_HOST, ...): its name starts with
// URNETWORK_ in any case, as Windows matches variable names. The elevated
// helper drops every one before it does anything (Updater/main.cpp): they are
// the user's to set and steer nothing elevated. The relaunch after an update
// is the user's app, and keeps them.
inline bool IsAppOverrideName(std::wstring_view name) {
  constexpr std::wstring_view kPrefix = L"URNETWORK_";
  if (name.size() < kPrefix.size()) return false;
  for (std::size_t i = 0; i < kPrefix.size(); ++i) {
    wchar_t c = name[i];
    if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
    if (c != kPrefix[i]) return false;
  }
  return true;
}

// How the tray app's wait on the update helper it started ended.
enum class HelperWait {
  // The helper ended while this app still ran: its report is this app's to
  // read.
  Ended,
  // This app began to exit first (the installer's close, a quit, the end of
  // the session): the helper runs on without it.
  AppExiting,
};

// The tray app's wait on the helper: `ended()` waits one slice and says
// whether the helper has ended, `exiting()` whether this app has begun to
// exit. It ends with whichever comes first, so the app's teardown, which joins
// the thread that waits, never waits for the helper.
template <class Ended, class Exiting>
HelperWait AwaitHelper(Ended&& ended, Exiting&& exiting) {
  for (;;) {
    if (ended()) return HelperWait::Ended;
    if (exiting()) return HelperWait::AppExiting;
  }
}

// `unixSeconds` as an ISO 8601 UTC second, 2026-10-01T14:51:17Z.
inline std::string FormatUtcSecond(std::int64_t unixSeconds) {
  const std::int64_t days = unixSeconds >= 0 ? unixSeconds / 86400 : (unixSeconds - 86399) / 86400;
  const std::int64_t secondOfDay = unixSeconds - days * 86400;
  const msi_detail::CivilDate date = msi_detail::CivilFromDays(days);
  auto two = [](std::int64_t value) {
    std::string text = std::to_string(value);
    return value < 10 ? "0" + text : text;
  };
  std::string year = std::to_string(date.year);
  while (year.size() < 4) year.insert(0, "0");
  return year + "-" + two(date.month) + "-" + two(date.day) + "T" + two(secondOfDay / 3600) + ":" +
         two(secondOfDay / 60 % 60) + ":" + two(secondOfDay % 60) + "Z";
}

}  // namespace urnw::update
