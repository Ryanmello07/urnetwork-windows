// Which published release the update checker offers and the elevated update
// helper installs, decided pure.
//
// The feed is one GitHub repository's releases, compiled in (kFeeds) and
// addressed by the repository's numeric id: the API path
// /repositories/<id>/releases is immune to a rename, and to someone else
// registering the owner's old name, both of which redirect or take over an
// owner/name path. The owner and repo are kept only to check the download URL
// a release names (FeedAssetUrl).
//
// A release is a tag `v<YYYY.M.D>-<code>` with one MSI per architecture
// attached as `URnetwork-<YYYY.M.D>-<code>-<x64|arm64>.msi`, the names
// build/all/run.sh mints (require_windows_artifacts + the github_release_upload
// loop). SelectRelease skips, before anything else is read:
//   - drafts;
//   - prereleases, except a `-beta` one on a feed that takes them: a
//     prerelease that outranks the stable release by code (the nightly repo's
//     android-only F-Droid variants at code+2 / code+3 are the model) must not
//     be offered or named as the newest release;
//   - releases that are not immutable, on a feed that requires it: an
//     immutable release's assets cannot be replaced after publication, and its
//     tag cannot be reused once it is deleted;
//   - codes whose instant is more than kFutureCodeLimit after the release
//     list's own Date header: one mistyped or hostile far-future code would
//     otherwise outrank every real release for good.
// "Newest" then counts only releases that carry this product's MSI for this
// architecture, so a release of the other platforms alone is never named as
// the newest one. The offer additionally needs a usable sha256 digest on that
// asset.
//
// The checker and the helper turn the releases JSON into these plain structs
// (ReleaseJson.h) and ask SelectRelease; the decision touches no Windows
// headers, so tools/update-release-tests.cpp runs it on any host against the
// names the release pipeline actually publishes.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "UpdateFormats.h"
#include "VersionGrammar.h"

namespace urnw::update {

// One release feed.
struct Feed {
  // The channel's name: "official". The opt-in developer channel joins the
  // table with its own feed.
  std::string_view id;
  // GitHub's repository id, the path of the release list.
  std::uint64_t numericRepoId = 0;
  // How GitHub spells the repository in its download URLs.
  std::string_view owner;
  std::string_view repo;
  // A prerelease whose tag ends in -beta counts; every other prerelease is
  // skipped.
  bool acceptBetaPrereleases = false;
  // Only immutable releases count.
  bool requireImmutable = false;
  // A runner test's feed only, never one of kFeeds: every tag starts with
  // this, and it is stripped before the grammar is read.
  std::string_view tagPrefix;
  // A runner test's feed only: any prerelease counts.
  bool acceptAnyPrerelease = false;
};

// The official releases: the stable urnetwork/windows releases, never a
// personal fork. Its numeric id is the one GitHub assigned the repository.
// Immutable releases are required, as on every official feed.
inline constexpr Feed kOfficialFeed{
    .id = "official",
    .numericRepoId = 1297133846,
    .owner = "urnetwork",
    .repo = "windows",
    .acceptBetaPrereleases = false,
    .requireImmutable = true,
    .tagPrefix = "",
    .acceptAnyPrerelease = false,
};

// Every feed this build knows. The update channel names one of them.
inline constexpr Feed kFeeds[] = {kOfficialFeed};

// The feed named `id`, or null.
inline const Feed* FeedById(std::string_view id) {
  for (const Feed& feed : kFeeds) {
    if (feed.id == id) return &feed;
  }
  return nullptr;
}

// How far after the release list's Date header a release code may point: a
// code is minted from the clock of the machine that built the release, and
// two days covers any honest skew between that clock and GitHub's.
inline constexpr std::int64_t kFutureCodeLimitSeconds = 48 * 60 * 60;

// The instant every release code counts from, 2023-05-23T00:00:00Z, in Unix
// seconds. A code is tenths of a second after it.
inline constexpr std::int64_t kCodeEpochUnixSeconds = 1684800000;

// The Unix second a release code names. Codes are at most 18 digits
// (VersionGrammar.h), so this cannot overflow.
inline constexpr std::int64_t CodeUnixSeconds(std::uint64_t code) {
  return kCodeEpochUnixSeconds + static_cast<std::int64_t>(code / 10);
}

struct ReleaseAsset {
  std::string name;
  std::string url;     // browser_download_url
  std::string digest;  // the API's `sha256:<hex>`, verbatim
};

struct Release {
  std::string tag;  // tag_name, with its v (and a test feed's prefix)
  bool draft = false;
  bool prerelease = false;
  bool immutable = false;
  std::vector<ReleaseAsset> assets;
};

struct Selection {
  // The newest release that counts and carries this product's MSI for this
  // architecture, offerable or not: the developer screen names it either way.
  std::uint64_t newestCode = 0;
  std::string newestVersion;  // v-less

