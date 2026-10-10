// How long ago something happened, in the units urnw::RelativeTime
// (StatsFormat.h) words it in: "now" under 5 seconds, then whole seconds,
// minutes, hours and days, and a date from 7 days on. The connection events,
// the block log and the Account > Sessions "Last used" line all read it; the
// Sessions screen is why it reaches days and a date (server
// session/REVOKE-UI-FINAL.md §3.1), the cut-over android's and apple's system
// relative formatters make at a week.
//
// Pure, for the reason ExtenderPresentation.h gives: tools/sessions-tests.cpp
// checks every boundary on any host with a C++20 compiler. StatsFormat.cpp
// supplies the store's words and the Windows date.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

namespace urnw::relativetime {

enum class Unit { Now, Seconds, Minutes, Hours, Days, Date };

struct Span {
  Unit unit = Unit::Now;
  // whole units elapsed; 0 for Now and Date
  int64_t count = 0;

  bool operator==(const Span&) const = default;
};

// under this many seconds a time is "now"
inline constexpr int64_t kNowSeconds = 5;
// from this many days on a time is a date
inline constexpr int64_t kDateDays = 7;

// A time ahead of `nowMillis` (a skewed clock) reads as now.
constexpr Span SpanFor(int64_t thenMillis, int64_t nowMillis) {
  const int64_t elapsedMillis = nowMillis - thenMillis;
  const int64_t seconds = 0 < elapsedMillis ? elapsedMillis / 1000 : 0;
  constexpr int64_t kMinute = 60;
  constexpr int64_t kHour = 60 * kMinute;
  constexpr int64_t kDay = 24 * kHour;
  if (seconds < kNowSeconds) return {Unit::Now, 0};
  if (seconds < kMinute) return {Unit::Seconds, seconds};
  if (seconds < kHour) return {Unit::Minutes, seconds / kMinute};
  if (seconds < kDay) return {Unit::Hours, seconds / kHour};
  if (seconds < kDateDays * kDay) return {Unit::Days, seconds / kDay};
  return {Unit::Date, 0};
}

// The store key a span reads with: "now", or the "{count}<unit> ago" key of
// its unit. "" for a date, which the platform formats.
constexpr const char* KeyFor(Unit unit) {
  switch (unit) {
    case Unit::Now: return "now";
    case Unit::Seconds: return "seconds_ago_abbrev";
    case Unit::Minutes: return "minutes_ago_abbrev";
    case Unit::Hours: return "hours_ago_abbrev";
    case Unit::Days: return "days_ago_abbrev";
    case Unit::Date: return "";
  }
  return "";
}

}  // namespace urnw::relativetime
