// Everything the SN payout line decides BEFORE it touches a XAML object: which
// line shows under the points figure on Earnings, the current epoch's times it
// names and whether it carries Claim.
//
// Why it exists: provider payouts moved from weekly USDC to the UR subnet, and
// nothing on Earnings said how or when a provider is paid ("used it a month, no
// crypto"). Per the sn whitepaper (§5.2, §8.3), earnings settle every epoch and
// are paid in SN25a to the provider's Bittensor coldkey when the provider
// claims them; the app never claims by itself. The times come from the epoch
// schedule the SDK reads with the claims (the coordinator's claim-open offset
// and claim TTL), so the copy never fixes a duration.
//
// It is all here, and all pure, for the reason ExtenderPresentation.h gives:
// the windows solution has no test project and a WinUI 3 app cannot be built
// off Windows, so every decision expressed on plain values is verified by
// tools/sn-payout-tests.cpp on any host with a C++20 compiler. The SDK's
// SnEpochSchedule is mirrored as a plain view; WalletPage.cpp copies the fields
// across at its boundary.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace urnw::snpayout {

// The current epoch's settlement times (the SDK's SnEpochSchedule): when it
// ends, when its share can be claimed and when an unclaimed share expires.
struct EpochSchedule {
  int64_t epoch = 0;
  int64_t endMillis = 0;
  int64_t claimOpenMillis = 0;
  int64_t expiryMillis = 0;
};

enum class LineKind {
  Hidden,      // whether a coldkey is set is not known yet
  SetColdkey,  // set_coldkey_to_get_paid, with set_coldkey opening the coldkey flow
  Schedule,    // sn_payout_schedule, then sn_payout_schedule_times when known
};

// What the line under the points figure shows.
struct PayoutLineView {
  LineKind kind = LineKind::Hidden;
  bool showTimes = false;  // sn_payout_schedule_times, with the three times below
  std::string epochEnd;
  std::string claimOpen;
  std::string expiry;
  bool showClaim = false;  // Claim, which opens the claim dialog

  bool operator==(const PayoutLineView&) const = default;
};

// The line for the pane. Hidden while it is not known whether a coldkey is set
// (the wallet is loading, or its read failed with none cached); SetColdkey with
// none, since alpha goes only to a coldkey; otherwise the schedule, with the
// times when the schedule is known and its epoch has not already ended (an
// ended epoch waits for the next read), and Claim while something is claimable.
PayoutLineView PayoutLineFor(bool walletKnown, bool hasColdkey, int64_t totalClaimableRao,
                             const std::optional<EpochSchedule>& schedule, int64_t nowMillis,
                             const std::function<std::string(int64_t)>& formatTime);

// "2026-10-13 02:00": a schedule time in the reader's local time, given the
// zone's offset from UTC at that instant (the page asks Windows per instant, so
// a daylight change before the expiry is honored). Civil-from-days like the
// history's dates (EarningsSheets DateFromMillis), so no locale.
std::string FormatScheduleTime(int64_t millis, int32_t utcOffsetMinutes);

}  // namespace urnw::snpayout
