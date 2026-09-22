// Executable spec for the dual-stack pure logic (connect/IPV6.md): the
// IP-family status row's counting, ranking and line selection
// (App/IpFamilyStatus.h), the hero's dot-size rule (App/ProviderDotDiameter.h),
// the provider row's family fields (App/ProviderLocations.h), and the IPv6 half
// of the tunnel's route/firewall table (Service/NetPolicy.h) — run against the
// SAME sources the app and the service compile, on any host with a C++20
// compiler. The Windows-only halves (the IpFamilyStatusRow drawing,
// NetworkConfig's settings validation and the WFP filter set) are covered by
// the app build and urnetworkd's own selftest.
//
//   c++ -std=c++20 -I ../src/App -I ../src/Service ip-family-tests.cpp \
//       ../src/App/IpFamilyStatus.cpp ../src/App/ProviderLocations.cpp \
//       -o /tmp/ip-family-tests && /tmp/ip-family-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <cmath>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "IpFamilyStatus.h"
#include "NetPolicy.h"
#include "ProviderDotDiameter.h"
#include "ProviderLocations.h"

using namespace urnw;

namespace {

int gFailures = 0;
int gCases = 0;
std::string gCurrentCase;

void Fail(const std::string& message) {
  ++gFailures;
  std::cout << "  FAIL [" << gCurrentCase << "] " << message << "\n";
}

void Check(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}

void CheckNear(double expected, double actual, double tolerance, const std::string& what) {
  if (!(std::fabs(expected - actual) <= tolerance)) {
    std::ostringstream out;
    out << what << ": expected " << expected << " +/- " << tolerance << ", got " << actual;
    Fail(out.str());
  }
}

struct Case {
  explicit Case(const char* name) {
    gCurrentCase = name;
    ++gCases;
  }
};
#define TEST_CASE(name) Case case_##__LINE__(name)

IpFamilyPoint Point(const char* family, const char* state = "Added", const char* id = "p") {
  return IpFamilyPoint{id, state, family};
}

IpFamilyColumnStatus Status(IpFamilyColumn column, int64_t connected = 0, int64_t connecting = 0) {
  IpFamilyColumnStatus status;
  status.column = column;
  status.connectedCount = connected;
  status.connectingCount = connecting;
  return status;
}

IpFamilyStatusLine Line(IpFamilyLineKind kind, int64_t count = 0) {
  return IpFamilyStatusLine{kind, count};
}

std::string Text6(const net::V6Prefix& p) {
  const auto b = p.Bytes();
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%02x%02x:%02x%02x::/%u", b[0], b[1], b[2], b[3],
                static_cast<unsigned>(p.prefix));
  return buf;
}

// ---- IpFamilyStatus.h: counting -------------------------------------------
//
// The same fourteen cases apple's IpFamilyStatusRowTests pin, so the platforms
// agree on every column, tier and line.

