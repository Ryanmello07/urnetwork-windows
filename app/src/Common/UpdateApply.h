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
// (UpdateResult.h).
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
