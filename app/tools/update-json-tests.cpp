// Executable spec for the two JSON documents the update reads: GitHub's
// release list (Common/ReleaseJson.h), which the tray app and the update
// helper must read identically, and the helper's last-result.json
// (Common/UpdateResultJson.h). Needs nlohmann/json, which tools/fetch-deps.ps1
// fetches:
//
//   c++ -std=c++20 -I ../src/Common -isystem <dir with nlohmann/json.hpp> update-json-tests.cpp
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>
#include <string_view>

#include "ReleaseJson.h"
#include "ReleaseSelection.h"
#include "UpdateResultJson.h"

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

// The shape of /repositories/<id>/releases, trimmed: a published immutable
// release with both MSIs, a draft, and a release whose fields are not what
// they should be.
constexpr std::string_view kReleaseList = R"([
  {
    "tag_name": "v2026.10.1-1060587890",
    "draft": false,
    "prerelease": false,
    "immutable": true,
    "created_at": "2026-10-01T13:00:25Z",
    "published_at": "2026-10-01T14:51:17Z",
    "assets": [
      {"name": "URnetwork-2026.10.1-1060587890-x64.msi",
       "browser_download_url": "https://github.com/urnetwork/build/releases/download/v2026.10.1-1060587890/URnetwork-2026.10.1-1060587890-x64.msi",
       "digest": "sha256:140dd13248bed0965ffb36df57d88e34b24676690a7e5c6a6fb379c64cc783dc",
       "size": 55239225},
      {"name": "URnetwork-2026.10.1-1060587890-arm64.msi",
       "browser_download_url": "https://github.com/urnetwork/build/releases/download/v2026.10.1-1060587890/URnetwork-2026.10.1-1060587890-arm64.msi",
       "digest": "sha256:e1c8d91032023d2b44fe8bc117beeeb4edfb1cca6cf005bfccdf9a1a1742918c"}
    ]
  },
  {
    "tag_name": "v2026.10.2-1061000000",
    "draft": true,
    "prerelease": false,
    "immutable": false,
    "created_at": "2026-10-02T00:00:00Z",
    "published_at": null,
    "assets": []
  },
  {
    "tag_name": null,
    "draft": "no",
    "prerelease": 1,
    "immutable": "true",
    "published_at": 1790866277,
    "assets": [{"name": 7, "browser_download_url": null, "digest": ["sha256:00"]}, "not an asset"]
  },
  "not a release",
  {"tag_name": "v2026.9.1-1034208000", "assets": {"name": "not a list"}}
])";