void CountingTests() {
  {
    TEST_CASE("columnsAreAlwaysPresentInDisplayOrder");
    const auto statuses = IpFamilyColumnStatuses({});
    Check(statuses.size() == 3, "three columns");
    Check(statuses[0].column == IpFamilyColumn::Dualstack &&
              statuses[1].column == IpFamilyColumn::V4 && statuses[2].column == IpFamilyColumn::V6,
          "Dualstack, IPv4, IPv6 in that order");
    for (const auto& status : statuses) Check(status.Unavailable(), "nothing counted");
  }
  {
    // The columns are categories, not capabilities: a dualstack provider
    // counts once, under Dualstack, never under IPv4 or IPv6 as well.
    TEST_CASE("countsConnectedAndConnectingByCategory");
    const auto statuses = IpFamilyColumnStatuses({
        Point("dualstack", "Added", "a"),
        Point("dualstack", "Added", "b"),
        Point("dualstack", "InEvaluation", "c"),
        Point("v4-only", "Added", "d"),
        Point("v6-only", "InEvaluation", "e"),
        Point("v6-only", "InEvaluation", "f"),
    });
    Check(statuses[0] == Status(IpFamilyColumn::Dualstack, 2, 1), "dualstack 2 connected, 1 connecting");
    Check(statuses[1] == Status(IpFamilyColumn::V4, 1, 0), "v4 1 connected");
    Check(statuses[2] == Status(IpFamilyColumn::V6, 0, 2), "v6 2 connecting");
  }
  {
    // A provider that failed evaluation, was not added, or is on its way out
    // (it lingers on the grid for the removal tween) counts as nothing.
    TEST_CASE("ignoresProvidersThatAreNotLive");
    const auto statuses = IpFamilyColumnStatuses({
        Point("dualstack", "EvaluationFailed", "a"),
        Point("v4-only", "NotAdded", "b"),
        Point("v6-only", "Removed", "c"),
        Point("v6-only", "something-newer", "d"),
    });
    for (const auto& status : statuses) Check(status.Unavailable(), "nothing counted");
  }
  {
    // A legacy or unknown category carries v4, so it is an IPv4 provider
    // rather than one that vanishes from the row.
    TEST_CASE("legacyAndUnknownCategoriesReadAsV4");
    const auto statuses = IpFamilyColumnStatuses({
        Point("", "Added", "a"),
        Point("something-newer", "InEvaluation", "b"),
    });
    Check(statuses[1] == Status(IpFamilyColumn::V4, 1, 1), "both under v4");
    Check(statuses[0].Unavailable() && statuses[2].Unavailable(), "nothing elsewhere");
    Check(IpFamilyColumnFor("dualstack") == IpFamilyColumn::Dualstack, "dualstack -> Dualstack");
    Check(IpFamilyColumnFor("v4-only") == IpFamilyColumn::V4, "v4-only -> V4");
    Check(IpFamilyColumnFor("v6-only") == IpFamilyColumn::V6, "v6-only -> V6");
  }
  {
    // The SDK's grid list carries one entry per provider, keyed by client id;
    // IpFamilyStatusRow::SetGrid hands the id, state and family through, and
    // a cell without an id is a position, not a provider (apple's
    // "statusesFromSdkGridPoints" pins the same conversion there).
    TEST_CASE("statusesFromGridPoints");
    const auto statuses = IpFamilyColumnStatuses({
        Point("v6-only", "Added", "added"),
        Point("dualstack", "InEvaluation", "evaluating"),
        Point("dualstack", "Added", ""),
    });
    Check(statuses[0] == Status(IpFamilyColumn::Dualstack, 0, 1), "dualstack 1 connecting");
    Check(statuses[2] == Status(IpFamilyColumn::V6, 1, 0), "v6 1 connected");
    Check(statuses[0].connectedCount == 0, "the bare cell is not a provider");
  }
  {
    TEST_CASE("tokensRoundTrip");
    for (IpFamilyColumn column : kIpFamilyColumns) {
      Check(IpFamilyColumnForToken(IpFamilyColumnToken(column)) == column, "token round trip");
    }
    Check(std::string(IpFamilyColumnToken(IpFamilyColumn::Dualstack)) == "both", "both token");
    Check(std::string(IpFamilyColumnToken(IpFamilyColumn::V4)) == "v4", "v4 token");
    Check(std::string(IpFamilyColumnToken(IpFamilyColumn::V6)) == "v6", "v6 token");
    Check(IpFamilyColumnForToken("") == IpFamilyColumn::V4, "empty token -> V4");
    Check(IpFamilyColumnForToken("dualstack") == IpFamilyColumn::V4,
          "a category is not a token; unknown tokens read as v4");
  }
}

// ---- IpFamilyStatus.h: ranking --------------------------------------------

