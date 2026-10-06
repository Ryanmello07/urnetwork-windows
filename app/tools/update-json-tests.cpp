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
    "published_at": "2026-10-01T14:51:17Z",
    "assets": [
      {"name": "URnetwork-2026.10.1-1060587890-x64.msi",
       "browser_download_url": "https://github.com/urnetwork/windows/releases/download/v2026.10.1-1060587890/URnetwork-2026.10.1-1060587890-x64.msi",
       "digest": "sha256:140dd13248bed0965ffb36df57d88e34b24676690a7e5c6a6fb379c64cc783dc",
       "size": 117326032},
      {"name": "URnetwork-2026.10.1-1060587890-arm64.msi",
       "browser_download_url": "https://github.com/urnetwork/windows/releases/download/v2026.10.1-1060587890/URnetwork-2026.10.1-1060587890-arm64.msi",
       "digest": "sha256:e1c8d91032023d2b44fe8bc117beeeb4edfb1cca6cf005bfccdf9a1a1742918c"}
    ]
  },
  {
    "tag_name": "v2026.10.2-1061000000",
    "draft": true,
    "prerelease": false,
    "immutable": false,
    "assets": []
  },
  {
    "tag_name": null,
    "draft": "no",
    "prerelease": 1,
    "immutable": "true",
    "assets": [{"name": 7, "browser_download_url": null, "digest": ["sha256:00"]}, "not an asset"]
  },
  "not a release",
  {"tag_name": "v2026.9.1-1034208000", "assets": {"name": "not a list"}}
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
  Check((*releases)[1].draft && !(*releases)[1].immutable, "a draft reads as a draft");
  const Release& malformed = (*releases)[2];
  Check(malformed.tag.empty() && !malformed.draft && !malformed.prerelease && !malformed.immutable,
        "fields of the wrong type read as empty and false, never as true");
  Check(malformed.assets.size() == 1 && malformed.assets[0].name.empty() &&
            malformed.assets[0].url.empty() && malformed.assets[0].digest.empty(),
        "an asset's wrong-type fields read as empty");
  Check((*releases)[3].assets.empty(), "assets that are not a list read as none");

  const Selection s = SelectRelease(*releases, "x64", kOfficialFeed, 1791244800);
  Check(s.code == 1060587890 && s.tag == "v2026.10.1-1060587890", "the published release is offered");
  Check(IsFeedAssetUrl(kOfficialFeed, s.tag, s.assetName, s.assetUrl),
        "its URL is the official feed's own");

  Check(!ParseReleaseList("{\"message\": \"API rate limit exceeded\"}"),
        "an error object is no release list");
  Check(!ParseReleaseList("[{\"tag_name\": "), "a truncated body is no release list");
  Check(!ParseReleaseList(""), "an empty body is no release list");
  const auto none = ParseReleaseList("[]");
  Check(none && none->empty(), "an empty list is a list of nothing");
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
  Results();
  std::cout << (gFailures == 0 ? "PASS" : "FAIL") << " update-json-tests: " << gCases
            << " checks, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
