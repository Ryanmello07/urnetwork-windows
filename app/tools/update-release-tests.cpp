// Executable spec for the update checker's release decision
// (Common/ReleaseSelection.h) and its feed (App/Config.h kUpdateRepo): which
// repo is polled, which release is offered, and which asset is downloaded -
// run against the SAME headers the app compiles, on any host with a C++20
// compiler, with the tag and asset names build/all/run.sh actually publishes
// and the stable urnetwork/windows releases carry.
//
//   c++ -std=c++20 -I ../src/Common -I ../src/App update-release-tests.cpp -o /tmp/update-release-tests && /tmp/update-release-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "Config.h"
#include "ReleaseSelection.h"

using namespace urnw::update;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << "\n";
  }
}

void CheckEq(const std::string& expected, const std::string& actual, const std::string& what) {
  Check(expected == actual, what + ": expected \"" + expected + "\", got \"" + actual + "\"");
}

std::string Digest(char c) { return "sha256:" + std::string(64, c); }

ReleaseAsset Asset(std::string name, char digest = 'a') {
  return {name, "https://github.com/urnetwork/windows/releases/download/x/" + name,
          Digest(digest)};
}

// One official build as run.sh publishes it: v<version>, both MSIs next to the
// SDK and the other platforms' assets.
Release Official(const std::string& version) {
  return {"v" + version,
          false,
          false,
          {Asset("URnetworkSdk-" + version + ".aar"),
           Asset("URnetworkSdkWindows-" + version + ".zip"),
           Asset("URnetwork-" + version + "-x64.msi", 'b'),
           Asset("URnetwork-" + version + "-arm64.msi", 'c'),
           Asset("URnetwork-" + version + ".pkg")}};
}

// The F-Droid reproducible-build prerelease run.sh mints at code+2 / code+3.
Release AndroidPrerelease(const std::string& version) {
  return {"v" + version,
          false,
          true,
          {Asset("com.bringyour.network-" + version + "-github-arm64-v8a-release.apk")}};
}

}  // namespace

int main() {
  // ---- the feed: the stable urnetwork/windows releases — not the nightly
  // build repo, never a personal fork ----
  const std::wstring_view repo(urnw::config::kUpdateRepo);
  Check(repo == L"urnetwork/windows",
        "the update checker polls the stable urnetwork/windows releases");
  Check(repo != L"urnetwork/build",
        "urnetwork/build holds nightly builds, not the stable feed");
  Check(repo.starts_with(L"urnetwork/"), "the feed is an official urnetwork repo");

  // ---- asset names: run.sh require_windows_artifacts ----
  CheckEq("URnetwork-2026.8.28-1031763440-x64.msi",
          InstallerAssetName("2026.8.28-1031763440", "x64"), "x64 MSI name");
  CheckEq("URnetwork-2026.8.28-1031763440-arm64.msi",
          InstallerAssetName("2026.8.28-1031763440", "arm64"), "arm64 MSI name");

  // ---- a real release list, newest first as the API returns it ----
  const std::vector<Release> releases = {
      AndroidPrerelease("2026.9.22-1053244733"),
      AndroidPrerelease("2026.9.22-1053244732"),
      Official("2026.9.22-1053244730"),
      AndroidPrerelease("2026.8.28-1031763443"),
      Official("2026.8.28-1031763440"),
  };
  {
    const Selection s = SelectRelease(releases, "x64");
    Check(s.code == 1053244730, "offers the newest official release, not an android prerelease");
    CheckEq("2026.9.22-1053244730", s.version, "offered version is v-less");
    CheckEq("v2026.9.22-1053244730", s.tag, "offered tag keeps its v");
    CheckEq("URnetwork-2026.9.22-1053244730-x64.msi", s.assetName, "own-arch MSI");
    CheckEq("https://github.com/urnetwork/windows/releases/download/x/"
            "URnetwork-2026.9.22-1053244730-x64.msi",
            s.assetUrl, "download URL comes from the matched asset");
    CheckEq(std::string(64, 'b'), s.digestHex, "digest comes from the matched asset");
    Check(s.newestCode == 1053244730, "newest ignores prereleases");
    CheckEq("2026.9.22-1053244730", s.newestVersion, "newest version");
  }
  {
    const Selection s = SelectRelease(releases, "arm64");
    CheckEq("URnetwork-2026.9.22-1053244730-arm64.msi", s.assetName, "arm64 picks its own MSI");
    CheckEq(std::string(64, 'c'), s.digestHex, "arm64 digest");
  }

  // ---- what is not offered ----
  {
    Release draft = Official("2026.10.1-1060000000");
    draft.draft = true;
    Release noMsi = Official("2026.9.30-1059000000");
    noMsi.assets = {Asset("URnetwork-2026.9.30-1059000000.pkg")};
    Release badDigest = Official("2026.9.29-1058000000");
    for (auto& a : badDigest.assets) a.digest = "sha512:" + std::string(64, 'b');
    Release oldZip = {"v2026.9.28-1057000000", false, false,
                      {Asset("URnetwork-v2026.9.28-1057000000-windows-x64-portable.zip")}};
    const Selection s = SelectRelease(
        {draft, noMsi, badDigest, oldZip, Official("2026.9.22-1053244730")}, "x64");
    Check(s.code == 1053244730, "drafts, MSI-less, digest-less and zip-only releases are skipped");
    Check(s.newestCode == 1059000000, "newest names the newest parsed non-draft release");
    Check(s.skipped.size() == 3, "the three unverifiable releases are reported as skipped");
  }
  {
    const Selection s = SelectRelease({}, "x64");
    Check(s.code == 0 && s.newestCode == 0 && s.assetUrl.empty(), "empty list offers nothing");
    const Selection t = SelectRelease({{"latest", false, false, {}}}, "x64");
    Check(t.code == 0 && t.newestCode == 0, "a tag outside the grammar is ignored");
  }

  std::cout << (gFailures == 0 ? "PASS" : "FAIL") << " update-release-tests: " << gCases
            << " checks, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