void RankingTests() {
  {
    TEST_CASE("dualstackConnectedIsBestAndTheOthersAreDimmed");
    const auto tiers = IpFamilyColumnTiers({
        Status(IpFamilyColumn::Dualstack, 1),
        Status(IpFamilyColumn::V4, 3),
        Status(IpFamilyColumn::V6, 0, 1),
    });
    Check(tiers == std::vector<IpFamilyTier>{IpFamilyTier::Best, IpFamilyTier::Active,
                                             IpFamilyTier::Active},
          "dualstack best, v4 and v6 active");
  }
  {
    // IPv4 and IPv6 tie, so with nothing dualstack connected they share the top.
    TEST_CASE("v4AndV6ShareBestWhenNothingDualstackIsConnected");
    const auto tiers = IpFamilyColumnTiers({
        Status(IpFamilyColumn::Dualstack),
        Status(IpFamilyColumn::V4, 2),
        Status(IpFamilyColumn::V6, 1),
    });
    Check(tiers == std::vector<IpFamilyTier>{IpFamilyTier::Unavailable, IpFamilyTier::Best,
                                             IpFamilyTier::Best},
          "dualstack unavailable, v4 and v6 both best");
  }
  {
    TEST_CASE("aLoneConnectedColumnIsBest");
    const auto tiers = IpFamilyColumnTiers({
        Status(IpFamilyColumn::Dualstack),
        Status(IpFamilyColumn::V4),
        Status(IpFamilyColumn::V6, 1),
    });
    Check(tiers == std::vector<IpFamilyTier>{IpFamilyTier::Unavailable, IpFamilyTier::Unavailable,
                                             IpFamilyTier::Best},
          "only v6 best");
  }
  {
    // A column that is only connecting carries no traffic yet: it is active,
    // never best, even when it outranks the connected column.
    TEST_CASE("aConnectingOnlyColumnIsActiveNotBest");
    const auto tiers = IpFamilyColumnTiers({
        Status(IpFamilyColumn::Dualstack, 0, 2),
        Status(IpFamilyColumn::V4, 1),
        Status(IpFamilyColumn::V6),
    });
    Check(tiers == std::vector<IpFamilyTier>{IpFamilyTier::Active, IpFamilyTier::Best,
                                             IpFamilyTier::Unavailable},
          "dualstack active, v4 best, v6 unavailable");
  }
  {
    TEST_CASE("onlyConnectingColumnsMakeNothingBest");
    const auto tiers = IpFamilyColumnTiers({
        Status(IpFamilyColumn::Dualstack, 0, 1),
        Status(IpFamilyColumn::V4, 0, 1),
        Status(IpFamilyColumn::V6),
    });
    Check(tiers == std::vector<IpFamilyTier>{IpFamilyTier::Active, IpFamilyTier::Active,
                                             IpFamilyTier::Unavailable},
          "nothing best while nothing is connected");
  }
  {
    TEST_CASE("nothingLiveMakesEveryColumnUnavailable");
    const auto tiers = IpFamilyColumnTiers(IpFamilyColumnStatuses({}));
    Check(tiers.size() == 3, "three tiers");
    for (IpFamilyTier tier : tiers) Check(tier == IpFamilyTier::Unavailable, "unavailable");
    Check(IpFamilyColumnRank(IpFamilyColumn::Dualstack) < IpFamilyColumnRank(IpFamilyColumn::V4) &&
              IpFamilyColumnRank(IpFamilyColumn::V4) == IpFamilyColumnRank(IpFamilyColumn::V6),
          "dualstack outranks the tied v4 and v6");
  }
}

// ---- IpFamilyStatus.h: lines ----------------------------------------------

void LineTests() {
  {
    TEST_CASE("linesShowTheNonZeroCountsConnectedFirst");
    Check(IpFamilyStatusLines(Status(IpFamilyColumn::V4, 3, 1)) ==
              std::vector<IpFamilyStatusLine>{Line(IpFamilyLineKind::Connected, 3),
                                              Line(IpFamilyLineKind::Connecting, 1)},
          "connected then connecting");
    Check(IpFamilyStatusLines(Status(IpFamilyColumn::V4, 3)) ==
              std::vector<IpFamilyStatusLine>{Line(IpFamilyLineKind::Connected, 3)},
          "connected alone");
    Check(IpFamilyStatusLines(Status(IpFamilyColumn::V4, 0, 1)) ==
              std::vector<IpFamilyStatusLine>{Line(IpFamilyLineKind::Connecting, 1)},
          "connecting alone");
  }
  {
    TEST_CASE("aColumnWithNothingReadsDisconnected");
    Check(IpFamilyStatusLines(Status(IpFamilyColumn::V6)) ==
              std::vector<IpFamilyStatusLine>{Line(IpFamilyLineKind::Disconnected)},
          "disconnected alone");
  }
  {
    // The line's identity is its kind: a count change is the same line with a
    // new number (the slot updates in place), a kind change is a line coming
    // or going (the slot appears or collapses).
    TEST_CASE("lineIdentityIsTheKind");
    Check(Line(IpFamilyLineKind::Connected, 1).kind == Line(IpFamilyLineKind::Connected, 2).kind,
          "same kind across counts");
    Check(Line(IpFamilyLineKind::Connected, 1).kind != Line(IpFamilyLineKind::Connecting, 1).kind,
          "different kinds");
    Check(Line(IpFamilyLineKind::Connected, 1) != Line(IpFamilyLineKind::Connected, 2),
          "a count change is a value change");
  }
}

