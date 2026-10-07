// The pure logic behind the connect drawer's IP-family status row (connect/
// IPV6.md D2): which providers count in which column, which column is bright,
// and which lines a column shows. Shared with the provider-locations rows,
// which tag each provider with the same family vocabulary.
//
// Pure standard C++ — no WinRT, no localization, no SDK header — so
// tools/ip-family-tests.cpp runs it on any host (App.vcxproj compiles this
// with PrecompiledHeader=NotUsing, like ProviderLocations.cpp). The WinUI
// layer (IpFamilyStatusRow.cpp) converts the SDK's ProviderGridPoint into the
// input struct below and draws the result.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace urnw {

// The three columns, in display order, which is also rank order. Each is a
// provider CATEGORY, never a capability: a dualstack provider counts once,
// under Dualstack, and never under V4 or V6 as well.
enum class IpFamilyColumn { Dualstack, V4, V6 };

inline constexpr IpFamilyColumn kIpFamilyColumns[] = {
    IpFamilyColumn::Dualstack, IpFamilyColumn::V4, IpFamilyColumn::V6};

// The SDK category ("dualstack" / "v4-only" / "v6-only") -> the column. Legacy
// (empty) and any category this build does not know read as v4-only, which is
// the normalization the SDK itself applies at its boundary: an unknown
// category must not claim v6 it cannot prove.
IpFamilyColumn IpFamilyColumnFor(std::string_view ipFamily);

// The family's rank, lower is better: dualstack carries both families, and
// IPv4 and IPv6 tie below it.
int IpFamilyColumnRank(IpFamilyColumn column);

// The label token the apps share for a family ("both" / "v4" / "v6"), which
// is also what the SDK's ipFamilyLabel carries per provider (the tag on the
// provider rows).
const char* IpFamilyColumnToken(IpFamilyColumn column);

// The token -> the column, for the provider rows (which get the label from
// the SDK) and the tests. Unknown tokens read as v4, like unknown categories.
IpFamilyColumn IpFamilyColumnForToken(std::string_view token);

// The emphasis of a column.
enum class IpFamilyTier {
  // the best-ranked column with a connected provider, shared on a tie
  Best,
  // something connected or connecting, but a better column is connected
  Active,
  // nothing connected or connecting
  Unavailable,
};

// One status line of a column, in display order: connected, then connecting,
// or disconnected alone. The kind is the line's identity: a count change is
// the same line with a new number, a kind appearing or vanishing is a line
// coming or going.
enum class IpFamilyLineKind { Connected, Connecting, Disconnected };

struct IpFamilyStatusLine {
  IpFamilyLineKind kind = IpFamilyLineKind::Disconnected;
  int64_t count = 0;  // 0 for Disconnected

  bool operator==(const IpFamilyStatusLine& o) const {
    return kind == o.kind && count == o.count;
  }
  bool operator!=(const IpFamilyStatusLine& o) const { return !(*this == o); }
};

// One grid point, as much of it as the counting needs.
struct IpFamilyPoint {
  std::string clientId;  // the window-local id; empty for a bare cell
  std::string state;     // the SDK's ProviderGridPoint::State
  std::string ipFamily;  // the SDK's ProviderGridPoint::IpFamily
};

struct IpFamilyColumnStatus {
  IpFamilyColumn column = IpFamilyColumn::Dualstack;
  // the Added providers in this category
  int64_t connectedCount = 0;
  // the InEvaluation providers in this category
  int64_t connectingCount = 0;

  // nothing connected or connecting: the column reads "disconnected"
  bool Unavailable() const { return connectedCount == 0 && connectingCount == 0; }

  bool operator==(const IpFamilyColumnStatus& o) const {
    return column == o.column && connectedCount == o.connectedCount &&
           connectingCount == o.connectingCount;
  }
  bool operator!=(const IpFamilyColumnStatus& o) const { return !(*this == o); }
};

// The three columns, always present, in display order, counting only the
// connected (Added) and the connecting (InEvaluation) providers of each
// category. A provider that failed evaluation, was not added, or is on its
// way out (it lingers on the grid for the removal tween) counts as nothing,
// and so does a bare cell (no client id), which is a position, not a provider.
std::vector<IpFamilyColumnStatus> IpFamilyColumnStatuses(
    const std::vector<IpFamilyPoint>& points);

// The tier of every column, index-aligned with `statuses`: the best rank among
// the columns with a connected provider is Best (every column at that rank, on
// a tie), any other column with a live provider is Active, and the rest are
// Unavailable. A column that is only connecting carries no traffic yet, so it
// is never Best.
std::vector<IpFamilyTier> IpFamilyColumnTiers(
    const std::vector<IpFamilyColumnStatus>& statuses);

// The status lines of a column: the non-zero counts, connected first, or
// Disconnected alone when both are zero. Never empty, never more than two.
std::vector<IpFamilyStatusLine> IpFamilyStatusLines(const IpFamilyColumnStatus& status);

}  // namespace urnw
