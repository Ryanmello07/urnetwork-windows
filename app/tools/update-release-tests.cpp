// Executable spec for the update's pure decisions, run against the same
// headers the tray app and the update helper compile, on any host with a C++20
// compiler:
//   - which feed is polled (Common/ReleaseSelection.h kFeeds), and which
//     release is offered on it (SelectRelease), with the tag and asset names
//     build/all/run.sh actually publishes;
//   - the MSI ProductVersion a release code must carry (UrMsiVersion), against
//     the vectors tools/UrVersion.ps1's Go oracle uses, and against a file of
//     the oracle's own answers when one is given;
//   - the download URL a release may name, and the hosts its redirect may go
//     to;
//   - which install locations an elevated process may run from
//     (Common/InstallLocation.h);
//   - what the update helper's exit code means, and when the tray app shows
//     a report (Common/UpdateResult.h);
//   - what the helper decides before msiexec runs, which variables it drops,
//     and how the tray app's wait on it ends (Common/UpdateApply.h);
//   - when the tray app asks GitHub again after a refusal, and when it says
//     its checks have not worked (Common/UpdateSchedule.h).
//
//   c++ -std=c++20 -I ../src/Common update-release-tests.cpp -o /tmp/update-release-tests
//   /tmp/update-release-tests [<file of "code msi-version" lines>]
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "InstallLocation.h"
#include "ReleaseSelection.h"
#include "UpdateApply.h"
#include "UpdateResult.h"
#include "UpdateSchedule.h"

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

// The release list's Date header in every selection below: 2026-10-06T00:00:00Z.
constexpr std::int64_t kServerUnix = 1791244800;

std::string Narrow(const std::wstring& text) { return std::string(text.begin(), text.end()); }

std::string Hex(std::uint32_t value) {
  std::ostringstream text;
  text << "0x" << std::hex << value;
  return text.str();
}

std::string Digest(char c) { return "sha256:" + std::string(64, c); }

ReleaseAsset Asset(const std::string& tag, const std::string& name, char digest = 'a') {
  return {.name = name,
          .url = "https://github.com/urnetwork/windows/releases/download/" + tag + "/" + name,
          .digest = Digest(digest)};
}

// One official build as run.sh publishes it: v<version>, both MSIs next to the
// SDK and the other platforms' assets, immutable.
Release Official(const std::string& version) {
  const std::string tag = "v" + version;
  return {.tag = tag,
          .draft = false,
          .prerelease = false,
          .immutable = true,
          .assets = {Asset(tag, "URnetworkSdk-" + version + ".aar"),
                     Asset(tag, "URnetworkSdkWindows-" + version + ".zip"),
                     Asset(tag, "URnetwork-" + version + "-x64.msi", 'b'),
                     Asset(tag, "URnetwork-" + version + "-arm64.msi", 'c'),
                     Asset(tag, "URnetwork-" + version + ".pkg")}};
}

// The F-Droid reproducible-build prerelease run.sh mints at code+2 / code+3.
Release AndroidPrerelease(const std::string& version) {
  const std::string tag = "v" + version;
  return {.tag = tag,
          .draft = false,
          .prerelease = true,
          .immutable = true,
          .assets = {Asset(tag, "com.bringyour.network-" + version +
                                    "-github-arm64-v8a-release.apk")}};
}

// The code of the UTC instant `unix` seconds.
std::uint64_t CodeAt(std::int64_t unix) {
  return static_cast<std::uint64_t>(unix - kCodeEpochUnixSeconds) * 10;
}

// A feed that takes -beta prereleases, and one shaped like a runner test's.
constexpr Feed kBetaLikeFeed{.id = "beta-like",
                             .numericRepoId = 1,
                             .owner = "example",
                             .repo = "example",
                             .acceptBetaPrereleases = true,
                             .requireImmutable = false,
                             .tagPrefix = "",
                             .acceptAnyPrerelease = false};
constexpr Feed kRunnerLikeFeed{.id = "runner-test",
                               .numericRepoId = 2,
                               .owner = "example",
                               .repo = "example",
                               .acceptBetaPrereleases = false,
                               .requireImmutable = false,
                               .tagPrefix = "runner-test-",
                               .acceptAnyPrerelease = true};

void FeedTable() {
  // The official releases: the stable urnetwork/windows releases, addressed by
  // the id GitHub gave that repository. Not the nightly build repo, and never
  // a personal fork.
  const Feed* official = FeedById("official");
  Check(official != nullptr, "the official feed is in the table");
  if (!official) return;
  Check(official->owner == "urnetwork" && official->repo == "windows",
        "the update checker polls the stable urnetwork/windows releases");
  Check(official->numericRepoId == 1297133846,
        "the official feed is repository 1297133846, urnetwork/windows' own id");
  Check(official->repo != "build" && official->numericRepoId != 936244679,
        "urnetwork/build holds nightly builds, not the stable feed");
  Check(official->owner == "urnetwork", "the feed is an official urnetwork repo");
  Check(!official->acceptBetaPrereleases && !official->acceptAnyPrerelease,
        "the official feed takes no prerelease");
  Check(official->requireImmutable, "the official feed requires immutable releases");
  Check(official->tagPrefix.empty(), "the official feed's tags carry no prefix");
  Check(std::size(kFeeds) == 1, "the table holds the official feed alone");
  Check(FeedById("beta") == nullptr && FeedById("") == nullptr, "no other channel resolves");
  Check(kFeeds[0].numericRepoId == kOfficialFeed.numericRepoId &&
            kFeeds[0].owner == kOfficialFeed.owner && kFeeds[0].repo == kOfficialFeed.repo,
        "the table's official row is kOfficialFeed");
}