// ---- the dot size rule (ConnectCanvas::Layout parity) ----------------------

void DotDiameterTests() {
  {
    TEST_CASE("diameterIsSideOverColumns");
    CheckNear(256.0 / 14, ProviderDotDiameter(256, 14, 14), 1e-9, "square 14 grid on 256");
    CheckNear(288.0 / 16, ProviderDotDiameter(288, 16, 16), 1e-9, "square 16 grid on 288");
    CheckNear(168.0 / 10, ProviderDotDiameter(168, 10, 10), 1e-9, "square 10 grid on 168");
  }
  {
    TEST_CASE("nonSquareGridUsesTheLargerDimension");
    CheckNear(256.0 / 20, ProviderDotDiameter(256, 12, 20), 1e-9, "taller than wide");
    CheckNear(256.0 / 20, ProviderDotDiameter(256, 20, 12), 1e-9, "wider than tall");
    CheckNear(256.0 / 12, ProviderDotDiameter(256, 12, 0), 1e-9, "no height reported");
  }
  {
    TEST_CASE("unmeasuredCanvasUsesTheIosCanvas");
    CheckNear(256.0 / 14, ProviderDotDiameter(0, 14, 14), 1e-9, "side 0 -> 256");
    CheckNear(256.0 / 14, ProviderDotDiameter(-5, 14, 14), 1e-9, "negative side -> 256");
  }
  {
    TEST_CASE("shapelessGridUsesTheDefaultColumns");
    CheckNear(256.0 / kProviderDotDefaultGridWidth, ProviderDotDiameter(0, 0, 0), 1e-9,
              "nothing known");
    CheckNear(288.0 / kProviderDotDefaultGridWidth, ProviderDotDiameter(288, 0, 0), 1e-9,
              "side known, grid not");
    Check(0 < ProviderDotDiameter(0, 0, 0), "never zero");
  }
}

// ---- ProviderLocations.h: the family rides the row --------------------------

void ProviderRowTests() {
  {
    TEST_CASE("rowEqualityIncludesTheFamily");
    ProviderLocationRow a;
    a.clientId = "c";
    a.ipFamily = "dualstack";
    a.ipFamilyLabel = "both";
    ProviderLocationRow b = a;
    Check(a == b, "copies are equal");
    b.ipFamily = "v4-only";
    b.ipFamilyLabel = "v4";
    Check(a != b, "a family change (the local downgrade) is a row change, so the "
                  "sheet re-renders it");
    ProviderLocationRow legacy;
    legacy.clientId = "c";
    Check(legacy.ipFamily.empty() && legacy.ipFamilyLabel.empty(),
          "an SDK without the field leaves both empty");
  }
}

// ---- NetPolicy.h: the IPv6 half of THE table --------------------------------

