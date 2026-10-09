// When the update checker asks GitHub again, and when it tells the user it
// has not been able to, decided pure.
//
// Requests are anonymous: GitHub allows 60 an hour per IP address, and every
// user behind one exit or one NAT shares them. A refused request carries when
// to ask again: Retry-After (seconds, for a secondary limit), or
// X-RateLimit-Reset (a Unix time on GitHub's clock) with X-RateLimit-Remaining
// at 0. The checker never asks before then, and never waits more than a day
// whatever a header says, so a hostile or broken header cannot stop checks
// for good.
//
// A check that cannot succeed must not fail silently forever: when none has
// succeeded for kStaleAfter, the app says "Couldn't check for updates since
// <date>" until one does.
//
// Pure and header-only: tools/update-release-tests.cpp runs it on any host.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cstdint>

namespace urnw::update {

// How long without a successful check before the app says so.
inline constexpr std::int64_t kStaleAfterSeconds = 72 * 60 * 60;

// The longest a refused request can push the next one out.
inline constexpr std::int64_t kMaxBackoffSeconds = 24 * 60 * 60;

// What a refused release-list request said about asking again.
struct RateLimit {
  // Retry-After in seconds, 0 when absent.
  std::int64_t retryAfterSeconds = 0;
  // X-RateLimit-Reset in Unix seconds on GitHub's clock, 0 when absent.
  std::int64_t resetUnixSeconds = 0;
  // X-RateLimit-Remaining was 0.
  bool exhausted = false;
  // The response's Date header in Unix seconds, 0 when absent: the reset is
  // measured against it, not against this machine's clock.
  std::int64_t serverUnixSeconds = 0;
};

// How many seconds after a refused request the next one may go: at least
// `cadenceSeconds`, later when the response asked for it, at most
// kMaxBackoffSeconds unless the cadence itself is longer.
inline constexpr std::int64_t NextCheckDelaySeconds(std::int64_t cadenceSeconds,
                                                    const RateLimit& limit) {
  std::int64_t wait = 0;
  if (limit.retryAfterSeconds > 0) wait = limit.retryAfterSeconds;
  if (limit.exhausted && limit.resetUnixSeconds > 0 && limit.serverUnixSeconds > 0) {
    wait = std::max(wait, limit.resetUnixSeconds - limit.serverUnixSeconds);
  }
  wait = std::min(wait, kMaxBackoffSeconds);
  return std::max(cadenceSeconds, wait);
}

// Whether the app should say it has not been able to check: automatic checks
// are on, and the last success (or the first attempt, when none has succeeded)
// is more than kStaleAfter ago.
inline constexpr bool CheckIsStale(std::int64_t nowUnixSeconds, std::int64_t lastSuccessUnixSeconds,
                                   bool checking) {
  return checking && lastSuccessUnixSeconds > 0 &&
         nowUnixSeconds - lastSuccessUnixSeconds > kStaleAfterSeconds;
}

}  // namespace urnw::update