  // The newest release this build can actually install and verify: own-arch
  // MSI attached, carrying a usable sha256 digest. code == 0 means none.
  std::uint64_t code = 0;
  std::string version;  // v-less
  std::string tag;      // as minted, with the v (and a test feed's prefix)
  std::string assetName;
  std::string assetUrl;
  std::string digestHex;  // lowercase

  // Releases that parsed but could not be offered, with why, for the log.
  struct Skip {
    std::string tag;
    std::string reason;
  };
  std::vector<Skip> skipped;
};

// The MSI asset name the release pipeline uploads for `version` (v-less) on
// `arch` ("x64" or "arm64"): URnetwork-<version>-<arch>.msi.
inline std::string InstallerAssetName(std::string_view version, std::string_view arch) {
  std::string name = "URnetwork-";
  name.append(version);
  name.push_back('-');
  name.append(arch);
  name.append(".msi");
  return name;
}

// The release a tag names on `feed`: the tag without the feed's prefix and its
// v, and its code; nullopt when the tag is not one of the feed's.
struct FeedTag {
  std::string version;  // v-less
  std::uint64_t code = 0;
};
inline std::optional<FeedTag> ParseFeedTag(const Feed& feed, std::string_view tag) {
  if (tag.substr(0, feed.tagPrefix.size()) != feed.tagPrefix) return std::nullopt;
  tag.remove_prefix(feed.tagPrefix.size());
  const std::uint64_t code = version::ParseReleaseCode(tag);
  if (code == 0) return std::nullopt;
  if (!tag.empty() && tag.front() == 'v') tag.remove_prefix(1);
  return FeedTag{.version = std::string(tag), .code = code};
}

// The download URL a release of `feed` carries for `asset` under `tag`:
// https://github.com/<owner>/<repo>/releases/download/<tag>/<asset>. The tag
// and asset names come from the grammar, which has nothing to percent-encode.
inline std::string FeedAssetUrl(const Feed& feed, std::string_view tag, std::string_view asset) {
  std::string url = "https://github.com/";
  url.append(feed.owner);
  url.push_back('/');
  url.append(feed.repo);
  url.append("/releases/download/");
  url.append(tag);
  url.push_back('/');
  url.append(asset);
  return url;
}

// Whether a release's download URL is the one its feed, tag and asset name.
// Matched whole: a URL that only starts with it names something else.
inline bool IsFeedAssetUrl(const Feed& feed, std::string_view tag, std::string_view asset,
                           std::string_view url) {
  return url == FeedAssetUrl(feed, tag, asset);
}

namespace selection_detail {

inline bool EndsWith(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

}  // namespace selection_detail

// The release to offer from `releases` (the API's order) for `arch`, on
// `feed`, judged against `serverUnixSeconds`, the release list's Date header.
inline Selection SelectRelease(std::vector<Release> const& releases, std::string_view arch,
                               const Feed& feed, std::int64_t serverUnixSeconds) {
  Selection s;
  for (auto const& rel : releases) {
    if (rel.draft) continue;
    if (rel.prerelease && !feed.acceptAnyPrerelease &&
        !(feed.acceptBetaPrereleases && selection_detail::EndsWith(rel.tag, "-beta"))) {
      continue;
    }
    const std::optional<FeedTag> tag = ParseFeedTag(feed, rel.tag);
    if (!tag) continue;
    if (feed.requireImmutable && !rel.immutable) {
      s.skipped.push_back({rel.tag, "is not an immutable release"});
      continue;
    }
    if (CodeUnixSeconds(tag->code) > serverUnixSeconds + kFutureCodeLimitSeconds) {
      s.skipped.push_back({rel.tag, "has a future code (more than 48 h after the server's date)"});
      continue;
    }

    const std::string name = InstallerAssetName(tag->version, arch);
    const ReleaseAsset* match = nullptr;
    for (auto const& asset : rel.assets) {
      if (asset.name == name) match = &asset;
    }
    if (!match || match->url.empty()) {
      s.skipped.push_back({rel.tag, "lacks " + name});
      continue;
    }
    if (tag->code > s.newestCode) {
      s.newestCode = tag->code;
      s.newestVersion = tag->version;
    }
    if (tag->code <= s.code) continue;
    // URL and expected hash from the same asset object: the digest is GitHub's
    // own upload-time SHA-256 for exactly the bytes this URL serves.
    std::string digestHex = DigestHexFromAssetDigest(match->digest);
    if (digestHex.empty()) {
      s.skipped.push_back({rel.tag, "lacks a usable digest for " + name});
      continue;
    }
    s.code = tag->code;
    s.version = tag->version;
    s.tag = rel.tag;
    s.assetName = name;
    s.assetUrl = match->url;
    s.digestHex = std::move(digestHex);
  }
  return s;
}

// The hosts a release download may redirect to: GitHub's release-asset
// storage.
inline constexpr std::string_view kAssetRedirectHosts[] = {
    "release-assets.githubusercontent.com",
    "objects.githubusercontent.com",
};

// Whether a download's redirect Location may be followed: https to one of
// kAssetRedirectHosts on the default port, with a path, no user info and
// nothing a lenient parser could read as another host. Anything else,
// including a URL this cannot parse, is refused.
inline bool IsAllowedAssetRedirect(std::string_view url) {
  constexpr std::string_view kScheme = "https://";
  if (url.substr(0, kScheme.size()) != kScheme) return false;
  for (const char c : url) {
    // controls, spaces and the characters a URL parser may treat as separators
    if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) >= 0x7f ||
        c == '\\' || c == '"' || c == '<' || c == '>' || c == '`') {
      return false;
    }
  }
  std::string_view rest = url.substr(kScheme.size());
  const std::size_t authorityEnd = rest.find_first_of("/?#");
  if (authorityEnd == std::string_view::npos || rest[authorityEnd] != '/') return false;
  std::string_view authority = rest.substr(0, authorityEnd);
  if (authority.find('@') != std::string_view::npos) return false;
  if (const std::size_t colon = authority.find(':'); colon != std::string_view::npos) {
    if (authority.substr(colon + 1) != "443") return false;
    authority = authority.substr(0, colon);
  }
  for (const std::string_view host : kAssetRedirectHosts) {
    if (EqualsAsciiCaseless(authority, host)) return true;
  }
  return false;
}