void NetPolicyV6Tests() {
  {
    TEST_CASE("captureSetIsTheValidatedEightPrefixes");
    const net::V6Prefix expected[] = {
        {0x0000'0000'0000'0000ull, 0, 1}, {0x8000'0000'0000'0000ull, 0, 2},
        {0xC000'0000'0000'0000ull, 0, 3}, {0xE000'0000'0000'0000ull, 0, 4},
        {0xF000'0000'0000'0000ull, 0, 5}, {0xF800'0000'0000'0000ull, 0, 6},
        {0xFE00'0000'0000'0000ull, 0, 9}, {0xFEC0'0000'0000'0000ull, 0, 10},
    };
    Check(net::kTunCaptureV6Count == 8, "eight prefixes");
    for (std::size_t i = 0; i < net::kTunCaptureV6Count && i < 8; ++i) {
      Check(net::kTunCaptureV6[i] == expected[i],
            "index " + std::to_string(i) + ": " + Text6(net::kTunCaptureV6[i]));
    }
  }
  {
    TEST_CASE("captureUnionBypassCoversEverythingExactly");
    long double covered = 0;
    for (const auto& p : net::kTunCaptureV6) covered += std::ldexp(1.0L, -int(p.prefix));
    for (const auto& p : net::kLocalBypassV6) covered += std::ldexp(1.0L, -int(p.prefix));
    Check(covered == 1.0L, "sum of 2^-prefix over capture and bypass is exactly 1");
    for (const auto& c : net::kTunCaptureV6) {
      for (const auto& b : net::kLocalBypassV6) {
        Check(!net::detail::Contains(b, c) && !net::detail::Contains(c, b),
              Text6(c) + " must not intersect " + Text6(b));
      }
    }
  }
  {
    TEST_CASE("capturePrefixesAreSortedAndMinimal");
    for (std::size_t i = 1; i < net::kTunCaptureV6Count; ++i) {
      const auto& a = net::kTunCaptureV6[i - 1];
      const auto& b = net::kTunCaptureV6[i];
      Check(a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo), "ascending");
    }
    Check(net::kTunCaptureV6[0].prefix == 1, "::/1 is one prefix, not split further");
  }
  {
    TEST_CASE("bypassProbes");
    struct { uint8_t addr[16]; bool bypass; const char* label; } probes[] = {
        {{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0x65, 0, 0x49, 0, 0x70, 0, 0x65}, false,
         "2001:db8::65:49:70:65 (the SDK's in-tunnel resolver) is captured"},
        {{0x26, 0x06, 0x47, 0x00, 0x47, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0x11, 0x11}, false,
         "2606:4700:4700::1111 is captured"},
        {{0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, true, "fe80::1 bypasses"},
        {{0xfe, 0xbf, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, true,
         "febf::1 (top of fe80::/10) bypasses"},
        {{0xfe, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, false,
         "fec0::1 (just above fe80::/10) is captured"},
        {{0xfc, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, true, "fc00::1 bypasses"},
        {{0xfd, 0x00, 0x75, 0x72, 0x6e, 0x65, 0x12, 0x34, 0, 0, 0, 0, 0, 0, 0, 1}, true,
         "fd00:7572:6e65:1234::1 (our own tun ULA) bypasses; its on-link /64 "
         "still wins"},
        {{0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
          0xff, 0xff}, true, "top of fc00::/7 bypasses"},
        {{0xfe, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, false,
         "fe00::1 (just above fc00::/7) is captured"},
        {{0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, true, "ff02::1 bypasses"},
        {{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
          0xff, 0xff}, true, "ffff::ffff bypasses"},
        {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, true, "::1 bypasses (loopback)"},
        {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, false, ":: is captured"},
        {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2}, false, "::2 is captured"},
        {{0, 0x64, 0xff, 0x9b, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, false,
         "64:ff9b::1 (NAT64) is captured"},
        {{0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
          0xff, 0xff}, false, "top of ::/1 is captured"},
        {{0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, false, "8000:: is captured"},
    };
    for (const auto& p : probes) {
      Check(net::IsLocalBypassV6(p.addr) == p.bypass, p.label);
    }
  }
  {
    TEST_CASE("bytesAndFromBytesRoundTrip");
    for (const auto& p : net::kLocalBypassV6) {
      const auto bytes = p.Bytes();
      uint8_t raw[16];
      for (int i = 0; i < 16; ++i) raw[i] = bytes[static_cast<std::size_t>(i)];
      Check(net::V6Prefix::FromBytes(raw, p.prefix) == p, "round trip " + Text6(p));
    }
    const uint8_t full[16] = {0x20, 0x01, 0x0d, 0xb8, 0x11, 0x22, 0x33, 0x44,
                              0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc};
    const net::V6Prefix p = net::V6Prefix::FromBytes(full, 128);
    Check(p.hi == 0x2001'0db8'1122'3344ull && p.lo == 0x5566'7788'99aa'bbccull,
          "both halves populated in network order");
    const auto back = p.Bytes();
    bool same = true;
    for (int i = 0; i < 16; ++i)
      if (back[static_cast<std::size_t>(i)] != full[i]) same = false;
    Check(same, "Bytes() reproduces the input");
  }
  {
    TEST_CASE("v4TableUnchanged");
    Check(net::kTunCaptureV4Count == 31, "the v4 capture set still has 31 prefixes");
  }
}

}  // namespace

int main() {
  std::cout << "IpFamilyStatus counting\n";
  CountingTests();
  std::cout << "IpFamilyStatus ranking\n";
  RankingTests();
  std::cout << "IpFamilyStatus lines\n";
  LineTests();
  std::cout << "ProviderDotDiameter\n";
  DotDiameterTests();
  std::cout << "ProviderLocationRow\n";
  ProviderRowTests();
  std::cout << "NetPolicy v6\n";
  NetPolicyV6Tests();

  std::cout << "\n" << gCases << " cases, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