// urnetwork/build's two newest builds as its API listed them, without a token,
// on 2026-10-09 (the list's Date header: 01:24:01Z), trimmed to the fields
// read here and to the MSIs: each build's release, and the two android
// prereleases published with it.
constexpr std::string_view kFeedPage = R"([
  {
    "tag_name": "v2026.10.8-1066946420",
    "draft": false,
    "prerelease": false,
    "immutable": true,
    "created_at": "2026-10-08T21:38:41Z",
    "published_at": "2026-10-08T23:34:12Z",
    "assets": [
      {"name": "URnetwork-2026.10.8-1066946420-arm64.msi",
       "browser_download_url": "https://github.com/urnetwork/build/releases/download/v2026.10.8-1066946420/URnetwork-2026.10.8-1066946420-arm64.msi",
       "digest": "sha256:54b5e831b5cbdcd9aa673d4b8452e9333397cc71da6c0606ecf1d96d890405d7",
       "size": 53301476},
      {"name": "URnetwork-2026.10.8-1066946420-x64.msi",
       "browser_download_url": "https://github.com/urnetwork/build/releases/download/v2026.10.8-1066946420/URnetwork-2026.10.8-1066946420-x64.msi",
       "digest": "sha256:fd8b99179dab8c939e9c8bfc419cf72a57f4efaeb8eda9ed5b5743decd6fdb7b",
       "size": 57807498}
    ]
  },
  {
    "tag_name": "v2026.10.8-1066946423",
    "draft": false,
    "prerelease": true,
    "immutable": true,
    "created_at": "2026-10-08T21:38:48Z",
    "published_at": "2026-10-08T23:34:28Z",
    "assets": [{"name": "com.bringyour.network-2026.10.8-1066946423-github-arm64-v8a-release.apk",
                "browser_download_url": "https://github.com/urnetwork/build/releases/download/v2026.10.8-1066946423/com.bringyour.network-2026.10.8-1066946423-github-arm64-v8a-release.apk",
                "digest": "sha256:ec7afbc164477939fef61a7ff318a0596ce56b08b742ad5c189fcacc7cf08f70",
                "size": 87537385}]
  },
  {
    "tag_name": "v2026.10.8-1066946422",
    "draft": false,
    "prerelease": true,
    "immutable": true,
    "created_at": "2026-10-08T21:38:44Z",
    "published_at": "2026-10-08T23:34:21Z",
    "assets": [{"name": "com.bringyour.network-2026.10.8-1066946422-github-armeabi-v7a-release.apk",
                "browser_download_url": "https://github.com/urnetwork/build/releases/download/v2026.10.8-1066946422/com.bringyour.network-2026.10.8-1066946422-github-armeabi-v7a-release.apk",
                "digest": "sha256:bc4d57fd984f9ad5ed620d52522a84a2a7bd458301dc845795946bee0c27db80",
                "size": 83800401}]
  },
  {
    "tag_name": "v2026.10.6-1065506180",
    "draft": false,
    "prerelease": false,
    "immutable": true,
    "created_at": "2026-10-07T05:35:58Z",
    "published_at": "2026-10-07T07:29:19Z",
    "assets": [
      {"name": "URnetwork-2026.10.6-1065506180-arm64.msi",
       "browser_download_url": "https://github.com/urnetwork/build/releases/download/v2026.10.6-1065506180/URnetwork-2026.10.6-1065506180-arm64.msi",
       "digest": "sha256:ff6ca8dd21098cee0c27b035d9247e5cdca226cfbda46129af77d3695a623997",
       "size": 52881466},
      {"name": "URnetwork-2026.10.6-1065506180-x64.msi",
       "browser_download_url": "https://github.com/urnetwork/build/releases/download/v2026.10.6-1065506180/URnetwork-2026.10.6-1065506180-x64.msi",
       "digest": "sha256:32a36e2e12454d49f792f77675cbe9630fd4ea4d07f6468d477eee7a5280af33",
       "size": 57379296}
    ]
  }
])";

