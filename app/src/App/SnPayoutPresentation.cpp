// SPDX-License-Identifier: MPL-2.0
#include "SnPayoutPresentation.h"

#include <cstdio>

namespace urnw::snpayout {

PayoutLineView PayoutLineFor(bool walletKnown, bool hasColdkey, int64_t totalClaimableRao,
                             const std::optional<EpochSchedule>& schedule, int64_t nowMillis,
                             const std::function<std::string(int64_t)>& formatTime) {
  PayoutLineView view;
  if (!walletKnown) return view;
  if (!hasColdkey) {
    view.kind = LineKind::SetColdkey;
    return view;
  }
  view.kind = LineKind::Schedule;
  if (schedule && nowMillis < schedule->endMillis) {
    view.showTimes = true;
    view.epochEnd = formatTime(schedule->endMillis);
    view.claimOpen = formatTime(schedule->claimOpenMillis);
    view.expiry = formatTime(schedule->expiryMillis);
  }
  view.showClaim = totalClaimableRao > 0;
  return view;
}

std::string FormatScheduleTime(int64_t millis, int32_t utcOffsetMinutes) {
  constexpr int64_t kDayMillis = 86'400'000;
  const int64_t local = millis + static_cast<int64_t>(utcOffsetMinutes) * 60'000;
  int64_t days = local / kDayMillis;
  int64_t rest = local % kDayMillis;
  if (rest < 0) {
    rest += kDayMillis;
    --days;
  }
  // civil-from-days (Howard Hinnant)
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const int64_t doe = days - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int64_t day = doy - (153 * mp + 2) / 5 + 1;
  const int64_t month = mp < 10 ? mp + 3 : mp - 9;
  const int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
  const int64_t minutes = rest / 60'000;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04lld-%02lld-%02lld %02lld:%02lld", static_cast<long long>(year),
                static_cast<long long>(month), static_cast<long long>(day),
                static_cast<long long>(minutes / 60), static_cast<long long>(minutes % 60));
  return buf;
}

}  // namespace urnw::snpayout