void AssetNames() {
  CheckEq("URnetwork-2026.8.28-1031763440-x64.msi",
          InstallerAssetName("2026.8.28-1031763440", "x64"), "x64 MSI name");
  CheckEq("URnetwork-2026.8.28-1031763440-arm64.msi",
          InstallerAssetName("2026.8.28-1031763440", "arm64"), "arm64 MSI name");
}

void RealReleaseList() {
  // newest first, as the API returns it
  const std::vector<Release> releases = {
      AndroidPrerelease("2026.9.22-1053244733"),
      AndroidPrerelease("2026.9.22-1053244732"),
      Official("2026.9.22-1053244730"),
      AndroidPrerelease("2026.8.28-1031763443"),
      Official("2026.8.28-1031763440"),
  };
  {
    const Selection s = SelectRelease(releases, "x64", kOfficialFeed, kServerUnix);
    Check(s.code == 1053244730, "offers the newest official release, not an android prerelease");
    CheckEq("2026.9.22-1053244730", s.version, "offered version is v-less");
    CheckEq("v2026.9.22-1053244730", s.tag, "offered tag keeps its v");
    CheckEq("URnetwork-2026.9.22-1053244730-x64.msi", s.assetName, "own-arch MSI");
    CheckEq("https://github.com/urnetwork/windows/releases/download/v2026.9.22-1053244730/"
            "URnetwork-2026.9.22-1053244730-x64.msi",
            s.assetUrl, "download URL comes from the matched asset");
    CheckEq(std::string(64, 'b'), s.digestHex, "digest comes from the matched asset");
    Check(s.newestCode == 1053244730, "newest ignores prereleases");
    CheckEq("2026.9.22-1053244730", s.newestVersion, "newest version");
  }
  {
    const Selection s = SelectRelease(releases, "arm64", kOfficialFeed, kServerUnix);
    CheckEq("URnetwork-2026.9.22-1053244730-arm64.msi", s.assetName, "arm64 picks its own MSI");
    CheckEq(std::string(64, 'c'), s.digestHex, "arm64 digest");
  }
}

void NotOffered() {
  Release draft = Official("2026.10.1-1060000000");
  draft.draft = true;
  Release noMsi = Official("2026.9.30-1059000000");
  noMsi.assets = {Asset(noMsi.tag, "URnetwork-2026.9.30-1059000000.pkg")};
  Release badDigest = Official("2026.9.29-1058000000");
  for (auto& a : badDigest.assets) a.digest = "sha512:" + std::string(64, 'b');
  Release oldZip = Official("2026.9.28-1057000000");
  oldZip.assets = {Asset(oldZip.tag, "URnetwork-v2026.9.28-1057000000-windows-x64-portable.zip")};
  const Selection s = SelectRelease({draft, noMsi, badDigest, oldZip, Official("2026.9.22-1053244730")},
                                    "x64", kOfficialFeed, kServerUnix);
  Check(s.code == 1053244730, "drafts, MSI-less, digest-less and zip-only releases are skipped");
  Check(s.newestCode == 1058000000,
        "newest names the newest release carrying this product's MSI, offerable or not");
  Check(s.skipped.size() == 3, "the three unverifiable releases are reported as skipped");

  const Selection empty = SelectRelease({}, "x64", kOfficialFeed, kServerUnix);
  Check(empty.code == 0 && empty.newestCode == 0 && empty.assetUrl.empty(),
        "empty list offers nothing");
  Release latest = Official("2026.9.22-1053244730");
  latest.tag = "latest";
  const Selection t = SelectRelease({latest}, "x64", kOfficialFeed, kServerUnix);
  Check(t.code == 0 && t.newestCode == 0, "a tag outside the grammar is ignored");
}

void ImmutableRequired() {
  Release mutableRelease = Official("2026.10.1-1060587890");
  mutableRelease.immutable = false;
  const Selection s = SelectRelease({mutableRelease, Official("2026.9.22-1053244730")}, "x64",
                                    kOfficialFeed, kServerUnix);
  Check(s.code == 1053244730, "a release that is not immutable is not offered on the official feed");
  Check(s.newestCode == 1053244730, "nor named as the newest");
  Check(s.skipped.size() == 1 && s.skipped[0].tag == "v2026.10.1-1060587890" &&
            s.skipped[0].reason.find("immutable") != std::string::npos,
        "and it is reported as not immutable");

  const Selection beta = SelectRelease({mutableRelease}, "x64", kBetaLikeFeed, kServerUnix);
  Check(beta.code == 1060587890, "a feed that does not require it offers a mutable release");
}

