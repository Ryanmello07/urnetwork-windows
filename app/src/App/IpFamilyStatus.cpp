// SPDX-License-Identifier: MPL-2.0
//
// No pch.h on purpose — see IpFamilyStatus.h. App.vcxproj compiles this with
// PrecompiledHeader=NotUsing.
#include "IpFamilyStatus.h"

#include <algorithm>

namespace urnw {
namespace {

// the grid states of a live provider, as the SDK spells them (the same
// vocabulary ConnectCanvas::ParsePointState reads)
constexpr std::string_view kConnectedState = "Added";
constexpr std::string_view kConnectingState = "InEvaluation";

std::size_t IndexOf(IpFamilyColumn column) {
  switch (column) {
    case IpFamilyColumn::Dualstack: return 0;
    case IpFamilyColumn::V4: return 1;
    case IpFamilyColumn::V6: break;
  }
  return 2;
}

}  // namespace

IpFamilyColumn IpFamilyColumnFor(std::string_view ipFamily) {
  if (ipFamily == "dualstack") return IpFamilyColumn::Dualstack;
  if (ipFamily == "v6-only") return IpFamilyColumn::V6;
  // "v4-only", legacy (empty) and anything newer than this build
  return IpFamilyColumn::V4;
}

int IpFamilyColumnRank(IpFamilyColumn column) {
  switch (column) {
    case IpFamilyColumn::Dualstack: return 0;
    case IpFamilyColumn::V4:
    case IpFamilyColumn::V6: break;
  }
  return 1;
}

const char* IpFamilyColumnToken(IpFamilyColumn column) {
  switch (column) {
    case IpFamilyColumn::Dualstack: return "both";
    case IpFamilyColumn::V6: return "v6";
    case IpFamilyColumn::V4: break;
  }
  return "v4";
}

IpFamilyColumn IpFamilyColumnForToken(std::string_view token) {
  if (token == "both") return IpFamilyColumn::Dualstack;
  if (token == "v6") return IpFamilyColumn::V6;
  return IpFamilyColumn::V4;
}

std::vector<IpFamilyColumnStatus> IpFamilyColumnStatuses(
    const std::vector<IpFamilyPoint>& points) {
  std::vector<IpFamilyColumnStatus> statuses;
  statuses.reserve(3);
  for (IpFamilyColumn column : kIpFamilyColumns) {
    IpFamilyColumnStatus status;
    status.column = column;
    statuses.push_back(status);
  }
  for (const IpFamilyPoint& point : points) {
    // a bare cell (no client id) is a position, not a provider
    if (point.clientId.empty()) continue;
    IpFamilyColumnStatus& status = statuses[IndexOf(IpFamilyColumnFor(point.ipFamily))];
    if (point.state == kConnectedState) {
      status.connectedCount += 1;
    } else if (point.state == kConnectingState) {
      status.connectingCount += 1;
    }
    // EvaluationFailed, NotAdded, Removed and anything this build does not
    // name: a provider the SDK has not accepted, or has let go of
  }
  return statuses;
}

std::vector<IpFamilyTier> IpFamilyColumnTiers(
    const std::vector<IpFamilyColumnStatus>& statuses) {
  // the best rank among the columns carrying traffic; none while nothing is
  // connected, so nothing can be Best then
  bool anyConnected = false;
  int bestRank = 0;
  for (const IpFamilyColumnStatus& status : statuses) {
    if (status.connectedCount <= 0) continue;
    const int rank = IpFamilyColumnRank(status.column);
    if (!anyConnected || rank < bestRank) bestRank = rank;
    anyConnected = true;
  }
  std::vector<IpFamilyTier> tiers;
  tiers.reserve(statuses.size());
  for (const IpFamilyColumnStatus& status : statuses) {
    if (status.Unavailable()) {
      tiers.push_back(IpFamilyTier::Unavailable);
    } else if (0 < status.connectedCount && anyConnected &&
               IpFamilyColumnRank(status.column) == bestRank) {
      tiers.push_back(IpFamilyTier::Best);
    } else {
      tiers.push_back(IpFamilyTier::Active);
    }
  }
  return tiers;
}

std::vector<IpFamilyStatusLine> IpFamilyStatusLines(const IpFamilyColumnStatus& status) {
  std::vector<IpFamilyStatusLine> lines;
  if (0 < status.connectedCount) {
    lines.push_back({IpFamilyLineKind::Connected, status.connectedCount});
  }
  if (0 < status.connectingCount) {
    lines.push_back({IpFamilyLineKind::Connecting, status.connectingCount});
  }
  if (lines.empty()) {
    lines.push_back({IpFamilyLineKind::Disconnected, 0});
  }
  return lines;
}

}  // namespace urnw