void ReleaseList() {
  const auto releases = ParseReleaseList(kReleaseList);
  Check(releases.has_value(), "the release list parses");
  if (!releases) return;
  Check(releases->size() == 4, "every object is a release, and nothing else is");
  if (releases->size() != 4) return;
  const Release& published = (*releases)[0];
  Check(published.tag == "v2026.10.1-1060587890" && !published.draft && !published.prerelease &&
            published.immutable,
        "a published immutable release reads as one");
  Check(published.assets.size() == 2 &&
            published.assets[0].name == "URnetwork-2026.10.1-1060587890-x64.msi" &&
            published.assets[0].digest ==
                "sha256:140dd13248bed0965ffb36df57d88e34b24676690a7e5c6a6fb379c64cc783dc",
        "its assets keep their names, URLs and digests");
  Check(published.publishedAt == "2026-10-01T14:51:17Z",
        "its publication time is published_at, verbatim, not created_at: " + published.publishedAt);
  Check((*releases)[1].draft && !(*releases)[1].immutable, "a draft reads as a draft");
  Check((*releases)[1].publishedAt.empty(), "a null published_at reads as none");
  const Release& malformed = (*releases)[2];
  Check(malformed.tag.empty() && !malformed.draft && !malformed.prerelease && !malformed.immutable,
        "fields of the wrong type read as empty and false, never as true");
  Check(malformed.publishedAt.empty(), "a published_at that is not a string reads as none");
  Check(malformed.assets.size() == 1 && malformed.assets[0].name.empty() &&
            malformed.assets[0].url.empty() && malformed.assets[0].digest.empty(),
        "an asset's wrong-type fields read as empty");
  Check((*releases)[3].assets.empty(), "assets that are not a list read as none");
  Check((*releases)[3].publishedAt.empty(), "a missing published_at reads as none");

  const Selection s = SelectRelease(*releases, "x64", kOfficialFeed, 1791244800);
  Check(s.code == 1060587890 && s.tag == "v2026.10.1-1060587890", "the published release is offered");
  Check(IsFeedAssetUrl(kOfficialFeed, s.tag, s.assetName, s.assetUrl),
        "its URL is the official feed's own");
  // 2026-10-02T00:00:00Z: it had been out 9 hours when that day began
  const Selection soaking = SelectRelease(*releases, "x64", kOfficialFeed, 1790899200);
  Check(soaking.code == 0 && soaking.waitingCode == 1060587890,
        "and the day after its publication it is still held back");

  Check(!ParseReleaseList("{\"message\": \"API rate limit exceeded\"}"),
        "an error object is no release list");
  Check(!ParseReleaseList("[{\"tag_name\": "), "a truncated body is no release list");
  Check(!ParseReleaseList(""), "an empty body is no release list");
  const auto none = ParseReleaseList("[]");
  Check(none && none->empty(), "an empty list is a list of nothing");
}

// The official feed's own page, read and judged as the tray app and the
// update helper do, against the Date header it came with.
void TheFeedAsListed() {
  const auto releases = ParseReleaseList(kFeedPage);
  Check(releases.has_value() && releases->size() == 4, "the feed's page parses");
  if (!releases || releases->size() != 4) return;
  Check((*releases)[0].publishedAt == "2026-10-08T23:34:12Z" && (*releases)[0].immutable &&
            !(*releases)[0].prerelease && (*releases)[0].assets.size() == 2,
        "its newest release reads as published, immutable and carrying both MSIs");
  Check((*releases)[1].prerelease && (*releases)[2].prerelease,
        "the android releases published with it read as prereleases");

  // 2026-10-09T01:24:01Z, the list's Date header
  const Selection s = SelectRelease(*releases, "x64", kOfficialFeed, 1791509041);
  Check(s.tag == "v2026.10.6-1065506180" && s.code == 1065506180,
        "on 2026-10-09 the feed offers the release published on 2026-10-07: " + s.tag);
  Check(s.digestHex == "32a36e2e12454d49f792f77675cbe9630fd4ea4d07f6468d477eee7a5280af33",
        "with GitHub's SHA-256 of its x64 MSI");
  Check(IsFeedAssetUrl(kOfficialFeed, s.tag, s.assetName, s.assetUrl),
        "at the URL the official feed names for it: " + s.assetUrl);
  Check(s.waitingCode == 1066946420 && s.waitingFromUnixSeconds == 1791590400,
        "and holds the release published two hours before that list back until 2026-10-10");
  const Selection arm = SelectRelease(*releases, "arm64", kOfficialFeed, 1791509041);
  Check(arm.assetName == "URnetwork-2026.10.6-1065506180-arm64.msi" &&
            arm.digestHex == "ff6ca8dd21098cee0c27b035d9247e5cdca226cfbda46129af77d3695a623997",
        "arm64 is offered its own MSI and digest");
  // 2026-10-10T00:00:00Z
  const Selection next = SelectRelease(*releases, "x64", kOfficialFeed, 1791590400);
  Check(next.tag == "v2026.10.8-1066946420" &&
            next.digestHex == "fd8b99179dab8c939e9c8bfc419cf72a57f4efaeb8eda9ed5b5743decd6fdb7b",
        "the next day begins with the newer release offered: " + next.tag);
}

