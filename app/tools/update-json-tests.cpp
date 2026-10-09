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
// release with both MSIs, whose notes were edited nine hours after it was
// published, a draft, and a release whose fields are not what they should be.
constexpr std::string_view kReleaseList = R"([
  {
    "tag_name": "v2026.10.1-1060587890",
    "draft": false,
    "prerelease": false,
    "immutable": true,
    "created_at": "2026-10-01T13:00:25Z",
    "published_at": "2026-10-01T14:51:17Z",
    "updated_at": "2026-10-02T00:10:00Z",
    "assets": [
      {"name": "URnetwork-2026.10.1-1060587890-x64.msi",
       "browser_download_url": "https://github.com/urnetwork/build/releases/download/v2026.10.1-1060587890/URnetwork-2026.10.1-1060587890-x64.msi",
       "digest": "sha256:140dd13248bed0965ffb36df57d88e34b24676690a7e5c6a6fb379c64cc783dc",
       "size": 55239225,
       "created_at": "2026-10-01T14:07:21Z",
       "updated_at": "2026-10-01T14:07:24Z"},
      {"name": "URnetwork-2026.10.1-1060587890-arm64.msi",
       "browser_download_url": "https://github.com/urnetwork/build/releases/download/v2026.10.1-1060587890/URnetwork-2026.10.1-1060587890-arm64.msi",
       "digest": "sha256:e1c8d91032023d2b44fe8bc117beeeb4edfb1cca6cf005bfccdf9a1a1742918c",
       "created_at": "2026-10-01T14:07:18Z",
       "updated_at": "2026-10-01T14:07:20Z"}
    ]
  },
  {
    "tag_name": "v2026.10.2-1061000000",
    "draft": true,
    "prerelease": false,
    "immutable": false,
    "created_at": "2026-10-02T00:00:00Z",
    "published_at": null,
    "updated_at": "2026-10-02T00:00:00Z",
    "assets": []
  },
  {
    "tag_name": null,
    "draft": "no",
    "prerelease": 1,
    "immutable": "true",
    "published_at": 1790866277,
    "updated_at": 1790866277,
    "assets": [{"name": 7, "browser_download_url": null, "digest": ["sha256:00"], "updated_at": null},
               "not an asset"]
  },
  "not a release",
  {"tag_name": "v2026.9.1-1034208000", "assets": {"name": "not a list"}}
])";

// One release that is complete in every other way (immutable, published and
// uploaded long before the list, both times given), with `marks` for its draft
// and prerelease keys: what the reader makes of marks that are not the two
// booleans the API sends.
std::string ReleaseWithMarks(std::string_view marks) {
  std::string list = R"([{"tag_name": "v2026.10.1-1060587890", )";
  list.append(marks);
  list.append(R"("immutable": true,
    "published_at": "2026-10-01T14:51:17Z",
    "updated_at": "2026-10-01T14:51:17Z",
    "assets": [{"name": "URnetwork-2026.10.1-1060587890-x64.msi",
                "browser_download_url": "https://github.com/urnetwork/build/releases/download/v2026.10.1-1060587890/URnetwork-2026.10.1-1060587890-x64.msi",
                "digest": "sha256:140dd13248bed0965ffb36df57d88e34b24676690a7e5c6a6fb379c64cc783dc",
                "updated_at": "2026-10-01T14:07:24Z"}]}])");
  return list;
}

