// Executable spec for the marker that refuses launches while the in-app
// updater's installer runs (Common/UpdateMarker.h, owner decision 2026-10-05:
// launches during an update are refused). A launch during an update is
// refused; a marker left by an update that is over, failed, crashed or never
// returned does not refuse launches for long; and the marker survives the
// trip through its file. Run against the same header the app compiles, on any
// host with a C++20 compiler.
//
//   c++ -std=c++20 -I ../src/Common update-marker-tests.cpp -o /tmp/update-marker-tests && /tmp/update-marker-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "UpdateMarker.h"

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

// A marker written at a fixed time, synthetic.
constexpr std::int64_t kWrittenAt = 1900000000;
const UpdateMarker kMarker{.installerProcessId = 4321,
                           .installerCreationTime = 133000000000000000ULL,
                           .writtenAt = kWrittenAt};

// The pure check, under a name the harness's own Check does not hide.
template <class Probe>
urnw::update::Verdict VerdictFor(const std::optional<std::string>& text, Probe&& probe,
                                 std::int64_t now) {
  return urnw::update::Check(text, std::forward<Probe>(probe), now);
}

// A probe that answers `state` and counts how often it was asked.
struct FakeProbe {
  InstallerState state = InstallerState::Running;
  int asked = 0;
  UpdateMarker seen{};
  InstallerState operator()(const UpdateMarker& marker) {
    ++asked;
    seen = marker;
    return state;
  }
};

void TestALaunchDuringAnUpdateIsRefused() {
  FakeProbe running{.state = InstallerState::Running};
  Check(VerdictFor(FormatUpdateMarker(kMarker), running, kWrittenAt + 30) == Verdict::Updating,
        "updating: while the updater's installer runs, a launch is refused");
  Check(running.asked == 1 && running.seen.installerProcessId == kMarker.installerProcessId &&
            running.seen.installerCreationTime == kMarker.installerCreationTime,
        "updating: the installer is looked up by its process id and creation time");
  FakeProbe unknown{.state = InstallerState::Unknown};
  Check(VerdictFor(FormatUpdateMarker(kMarker), unknown, kWrittenAt + 30) == Verdict::Updating,
        "updating: an installer that cannot be looked at still refuses, within the lifetime");
}

void TestNoMarkerRefusesNothing() {
  FakeProbe probe;
  Check(VerdictFor(std::nullopt, probe, kWrittenAt) == Verdict::None,
        "no update: without a marker, a launch starts as usual");
  Check(probe.asked == 0, "no update: nothing is looked up");
}

void TestAStaleMarkerFromADeadUpdateDoesNotBlock() {
  FakeProbe ended{.state = InstallerState::Ended};
  Check(VerdictFor(FormatUpdateMarker(kMarker), ended, kWrittenAt + 30) == Verdict::Stale,
        "stale: once the installer has ended (a finished, failed or crashed update), the marker "
        "is stale and the launch starts");
  const std::int64_t lifetime = kUpdateMarkerLifetime.count();
  FakeProbe hung{.state = InstallerState::Running};
  Check(VerdictFor(FormatUpdateMarker(kMarker), hung, kWrittenAt + lifetime) == Verdict::Updating,
        "stale: an installer still running at the end of the lifetime still refuses");
  Check(VerdictFor(FormatUpdateMarker(kMarker), hung, kWrittenAt + lifetime + 1) == Verdict::Stale,
        "stale: past its lifetime a marker no longer refuses, even if its installer never returns");
  FakeProbe unknown{.state = InstallerState::Unknown};
  Check(VerdictFor(FormatUpdateMarker(kMarker), unknown, kWrittenAt + lifetime + 1) ==
            Verdict::Stale,
        "stale: an installer that cannot be looked at refuses no longer than the lifetime");
  const std::int64_t skew = kUpdateMarkerClockSkew.count();
  Check(VerdictFor(FormatUpdateMarker(kMarker), hung, kWrittenAt - skew) == Verdict::Updating,
        "stale: a marker a little in the future (a clock moved back) is still trusted");
  Check(VerdictFor(FormatUpdateMarker(kMarker), hung, kWrittenAt - skew - 1) == Verdict::Stale,
        "stale: a marker far in the future cannot refuse launches for good");
  FakeProbe never;
  Check(VerdictFor(std::string("not a marker\n"), never, kWrittenAt) == Verdict::Stale &&
            never.asked == 0,
        "stale: a file that does not parse is stale, without a look at any process");
}

void TestTheMarkerSurvivesItsFile() {
  const std::string text = FormatUpdateMarker(kMarker);
  Check(text == "4321 133000000000000000 1900000000\n", "file: the text is the three numbers");
  const std::optional<UpdateMarker> parsed = ParseUpdateMarker(text);
  Check(parsed && parsed->installerProcessId == kMarker.installerProcessId &&
            parsed->installerCreationTime == kMarker.installerCreationTime &&
            parsed->writtenAt == kMarker.writtenAt,
        "file: a marker reads back as written");
  Check(ParseUpdateMarker("4321 133000000000000000 1900000000").has_value(),
        "file: the final newline is optional");
  const std::string_view broken[] = {
      "",
      "\n",
      "4321",
      "4321 133000000000000000",
      "4321 133000000000000000 1900000000 7",
      "4321  133000000000000000 1900000000",
      " 4321 133000000000000000 1900000000",
      "4321 133000000000000000 1900000000\n\n",
      "-1 133000000000000000 1900000000",
      "0 133000000000000000 1900000000",
      "4321 0 1900000000",
      "4321 x 1900000000",
      "99999999999 133000000000000000 1900000000",
  };
  for (std::string_view text : broken) {
    Check(!ParseUpdateMarker(text).has_value(),
          "file: '" + std::string(text) + "' is not a marker");
  }
}

void TestNames() {
  for (Verdict verdict : {Verdict::None, Verdict::Updating, Verdict::Stale}) {
    Check(std::string_view(ToString(verdict)) != "unknown", "names: every verdict");
  }
}

}  // namespace

int main() {
  TestALaunchDuringAnUpdateIsRefused();
  TestNoMarkerRefusesNothing();
  TestAStaleMarkerFromADeadUpdateDoesNotBlock();
  TestTheMarkerSurvivesItsFile();
  TestNames();
  std::cout << (gCases - gFailures) << "/" << gCases << " update marker checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