void Prereleases() {
  Release beta = Official("2026.10.3-1062717970-beta");
  beta.prerelease = true;
  Release rc = Official("2026.10.3-1062717980");
  rc.prerelease = true;
  {
    const Selection s = SelectRelease({rc, beta, Official("2026.10.1-1060587890")}, "x64",
                                      kOfficialFeed, kServerUnix);
    Check(s.code == 1060587890 && s.newestCode == 1060587890,
          "the official feed skips every prerelease, -beta ones included");
  }
  {
    const Selection s = SelectRelease({rc, beta, Official("2026.10.1-1060587890")}, "x64",
                                      kBetaLikeFeed, kServerUnix);
    Check(s.code == 1062717970, "a beta feed offers a -beta prerelease");
    Check(s.newestCode == 1062717970, "and skips any other prerelease");
  }
  {
    Release prefixed = Official("2026.10.3-1062717990");
    prefixed.tag = "runner-test-" + prefixed.tag;
    prefixed.prerelease = true;
    const std::string asset = "URnetwork-2026.10.3-1062717990-x64.msi";
    prefixed.assets = {{.name = asset,
                        .url = FeedAssetUrl(kRunnerLikeFeed, prefixed.tag, asset),
                        .digest = Digest('d')}};
    const Selection s = SelectRelease({prefixed, rc, Official("2026.10.1-1060587890")}, "x64",
                                      kRunnerLikeFeed, kServerUnix);
    Check(s.code == 1062717990 && s.tag == prefixed.tag,
          "a runner test feed offers its prefixed prerelease");
    CheckEq("2026.10.3-1062717990", s.version, "its version drops the prefix and the v");
    Check(s.newestCode == 1062717990, "tags without the prefix are not the runner feed's");
    const Selection official = SelectRelease({prefixed}, "x64", kOfficialFeed, kServerUnix);
    Check(official.code == 0 && official.newestCode == 0,
          "the official feed never reads a prefixed tag");
  }
}

void FutureCodes() {
  // the promised two days, written out: the constant itself could shrink
  const std::uint64_t atLimit = CodeAt(kServerUnix + 48 * 60 * 60);
  const std::uint64_t pastLimit = atLimit + 10;
  auto release = [](std::uint64_t code) {
    return Official("2026.10.8-" + std::to_string(code));
  };
  {
    const Selection s = SelectRelease({release(pastLimit), Official("2026.10.1-1060587890")},
                                      "x64", kOfficialFeed, kServerUnix);
    Check(s.code == 1060587890, "a code more than 48 h past the server's date is not offered");
    Check(s.newestCode == 1060587890, "nor named as the newest");
    Check(s.skipped.size() == 1 && s.skipped[0].reason.find("future code") != std::string::npos,
          "and it is logged as a future code");
  }
  {
    const Selection s = SelectRelease({release(atLimit)}, "x64", kOfficialFeed, kServerUnix);
    Check(s.code == atLimit, "a code exactly 48 h past the server's date is offered");
  }
  {
    // A one-digit typo of a real code lands decades out.
    const Selection s = SelectRelease({Official("2057.1.23-10627179700")}, "x64", kOfficialFeed,
                                      kServerUnix);
    Check(s.code == 0, "a far-future code is never offered");
  }
  {
    // The cap is the server's clock, not this machine's: the same release is
    // fine a day later.
    const Selection s = SelectRelease({release(pastLimit)}, "x64", kOfficialFeed,
                                      kServerUnix + 24 * 60 * 60);
    Check(s.code == pastLimit, "the cap moves with the server's date");
  }
}

void NewestCountsOwnProduct() {
  // A newer release with the other platforms' assets only.
  Release otherPlatforms = Official("2026.10.2-1061000000");
  otherPlatforms.assets = {Asset(otherPlatforms.tag, "URnetworkSdk-2026.10.2-1061000000.aar"),
                           Asset(otherPlatforms.tag, "URnetwork-2026.10.2-1061000000.pkg")};
  const Selection s = SelectRelease({otherPlatforms, Official("2026.10.1-1060587890")}, "x64",
                                    kOfficialFeed, kServerUnix);
  Check(s.newestCode == 1060587890,
        "newest counts only releases carrying this product's MSI for this architecture");
  // the arm64 MSI alone does not count for x64
  Release armOnly = Official("2026.10.2-1061000010");
  armOnly.assets = {Asset(armOnly.tag, "URnetwork-2026.10.2-1061000010-arm64.msi")};
  const Selection x64 = SelectRelease({armOnly, Official("2026.10.1-1060587890")}, "x64",
                                      kOfficialFeed, kServerUnix);
  Check(x64.newestCode == 1060587890, "another architecture's MSI does not count");
  const Selection arm = SelectRelease({armOnly, Official("2026.10.1-1060587890")}, "arm64",
                                      kOfficialFeed, kServerUnix);
  Check(arm.newestCode == 1061000010 && arm.code == 1061000010, "for its own architecture it does");
}

// tests/ur_version_test.go urFixedVectors, computed with Python's datetime.
struct MsiVector {
  std::uint64_t code;
  const char* msi;
};
constexpr MsiVector kMsiVectors[] = {
    {1060587890, "26.10.1090"},   {1062717970, "26.10.6139"},   {1060587895, "26.10.1090"},
    {1060587899, "26.10.1090"},   {1060127995, "26.9.61439"},   {1060127999, "26.9.61439"},
    {824255995, "25.12.63487"},   {1060363020, "26.10.557"},    {1060128000, "26.10.0"},
    {1060128420, "26.10.0"},      {1060128430, "26.10.1"},      {1034207990, "26.8.63487"},
    {1034208000, "26.9.0"},       {824255990, "25.12.63487"},   {824256000, "26.1.0"},
    {244080000, "24.2.58368"},    {244511990, "24.2.59391"},    {244512000, "24.3.0"},
    {1086911990, "26.10.63487"},  {1086912000, "26.11.0"},      {1, "23.5.45056"},
    {9, "23.5.45056"},            {73404575990, "255.12.63487"},
};