// urnetwork/build's two newest builds as its API listed them, without a token,
// on 2026-10-09 (the list's Date header: 01:24:01Z), trimmed to the fields
// read here and to the MSIs: each build's release, and the two android
// prereleases published with it. Each release's updated_at is its
// published_at, and each asset was uploaded before the publication: nobody
// touched them since.
constexpr std::string_view kFeedPage = R"([
  {
    "tag_name": "v2026.10.8-1066946420",
    "draft": false,
    "prerelease": false,
    "immutable": true,
    "created_at": "2026-10-08T21:38:41Z",
    "published_at": "2026-10-08T23:34:12Z",
    "updated_at": "2026-10-08T23:34:12Z",
    "assets": [
      {"name": "URnetwork-2026.10.8-1066946420-arm64.msi",
       "browser_download_url": "https://github.com/urnetwork/build/releases/download/v2026.10.8-1066946420/URnetwork-2026.10.8-1066946420-arm64.msi",
       "digest": "sha256:54b5e831b5cbdcd9aa673d4b8452e9333397cc71da6c0606ecf1d96d890405d7",
       "size": 53301476,
       "created_at": "2026-10-08T22:47:45Z",
       "updated_at": "2026-10-08T22:47:47Z"},
      {"name": "URnetwork-2026.10.8-1066946420-x64.msi",
       "browser_download_url": "https://github.com/urnetwork/build/releases/download/v2026.10.8-1066946420/URnetwork-2026.10.8-1066946420-x64.msi",
       "digest": "sha256:fd8b99179dab8c939e9c8bfc419cf72a57f4efaeb8eda9ed5b5743decd6fdb7b",
       "size": 57807498,
       "created_at": "2026-10-08T22:47:48Z",
       "updated_at": "2026-10-08T22:47:51Z"}
    ]
  },
  {
    "tag_name": "v2026.10.8-1066946423",
    "draft": false,
    "prerelease": true,
    "immutable": true,
    "created_at": "2026-10-08T21:38:48Z",
    "published_at": "2026-10-08T23:34:28Z",
    "updated_at": "2026-10-08T23:34:28Z",
    "assets": [{"name": "com.bringyour.network-2026.10.8-1066946423-github-arm64-v8a-release.apk",
                "browser_download_url": "https://github.com/urnetwork/build/releases/download/v2026.10.8-1066946423/com.bringyour.network-2026.10.8-1066946423-github-arm64-v8a-release.apk",
                "digest": "sha256:ec7afbc164477939fef61a7ff318a0596ce56b08b742ad5c189fcacc7cf08f70",
                "size": 87537385,
                "created_at": "2026-10-08T23:34:22Z",
                "updated_at": "2026-10-08T23:34:27Z"}]
  },
  {
    "tag_name": "v2026.10.8-1066946422",
    "draft": false,
    "prerelease": true,
    "immutable": true,
    "created_at": "2026-10-08T21:38:44Z",
    "published_at": "2026-10-08T23:34:21Z",
    "updated_at": "2026-10-08T23:34:21Z",
    "assets": [{"name": "com.bringyour.network-2026.10.8-1066946422-github-armeabi-v7a-release.apk",
                "browser_download_url": "https://github.com/urnetwork/build/releases/download/v2026.10.8-1066946422/com.bringyour.network-2026.10.8-1066946422-github-armeabi-v7a-release.apk",
                "digest": "sha256:bc4d57fd984f9ad5ed620d52522a84a2a7bd458301dc845795946bee0c27db80",
                "size": 83800401,
                "created_at": "2026-10-08T23:34:14Z",
                "updated_at": "2026-10-08T23:34:20Z"}]
  },
  {
    "tag_name": "v2026.10.6-1065506180",
    "draft": false,
    "prerelease": false,
    "immutable": true,
    "created_at": "2026-10-07T05:35:58Z",
    "published_at": "2026-10-07T07:29:19Z",
    "updated_at": "2026-10-07T07:29:19Z",
    "assets": [
      {"name": "URnetwork-2026.10.6-1065506180-arm64.msi",
       "browser_download_url": "https://github.com/urnetwork/build/releases/download/v2026.10.6-1065506180/URnetwork-2026.10.6-1065506180-arm64.msi",
       "digest": "sha256:ff6ca8dd21098cee0c27b035d9247e5cdca226cfbda46129af77d3695a623997",
       "size": 52881466,
       "created_at": "2026-10-07T06:43:26Z",
       "updated_at": "2026-10-07T06:43:28Z"},
      {"name": "URnetwork-2026.10.6-1065506180-x64.msi",
       "browser_download_url": "https://github.com/urnetwork/build/releases/download/v2026.10.6-1065506180/URnetwork-2026.10.6-1065506180-x64.msi",
       "digest": "sha256:32a36e2e12454d49f792f77675cbe9630fd4ea4d07f6468d477eee7a5280af33",
       "size": 57379296,
       "created_at": "2026-10-07T06:43:29Z",
       "updated_at": "2026-10-07T06:43:33Z"}
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
  Check(published.updatedAt == "2026-10-02T00:10:00Z",
        "its last change is updated_at, verbatim, not published_at: " + published.updatedAt);
  Check(published.assets.size() == 2 && published.assets[0].updatedAt == "2026-10-01T14:07:24Z" &&
            published.assets[1].updatedAt == "2026-10-01T14:07:20Z",
        "its assets keep their upload times, each its own updated_at, not created_at");
  Check((*releases)[1].draft && !(*releases)[1].immutable, "a draft reads as a draft");
  Check((*releases)[1].publishedAt.empty(), "a null published_at reads as none");
  const Release& malformed = (*releases)[2];
  Check(malformed.tag.empty() && !malformed.immutable,
        "a tag and an immutable mark of the wrong type read as empty and as not immutable");
  Check(malformed.draft && malformed.prerelease,
        "a draft or prerelease mark of the wrong type reads as a draft and a prerelease");
  Check(malformed.publishedAt.empty(), "a published_at that is not a string reads as none");
  Check(malformed.updatedAt.empty(), "an updated_at that is not a string reads as none");
  Check(malformed.assets.size() == 1 && malformed.assets[0].name.empty() &&
            malformed.assets[0].url.empty() && malformed.assets[0].digest.empty() &&
            malformed.assets[0].updatedAt.empty(),
        "an asset's wrong-type fields read as empty");
  Check((*releases)[3].assets.empty(), "assets that are not a list read as none");
  Check((*releases)[3].publishedAt.empty() && (*releases)[3].updatedAt.empty(),
        "a missing published_at and a missing updated_at read as none");
  Check((*releases)[3].draft && (*releases)[3].prerelease,
        "a release without the two marks reads as a draft and a prerelease");

  const Selection s = SelectRelease(*releases, "x64", kOfficialFeed, 1791244800);
  Check(s.code == 1060587890 && s.tag == "v2026.10.1-1060587890", "the published release is offered");
  Check(IsFeedAssetUrl(kOfficialFeed, s.tag, s.assetName, s.assetUrl),
        "its URL is the official feed's own");
  // 2026-10-02T00:00:00Z: it had been out 9 hours when that day began
  const Selection soaking = SelectRelease(*releases, "x64", kOfficialFeed, 1790899200);
  Check(soaking.code == 0 && soaking.waitingCode == 1060587890,
        "and the day after its publication it is still held back");
  // 2026-10-03T12:00:00Z: published 45 hours before, and edited 36 hours
  // before, ten minutes into 2026-10-02. By its publication it would count
  // from 2026-10-03; by its last change it counts from 2026-10-04.
  const Selection edited = SelectRelease(*releases, "x64", kOfficialFeed, 1791028800);
  Check(edited.code == 0 && edited.waitingCode == 1060587890 &&
            edited.waitingFromUnixSeconds == 1791072000,
        "its day is counted from its last change, updated_at: it counts from 2026-10-04");

  // A release counts only when the list says, with the boolean false, that it
  // is no draft and no prerelease.
  const auto offers = [](std::string_view marks) {
    const auto list = ParseReleaseList(ReleaseWithMarks(marks));
    return list && list->size() == 1 &&
           SelectRelease(*list, "x64", kOfficialFeed, 1791244800).code == 1060587890;
  };
  Check(offers(R"("draft": false, "prerelease": false, )"),
        "a release marked as no draft and no prerelease is offered");
  Check(!offers(R"("draft": false, )"), "a release without a prerelease mark is not offered");
  Check(!offers(R"("prerelease": false, )"), "a release without a draft mark is not offered");
  for (const char* other : {"null", "0", "\"false\"", "[]", "true"}) {
    Check(!offers(std::string(R"("draft": false, "prerelease": )") + other + ", "),
          std::string("a prerelease mark that is not the boolean false is a prerelease: ") + other);
    Check(!offers(std::string(R"("prerelease": false, "draft": )") + other + ", "),
          std::string("a draft mark that is not the boolean false is a draft: ") + other);
  }

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
  Check((*releases)[0].updatedAt == (*releases)[0].publishedAt &&
            (*releases)[0].assets[1].updatedAt == "2026-10-08T22:47:51Z",
        "unchanged since it was published, its x64 MSI uploaded 46 minutes before");
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