void Results() {
  const UpdateResult installed{.tag = "v2026.10.1-1060587890",
                               .code = 1060587890,
                               .exitCode = 0,
                               .finishedUtc = "2026-10-01T14:51:17Z"};
  const std::string text = FormatUpdateResult(installed);
  Check(text ==
            "{\"code\":1060587890,\"exitCode\":0,\"finishedUtc\":\"2026-10-01T14:51:17Z\","
            "\"tag\":\"v2026.10.1-1060587890\"}\n",
        "the report is written as one line: " + text);
  const auto read = ParseUpdateResult(text);
  Check(read && read->tag == installed.tag && read->code == installed.code &&
            read->exitCode == 0 && read->finishedUtc == installed.finishedUtc,
        "the report reads back as written");

  UpdateResult refused = installed;
  refused.exitCode = static_cast<std::int64_t>(Refusal::Digest);
  const auto refusedRead = ParseUpdateResult(FormatUpdateResult(refused));
  Check(refusedRead && refusedRead->exitCode == 0x20000009 &&
            OutcomeOf(refusedRead->exitCode) == Outcome::Refused,
        "a refusal reads back as a refusal");
  UpdateResult restart = installed;
  restart.exitCode = 3010;
  const auto restartRead = ParseUpdateResult(FormatUpdateResult(restart));
  Check(restartRead && OutcomeOf(restartRead->exitCode) == Outcome::RestartRequired,
        "3010 reads back as restart to finish");

  for (const char* bad : {
           "",
           "[]",
           "{}",
           "not json",
           R"({"tag":"v2026.10.1-1060587890","code":"1060587890","exitCode":0,"finishedUtc":"2026-10-01T14:51:17Z"})",
           R"({"tag":"v2026.10.1-1060587890","code":1060587890,"exitCode":"0","finishedUtc":"2026-10-01T14:51:17Z"})",
           R"({"tag":"v2026.10.1-1060587890","code":1060587890,"exitCode":0})",
           R"({"tag":"v2026.10.1-1060587890","code":-1060587890,"exitCode":0,"finishedUtc":"2026-10-01T14:51:17Z"})",
           R"({"tag":"v2026.10.1-1060587890","code":1060587891,"exitCode":0,"finishedUtc":"2026-10-01T14:51:17Z"})",
           R"({"tag":"v2026.10.1-1060587890","code":1060587890,"exitCode":-1,"finishedUtc":"2026-10-01T14:51:17Z"})",
           R"({"tag":"v2026.10.1-1060587890","code":1060587890,"exitCode":0,"finishedUtc":"yesterday"})",
           R"({"tag":"latest","code":1060587890,"exitCode":0,"finishedUtc":"2026-10-01T14:51:17Z"})",
           R"({"tag":"v2026.10.1-1060587890","code":1060587890,"exitCode":0.5,"finishedUtc":"2026-10-01T14:51:17Z"})",
       }) {
    // the reader must not throw either: the tray app reads this file on its
    // worker thread, where an escaped exception ends the app
    bool refused = false;
    try {
      refused = !ParseUpdateResult(bad).has_value();
    } catch (...) {
      refused = false;
    }
    Check(refused, std::string("not a report: ") + bad);
  }

  UpdateResult prefixed = installed;
  prefixed.tag = "runner-test-v2026.10.1-1060587890";
  Check(ParseUpdateResult(FormatUpdateResult(prefixed), "runner-test-").has_value() &&
            !ParseUpdateResult(FormatUpdateResult(prefixed)),
        "a runner test feed's report reads with its prefix only");
}

}  // namespace

int main() {
  ReleaseList();
  TheFeedAsListed();
  Results();
  std::cout << (gFailures == 0 ? "PASS" : "FAIL") << " update-json-tests: " << gCases
            << " checks, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
