// Which published release the update checker offers, decided pure.
//
// The official Windows releases are the urnetwork/build GitHub releases that
// build/all/run.sh mints once per build: tag `v<YYYY.M.D>-<code>`, one MSI per
// architecture attached as `URnetwork-<YYYY.M.D>-<code>-<x64|arm64>.msi`
// (require_windows_artifacts + the github_release_upload loop). The same repo
// also carries android-only PRE-releases at code+2 / code+3 (the F-Droid
// reproducible-build variants), which outrank the real release by code and
// carry no MSI — so prereleases are skipped outright rather than merely
// failing the asset match, or the developer line would name an android build
// as "the newest release".
//
// The checker turns the releases JSON into these plain structs and asks
// SelectRelease; the decision itself (tag grammar, asset name, digest) touches
// no Windows headers, so tools/update-release-tests.cpp runs it on any host
// against the names the release pipeline actually publishes.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "UpdateFormats.h"
#include "VersionGrammar.h"

namespace urnw::update {

struct ReleaseAsset {
  std::string name;
  std::string url;     // browser_download_url
  std::string digest;  // the API's `sha256:<hex>`, verbatim
};

struct Release {
  std::string tag;  // tag_name, with its v
  bool draft = false;
  bool prerelease = false;
  std::vector<ReleaseAsset> assets;
};

struct Selection {
  // The newest non-draft, non-prerelease tag that parses, offerable or not —
  // the developer screen names it either way.
  std::uint64_t newestCode = 0;
  std::string newestVersion;  // v-less

  // The newest release this build can actually install and verify: own-arch
  // MSI attached, carrying a usable sha256 digest. code == 0 means none.
  std::uint64_t code = 0;
  std::string version;  // v-less
  std::string tag;      // as minted, with the v
  std::string assetName;
  std::string assetUrl;
  std::string digestHex;  // lowercase

  // Releases that parsed but could not be offered, with why — for the log.
  struct Skip {
    std::string tag;
    std::string reason;
  };
  std::vector<Skip> skipped;
};

// The MSI asset name the release pipeline uploads for `version` (v-less) on
// `arch` ("x64" or "arm64"): URnetwork-<version>-<arch>.msi.
inline std::string InstallerAssetName(std::string_view version,
                                      std::string_view arch) {
  std::string name = "URnetwork-";
  name.append(version);
  name.push_back('-');
  name.append(arch);
  name.append(".msi");
  return name;
}

inline Selection SelectRelease(std::vector<Release> const& releases,
                               std::string_view arch) {
  Selection s;
  for (auto const& rel : releases) {
    if (rel.draft || rel.prerelease) continue;
    const std::uint64_t code = version::ParseReleaseCode(rel.tag);
    if (code == 0) continue;
    std::string ver = rel.tag;
    if (!ver.empty() && ver.front() == 'v') ver.erase(0, 1);
    if (code > s.newestCode) {
      s.newestCode = code;
      s.newestVersion = ver;
    }
    if (code <= s.code) continue;

    const std::string name = InstallerAssetName(ver, arch);
    const ReleaseAsset* match = nullptr;
    for (auto const& asset : rel.assets) {
      if (asset.name == name) match = &asset;
    }
    if (!match || match->url.empty()) {
      s.skipped.push_back({rel.tag, "lacks " + name});
      continue;
    }
    // URL and expected hash from the SAME asset object: the digest is
    // GitHub's own upload-time SHA-256 for exactly the bytes this URL serves.
    std::string digestHex = DigestHexFromAssetDigest(match->digest);
    if (digestHex.empty()) {
      s.skipped.push_back({rel.tag, "lacks a usable digest for " + name});
      continue;
    }
    s.code = code;
    s.version = ver;
    s.tag = rel.tag;
    s.assetName = name;
    s.assetUrl = match->url;
    s.digestHex = std::move(digestHex);
  }
  return s;
}

}  // namespace urnw::update