namespace msi_detail {

struct CivilDate {
  std::int64_t year = 0;
  unsigned month = 0;
  unsigned day = 0;
};

// The proleptic Gregorian date `days` after 1970-01-01 (Howard Hinnant's
// civil_from_days).
inline constexpr CivilDate CivilFromDays(std::int64_t days) {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const auto dayOfEra = static_cast<unsigned>(days - era * 146097);
  const unsigned yearOfEra =
      (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
  const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
  const unsigned monthIndex = (5 * dayOfYear + 2) / 153;
  const unsigned day = dayOfYear - (153 * monthIndex + 2) / 5 + 1;
  const unsigned month = monthIndex < 10 ? monthIndex + 3 : monthIndex - 9;
  const std::int64_t year = static_cast<std::int64_t>(yearOfEra) + era * 400 + (month <= 2 ? 1 : 0);
  return CivilDate{.year = year, .month = month, .day = day};
}

}  // namespace msi_detail

// The MSI ProductVersion a release build of `code` carries, as
// tools/UrVersion.ps1 derives it: for the UTC instant code/10 seconds after
// the founding, (Y-2000).M.((D-1)*2048 + floor(secondOfDay*2048/86400)).
// Empty for code 0, which no release carries, and past 2255, where the first
// field leaves its byte.
inline std::string UrMsiVersion(std::uint64_t code) {
  if (code == 0) return {};
  const std::int64_t seconds = CodeUnixSeconds(code);
  const std::int64_t days = seconds / 86400;
  const std::int64_t secondOfDay = seconds % 86400;
  const msi_detail::CivilDate date = msi_detail::CivilFromDays(days);
  const std::int64_t major = date.year - 2000;
  if (major < 0 || major > 255) return {};
  const std::int64_t patch =
      static_cast<std::int64_t>(date.day - 1) * 2048 + secondOfDay * 2048 / 86400;
  return std::to_string(major) + "." + std::to_string(date.month) + "." + std::to_string(patch);
}

}  // namespace urnw::update