void MsiVersions(const char* oracleFile) {
  for (const MsiVector& v : kMsiVectors) {
    CheckEq(v.msi, UrMsiVersion(v.code), "UrMsiVersion(" + std::to_string(v.code) + ")");
  }
  CheckEq("", UrMsiVersion(0), "code 0 has no MSI version");
  CheckEq("", UrMsiVersion(73404576000), "2256 is past the last ProductVersion the layout holds");
  if (!oracleFile) return;
  // The oracle's own answers: "<code> <msi version>", or "<code> -" for a code
  // past the layout.
  std::ifstream in(oracleFile);
  Check(static_cast<bool>(in), std::string("the oracle's vectors open: ") + oracleFile);
  std::string line;
  int vectors = 0;
  int mismatches = 0;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    std::istringstream fields(line);
    std::uint64_t code = 0;
    std::string want;
    fields >> code >> want;
    if (want == "-") want.clear();
    ++vectors;
    if (UrMsiVersion(code) != want && ++mismatches <= 10) {
      CheckEq(want, UrMsiVersion(code), "UrMsiVersion(" + std::to_string(code) + ") against the oracle");
    }
  }
  Check(mismatches == 0, std::to_string(mismatches) + " of the oracle's vectors disagree");
  Check(vectors >= 1000, "the oracle gave " + std::to_string(vectors) + " vectors");
  std::cout << "  " << vectors << " oracle vectors\n";
}

void DownloadUrls() {
  const std::string tag = "v2026.10.1-1060587890";
  const std::string asset = "URnetwork-2026.10.1-1060587890-x64.msi";
  const std::string url = "https://github.com/urnetwork/windows/releases/download/" + tag + "/" + asset;
  CheckEq(url, FeedAssetUrl(kOfficialFeed, tag, asset), "the official feed's download URL");
  Check(IsFeedAssetUrl(kOfficialFeed, tag, asset, url), "the feed's own URL is accepted");
  for (const std::string& bad : {
           url + "?x=1",
           url + "/",
           "http://github.com/urnetwork/windows/releases/download/" + tag + "/" + asset,
           "https://github.com/urnetwork/build/releases/download/" + tag + "/" + asset,
           "https://github.com/someone/windows/releases/download/" + tag + "/" + asset,
           "https://github.com/urnetwork/windows/releases/download/v2026.10.1-1060587891/" + asset,
           "https://github.com/urnetwork/windows/releases/download/" + tag + "/URnetwork-x.msi",
           "https://evil.example/urnetwork/windows/releases/download/" + tag + "/" + asset,
           "https://github.com.evil.example/urnetwork/windows/releases/download/" + tag + "/" + asset,
           "https://github.com/urnetwork/windows/releases/download/" + tag + "/../" + asset,
           std::string(),
       }) {
    Check(!IsFeedAssetUrl(kOfficialFeed, tag, asset, bad), "refused download URL: " + bad);
  }
}

void Redirects() {
  for (const char* good : {
           "https://release-assets.githubusercontent.com/github-production-release-asset/"
           "936244679/60615c0d?sp=r&sv=2018-11-09&sr=b&spr=https",
           "https://objects.githubusercontent.com/github-production-release-asset-2e65be/1/2",
           "https://RELEASE-ASSETS.githubusercontent.com/x",
           "https://release-assets.githubusercontent.com:443/x",
           "https://release-assets.githubusercontent.com/",
       }) {
    Check(IsAllowedAssetRedirect(good), std::string("allowed redirect: ") + good);
  }
  for (const char* bad : {
           "http://release-assets.githubusercontent.com/x",
           "HTTPS://release-assets.githubusercontent.com/x",
           "https://release-assets.githubusercontent.com.evil.example/x",
           "https://release-assets.githubusercontent.com./x",
           "https://evil.example/release-assets.githubusercontent.com",
           "https://evil.example/?release-assets.githubusercontent.com",
           "https://user@release-assets.githubusercontent.com/x",
           "https://release-assets.githubusercontent.com@evil.example/x",
           "https://release-assets.githubusercontent.com:8443/x",
           "https://release-assets.githubusercontent.com:/x",
           "https://release-assets.githubusercontent.com",
           "https://release-assets.githubusercontent.com?x",
           "https://release-assets.githubusercontent.com#x",
           "https://release-assets.githubusercontent.com\\@evil.example/",
           "https://release-assets.githubusercontent.com/x y",
           "https://release-assets.githubusercontent.com/x\ty",
           "https://xrelease-assets.githubusercontent.com/x",
           "https://githubusercontent.com/x",
           "https://github.com/urnetwork/windows/releases/download/v1/x.msi",
           "https://release-assets.githubusercontent.com\xc2\xa0/x",
           "",
       }) {
    Check(!IsAllowedAssetRedirect(bad), std::string("refused redirect: ") + bad);
  }
}

void InstallLocations() {
  using urnw::install::InstallPathInfo;
  using urnw::install::InstallRights;
  using urnw::install::IsAdminOnlyInstallDir;
  // FILE_GENERIC_READ | FILE_GENERIC_EXECUTE: what Program Files' Users ACE
  // grants (RX), and Modify and Full control over it
  constexpr std::uint32_t kUsersRx = 0x001200A9;
  constexpr std::uint32_t kUsersModify = 0x001301BF;
  constexpr std::uint32_t kFullControl = 0x001F01FF;
  constexpr InstallPathInfo kProgramFiles{
      .underProgramFiles = true, .resolvesToItself = true, .reparsePoint = false};

  Check(IsAdminOnlyInstallDir(kProgramFiles, {.folder = kUsersRx, .executable = kUsersRx}),
        "Program Files with Users RX is an admin-only install location");
  Check(IsAdminOnlyInstallDir(kProgramFiles, {.folder = 0, .executable = 0}),
        "no rights at all is admin-only too");
  Check(!IsAdminOnlyInstallDir({.underProgramFiles = false, .resolvesToItself = true},
                               {.folder = kUsersRx, .executable = kUsersRx}),
        "a user folder is refused, whatever its ACL");
  Check(!IsAdminOnlyInstallDir(kProgramFiles, {.folder = kUsersModify, .executable = kUsersRx}),
        "a folder Users can modify is refused");
  Check(!IsAdminOnlyInstallDir(kProgramFiles, {.folder = kUsersRx, .executable = kUsersModify}),
        "an executable Users can modify is refused");
  Check(!IsAdminOnlyInstallDir(kProgramFiles, {.folder = kFullControl, .executable = kFullControl}),
        "full control is refused");
  Check(!IsAdminOnlyInstallDir(
            {.underProgramFiles = true, .resolvesToItself = true, .reparsePoint = true},
            {.folder = kUsersRx, .executable = kUsersRx}),
        "a reparse point on the way is refused");
  Check(!IsAdminOnlyInstallDir({.underProgramFiles = true, .resolvesToItself = false},
                               {.folder = kUsersRx, .executable = kUsersRx}),
        "a path that resolves elsewhere is refused");
  for (const std::uint32_t bit :
       {urnw::install::kWriteData, urnw::install::kAppendData, urnw::install::kWriteEa,
        urnw::install::kDeleteChild, urnw::install::kWriteAttributes, urnw::install::kDelete,
        urnw::install::kWriteDac, urnw::install::kWriteOwner}) {
    Check(!IsAdminOnlyInstallDir(kProgramFiles, {.folder = kUsersRx | bit, .executable = kUsersRx}),
          "folder right " + Hex(bit) + " alone is refused");
    Check(!IsAdminOnlyInstallDir(kProgramFiles, {.folder = kUsersRx, .executable = kUsersRx | bit}),
          "executable right " + Hex(bit) + " alone is refused");
  }
}

void Outcomes() {
  Check(OutcomeOf(0) == Outcome::Installed, "exit 0 installed");
  Check(OutcomeOf(3010) == Outcome::RestartRequired, "3010 is restart to finish, not a failure");
  Check(OutcomeOf(1641) == Outcome::RestartRequired, "1641 is a restart too");
  for (const std::int64_t code : {1602, 1603, 1618, 1625, 1638, 1}) {
    Check(OutcomeOf(code) == Outcome::Failed, "msiexec " + std::to_string(code) + " failed");
  }
  for (const Refusal refusal :
       {Refusal::NotElevated, Refusal::NotInstalled, Refusal::DevBuild, Refusal::BadArguments,
        Refusal::Busy, Refusal::ReleaseList, Refusal::NotOffered, Refusal::Download,
        Refusal::Digest, Refusal::Package, Refusal::Staging, Refusal::InstallerNotStarted,
        Refusal::RateLimited}) {
    const auto code = static_cast<std::int64_t>(refusal);
    Check(IsRefusal(code) && OutcomeOf(code) == Outcome::Refused,
          "helper refusal " + std::to_string(code) + " is a refusal");
    Check(!KeepsPackage(code), "a refusal keeps no package");
  }
  Check(KeepsPackage(0) && KeepsPackage(3010) && KeepsPackage(1641),
        "an install that took keeps the package as the repair source");
  Check(!KeepsPackage(1603) && !KeepsPackage(1618), "a failed install deletes it");

  const UpdateResult good{.tag = "v2026.10.1-1060587890",
                          .code = 1060587890,
                          .exitCode = 0,
                          .finishedUtc = "2026-10-01T14:51:17Z"};
  Check(IsWellFormed(good), "a report the helper writes is well formed");
  UpdateResult bad = good;
  bad.code = 1060587891;
  Check(!IsWellFormed(bad), "a code that is not the tag's is refused");
  bad = good;
  bad.tag = "latest";
  Check(!IsWellFormed(bad), "a tag outside the grammar is refused");
  bad = good;
  bad.finishedUtc = "2026-10-01 14:51:17";
  Check(!IsWellFormed(bad), "a time that is not a UTC second is refused");
  bad = good;
  bad.exitCode = -1;
  Check(!IsWellFormed(bad), "a negative exit code is refused");
  bad = good;
  bad.exitCode = 0x100000000;
  Check(!IsWellFormed(bad), "an exit code no process returns is refused");
  UpdateResult prefixed = good;
  prefixed.tag = "runner-test-" + good.tag;
  Check(IsWellFormed(prefixed, "runner-test-") && !IsWellFormed(prefixed),
        "a runner test feed's tag reads with its prefix only");
}

void HelperDecisions() {
  // the tag argument
  Check(IsTagArgument(kOfficialFeed, "v2026.10.1-1060587890"), "an official tag is a tag argument");
  Check(IsTagArgument(kOfficialFeed, "2026.10.1-1060587890"), "so is one without its v");
  Check(IsTagArgument(kRunnerLikeFeed, "runner-test-v2026.10.1-1060587890"),
        "a runner test feed's prefixed tag is one of its");
  for (const char* bad : {
           "", "latest", "v2026.10.1-1060587890 ", " v2026.10.1-1060587890",
           "v2026.10.1-1060587890\"", "v2026.10.1-1060587890 /qn", "..", "../v2026.10.1-1060587890",
           "v2026.10.1-1060587890/x", "v2026.10.1-1060587890\\x", "-v2026.10.1-1060587890",
           "v2026.10.1-1060587890-rc", "runner-test-v2026.10.1-1060587890",
           "v2026.10.1-1060587890\xc2\xa0",
       }) {
    Check(!IsTagArgument(kOfficialFeed, bad), std::string("not an official tag argument: ") + bad);
  }
  Check(!IsTagArgument(kRunnerLikeFeed, "v2026.10.1-1060587890"),
        "a runner test feed takes only its prefixed tags");
  Check(!IsTagArgument(kOfficialFeed, "v2026.10.1-" + std::string(90, '1')),
        "a tag past the length cap is refused");

  // the selection must offer the tag, above this build
  Selection offered;
  offered.code = 1060587890;
  offered.tag = "v2026.10.1-1060587890";
  offered.assetName = "URnetwork-2026.10.1-1060587890-x64.msi";
  offered.assetUrl = FeedAssetUrl(kOfficialFeed, offered.tag, offered.assetName);
  offered.digestHex = std::string(64, 'a');
  Check(SelectionOffers(offered, "v2026.10.1-1060587890", 1053244730),
        "the list offering the tag above this build is an offer");
  Check(!SelectionOffers(offered, "v2026.10.2-1061000000", 1053244730),
        "a list that offers another tag is no offer of this one");
  Check(!SelectionOffers(offered, "v2026.10.1-1060587890", 1060587890),
        "a release no newer than this build is no offer");
  Check(!SelectionOffers(offered, "v2026.10.1-1060587890", 1070000000),
        "nor is one older than this build");
  Selection noDigest = offered;
  noDigest.digestHex.clear();
  Check(!SelectionOffers(noDigest, "v2026.10.1-1060587890", 1053244730),
        "an offer without a digest is no offer");
  Selection none;
  Check(!SelectionOffers(none, "", 0), "an empty selection offers nothing");

  // the package's identity
  CheckEq("{A7C1E2D3-4B5F-6081-9C2D-3E4F50617283}", std::string(kUpgradeCode), "the UpgradeCode");
  Check(PackageMatches("{A7C1E2D3-4B5F-6081-9C2D-3E4F50617283}", "26.10.1090", 1060587890),
        "this product at the release's ProductVersion matches");
  Check(PackageMatches("{a7c1e2d3-4b5f-6081-9c2d-3e4f50617283}", "26.10.1090", 1060587890),
        "the UpgradeCode compares without case");
  Check(!PackageMatches("{B7C1E2D3-4B5F-6081-9C2D-3E4F50617283}", "26.10.1090", 1060587890),
        "another product's UpgradeCode is refused");
  Check(!PackageMatches("{A7C1E2D3-4B5F-6081-9C2D-3E4F50617283}", "0.0.1", 1060587890),
        "an unstamped package under a release tag is refused");
  Check(!PackageMatches("{A7C1E2D3-4B5F-6081-9C2D-3E4F50617283}", "26.10.1089", 1060587890),
        "an older package re-uploaded under a newer tag is refused");
  Check(!PackageMatches("{A7C1E2D3-4B5F-6081-9C2D-3E4F50617283}", "", 0),
        "code 0 has no package");

  // msiexec's command line
  CheckEq("\"C:\\Windows\\system32\\msiexec.exe\" /i \"C:\\Program Files\\URnetwork\\updates\\"
          "v2026.10.1-1060587890\\URnetwork-2026.10.1-1060587890-x64.msi\" /passive /norestart "
          "/l*v \"C:\\Program Files\\URnetwork\\updates\\v2026.10.1-1060587890\\install.log\" "
          "UPDATE_RELAUNCH=1",
          Narrow(MsiexecCommandLine(
              L"C:\\Windows\\system32\\msiexec.exe",
              L"C:\\Program Files\\URnetwork\\updates\\v2026.10.1-1060587890\\"
              L"URnetwork-2026.10.1-1060587890-x64.msi",
              L"C:\\Program Files\\URnetwork\\updates\\v2026.10.1-1060587890\\install.log")),
          "msiexec installs passively, never restarts, logs verbosely and asks for the relaunch");

  // finishedUtc
  CheckEq("2026-10-01T12:46:29Z", FormatUtcSecond(CodeUnixSeconds(1060587890)),
          "a release code's instant as a UTC second");
  CheckEq("1970-01-01T00:00:00Z", FormatUtcSecond(0), "the epoch");
  CheckEq("2000-02-29T23:59:59Z", FormatUtcSecond(951868799), "a leap day's last second");
  Check(IsUtcSecond(FormatUtcSecond(kServerUnix)), "what FormatUtcSecond writes, IsUtcSecond reads");
}

// When the tray app shows a report, and whose report it is.
void Reports() {
  constexpr std::uint64_t kRelease = 1060587890;
  const auto report = [](std::int64_t exitCode) {
    return UpdateResult{.tag = "v2026.10.1-1060587890",
                        .code = kRelease,
                        .exitCode = exitCode,
                        .finishedUtc = "2026-10-01T14:51:17Z"};
  };
  const auto view = [&](std::int64_t exitCode, std::uint64_t own, bool live, bool restarted) {
    return ViewOfReport(report(exitCode), own, live, restarted);
  };
  constexpr std::uint64_t kOlder = 1053244730;
  constexpr std::uint64_t kNewer = 1070000000;
  // installed
  Check(view(0, kRelease, false, false) == ReportView::Installed,
        "an install reads as installed in the release it installed");
  Check(view(0, kRelease, true, false) == ReportView::Installed, "and so while live");
  Check(view(0, kOlder, true, false) == ReportView::RestartApp,
        "an install the old app outlived asks for URnetwork's restart");
  Check(view(0, kOlder, false, false) == ReportView::Hidden,
        "an older build launched after an install is told nothing");
  Check(view(0, kNewer, false, false) == ReportView::Hidden,
        "a newer build is told nothing about an older install");
  // installed up to a restart
  Check(view(3010, kRelease, false, false) == ReportView::RestartWindows,
        "3010 asks for Windows' restart until there has been one");
  Check(view(3010, kOlder, false, false) == ReportView::RestartWindows,
        "and so in the old exe that is still on disk");
  Check(view(1641, kRelease, false, false) == ReportView::RestartWindows, "1641 is a restart too");
  Check(view(3010, kRelease, false, true) == ReportView::Installed,
        "3010 after a restart reads as installed");
  Check(view(3010, kOlder, false, true) == ReportView::Hidden,
        "3010 after a restart, in an older build, is told nothing");
  Check(view(3010, kNewer, false, false) == ReportView::Hidden,
        "a newer build is told nothing about an older 3010");
  // not installed
  for (const std::int64_t code : {std::int64_t{1603}, static_cast<std::int64_t>(Refusal::Digest),
                                  static_cast<std::int64_t>(Refusal::NotOffered)}) {
    Check(view(code, kOlder, false, false) == ReportView::NotInstalled,
          "a failure " + std::to_string(code) + " shows while this build is older");
    Check(view(code, kOlder, true, false) == ReportView::NotInstalled, "and so while live");
    Check(view(code, kRelease, false, false) == ReportView::Hidden,
          "a failure " + std::to_string(code) + " is moot once the release runs");
    Check(view(code, kNewer, false, false) == ReportView::Hidden,
          "a failure " + std::to_string(code) + " is moot in a newer build");
  }

  // the UTC second, both ways
  for (const std::int64_t second : {std::int64_t{0}, std::int64_t{951868799}, std::int64_t{951868800},
                                    CodeUnixSeconds(1060587890), kServerUnix, std::int64_t{4102444799}}) {
    const std::optional<std::int64_t> parsed = ParseUtcSecond(FormatUtcSecond(second));
    Check(parsed && *parsed == second, "the UTC second of " + std::to_string(second) + " reads back");
  }
  Check(ParseUtcSecond("2026-10-01T14:51:17Z") == std::optional<std::int64_t>{1790866277},
        "2026-10-01T14:51:17Z is 1790866277");
  for (const char* bad : {"", "2026-10-01 14:51:17Z", "2026-13-01T00:00:00Z", "2026-00-01T00:00:00Z",
                          "2026-10-00T00:00:00Z", "2026-10-01T24:00:00Z", "2026-10-01T00:60:00Z",
                          "2026-10-01T00:00:60Z", "2026-10-01T00:00:00"}) {
    Check(!ParseUtcSecond(bad), std::string("not a UTC second: ") + bad);
  }

  // whose report
  const UpdateResult written = report(static_cast<std::int64_t>(Refusal::Digest));
  constexpr std::int64_t kStarted = 1790866200;  // 77 s before it finished
  Check(IsReportOfRun(written, "v2026.10.1-1060587890", 0x20000009, kStarted),
        "the report the helper wrote is this run's");
  Check(!IsReportOfRun(written, "v2026.10.1-1060587890",
                       static_cast<std::int64_t>(Refusal::NotInstalled), kStarted),
        "an earlier report is not the run that refused before writing one");
  Check(!IsReportOfRun(written, "v2026.10.2-1061000000", 0x20000009, kStarted),
        "another tag's report is not this run's");
  Check(!IsReportOfRun(written, "v2026.10.1-1060587890", 0x20000009, kStarted + 3600),
        "a report written before the helper started is not this run's");
  Check(IsReportOfRun(written, "v2026.10.1-1060587890", 0x20000009, 1790866277 + 120),
        "two minutes of clock set back are allowed");
  Check(!IsReportOfRun(written, "v2026.10.1-1060587890", 0x20000009, 1790866277 + 121),
        "and no more");
}

// Which variables the elevated helper drops, and how the tray's wait ends.
void HelperProcess() {
  for (const wchar_t* name : {L"URNETWORK_APP_ROOT", L"urnetwork_app_root", L"Urnetwork_Network_Host",
                              L"URNETWORK_NETWORK_ENV", L"URNETWORK_"}) {
    Check(IsAppOverrideName(name), "an app override: " + Narrow(name));
  }
  for (const wchar_t* name : {L"", L"PATH", L"URNETWORK", L"URNETWORKX_ROOT", L"_URNETWORK_APP_ROOT",
                              L"XURNETWORK_APP_ROOT", L"URNETWORK-APP-ROOT"}) {
    Check(!IsAppOverrideName(name), "not an app override: " + Narrow(name));
  }

  // The helper ends after three slices while the app runs on.
  int slices = 0;
  int asked = 0;
  HelperWait waited = AwaitHelper([&] { return ++slices == 3; }, [&] { ++asked; return false; });
  Check(waited == HelperWait::Ended && slices == 3 && asked == 2,
        "the wait ends with the helper, after the slices it took");
  // The app begins to exit at the fourth slice. (The helper here ends at the
  // hundredth, so a wait that never looks at the exit ends too, and fails
  // the check instead of hanging the spec.)
  slices = 0;
  asked = 0;
  waited = AwaitHelper([&] { return ++slices > 100; }, [&] { return ++asked >= 4; });
  Check(waited == HelperWait::AppExiting && slices == 4,
        "the wait ends as the app begins to exit, without the helper");
  // A helper that ends in the slice the app begins to exit is a helper that
  // ended: its report is the app's to read.
  waited = AwaitHelper([] { return true; }, [] { return true; });
  Check(waited == HelperWait::Ended, "an ended helper wins over an exit asked in the same slice");
}

RateLimit Limit(std::int64_t retryAfter, std::int64_t reset, bool exhausted, std::int64_t server) {
  RateLimit limit;
  limit.retryAfterSeconds = retryAfter;
  limit.resetUnixSeconds = reset;
  limit.exhausted = exhausted;
  limit.serverUnixSeconds = server;
  return limit;
}

void Schedule() {
  constexpr std::int64_t kCadence = 6 * 60 * 60;
  constexpr std::int64_t kDay = 24 * 60 * 60;

  // asking again after a refusal
  Check(NextCheckDelaySeconds(kCadence, RateLimit{}) == kCadence,
        "a refusal without headers keeps the cadence");
  Check(NextCheckDelaySeconds(0, RateLimit{}) == 0, "no header holds nothing");
  Check(NextCheckDelaySeconds(0, Limit(60, 0, false, kServerUnix)) == 60,
        "Retry-After holds the next request");
  Check(NextCheckDelaySeconds(kCadence, Limit(60, 0, false, kServerUnix)) == kCadence,
        "a short Retry-After does not bring the cadence forward");
  Check(NextCheckDelaySeconds(0, Limit(0, kServerUnix + 1800, true, kServerUnix)) == 1800,
        "a spent hour holds until GitHub's reset, measured on GitHub's clock");
  Check(NextCheckDelaySeconds(0, Limit(0, kServerUnix + 1800, false, kServerUnix)) == 0,
        "a reset time with requests left holds nothing");
  Check(NextCheckDelaySeconds(0, Limit(0, kServerUnix + 1800, true, 0)) == 0,
        "a reset without the server's date is not measured against this machine's clock");
  Check(NextCheckDelaySeconds(0, Limit(60, kServerUnix + 1800, true, kServerUnix)) == 1800,
        "the later of Retry-After and the reset holds");
  Check(NextCheckDelaySeconds(0, Limit(7200, kServerUnix + 1800, true, kServerUnix)) == 7200,
        "the later of the reset and Retry-After holds");
  Check(NextCheckDelaySeconds(0, Limit(10 * kDay, 0, false, kServerUnix)) == kDay,
        "no Retry-After holds checks for more than a day");
  Check(NextCheckDelaySeconds(0, Limit(0, kServerUnix + 10 * kDay, true, kServerUnix)) == kDay,
        "no reset time holds checks for more than a day");
  Check(NextCheckDelaySeconds(0, Limit(0, kServerUnix - 600, true, kServerUnix)) == 0,
        "a reset already past holds nothing");
  Check(NextCheckDelaySeconds(0, Limit(-5, 0, false, kServerUnix)) == 0,
        "a negative Retry-After holds nothing");
  Check(NextCheckDelaySeconds(2 * kDay, RateLimit{}) == 2 * kDay,
        "a cadence longer than a day is kept");

  // saying the checks have not worked
  constexpr std::int64_t kStale = 72 * 60 * 60;
  Check(!CheckIsStale(kServerUnix, kServerUnix - kStale, true), "72 hours exactly is not stale yet");
  Check(CheckIsStale(kServerUnix, kServerUnix - kStale - 1, true), "a second past 72 hours is stale");
  Check(!CheckIsStale(kServerUnix, kServerUnix - 3600, true), "an hour is not stale");
  Check(!CheckIsStale(kServerUnix, kServerUnix - kStale - 1, false),
        "with automatic checks off nothing is said");
  Check(!CheckIsStale(kServerUnix, 0, true), "without a baseline nothing is claimed");
  Check(!CheckIsStale(kServerUnix, kServerUnix + 3600, true),
        "a success after this clock's now (a clock set back) is not stale");
}

}  // namespace

int main(int argc, char** argv) {
  FeedTable();
  AssetNames();
  RealReleaseList();
  NotOffered();
  ImmutableRequired();
  Prereleases();
  FutureCodes();
  NewestCountsOwnProduct();
  MsiVersions(argc > 1 ? argv[1] : nullptr);
  DownloadUrls();
  Redirects();
  InstallLocations();
  Outcomes();
  HelperDecisions();
  Reports();
  HelperProcess();
  Schedule();

  std::cout << (gFailures == 0 ? "PASS" : "FAIL") << " update-release-tests: " << gCases
            << " checks, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
