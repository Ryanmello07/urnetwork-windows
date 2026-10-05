// Executable spec for the SN payout line under the points figure on Earnings
// (App/SnPayoutPresentation.h): which line shows (no coldkey, nothing
// claimable, claimable), the current epoch's end, claim-open and expiry times
// formatted from the SDK's epoch schedule, and - with the Solana panel's
// decision (App/SolanaWalletPresentation.h) - that the final USDC payout line
// shows only while USDC is pending. Run against the SAME sources the app
// compiles, on any host with a C++20 compiler.
//
// The WinUI half (WalletPage's line, its Claim and Set coldkey actions) cannot
// be built off Windows at all; what is verified here is every decision it makes
// before it touches a XAML object.
//
//   c++ -std=c++20 -I ../src/App sn-payout-tests.cpp ../src/App/SnPayoutPresentation.cpp ../src/App/SolanaWalletPresentation.cpp -o /tmp/sn-payout-tests && /tmp/sn-payout-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>

#include "SnPayoutPresentation.h"
#include "SolanaWalletPresentation.h"

using namespace urnw::snpayout;

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

void CheckEq(const std::string& expected, const std::string& actual, const std::string& what) {
  if (expected != actual) {
    Fail(what + ": expected \"" + expected + "\", got \"" + actual + "\"");
  }
}

struct Case {
  explicit Case(const char* name) {
    gCurrentCase = name;
    ++gCases;
  }
};
#define TEST_CASE(name) Case case_##__LINE__(name)

// The SDK's schedule for an epoch closing 2026-10-13 00:00 UTC on the mainnet
// policy: claims open 14,400 blocks (two days) later, and the share expires at
// the end of epoch e+9 (453,600 blocks after the close, less one block).
constexpr int64_t kEnd = 1'791'849'600'000;
constexpr int64_t kClaimOpen = kEnd + 14'400LL * 12'000;
constexpr int64_t kExpiry = kEnd + (453'600LL - 1) * 12'000;
// 2026-10-06 00:00 UTC, a week before the close
constexpr int64_t kNow = 1'791'244'800'000;

EpochSchedule Schedule() {
  EpochSchedule schedule;
  schedule.epoch = 9;
  schedule.endMillis = kEnd;
  schedule.claimOpenMillis = kClaimOpen;
  schedule.expiryMillis = kExpiry;
  return schedule;
}

// names each time by the millis it was formatted from
std::string Tag(int64_t millis) { return "t" + std::to_string(millis); }

PayoutLineView Line(bool walletKnown, bool hasColdkey, int64_t claimableRao,
                    std::optional<EpochSchedule> schedule, int64_t now = kNow) {
  return PayoutLineFor(walletKnown, hasColdkey, claimableRao, schedule, now, Tag);
}

void CheckTimes(const PayoutLineView& view) {
  Check(view.showTimes, "the times show");
  CheckEq(Tag(kEnd), view.epochEnd, "epoch end");
  CheckEq(Tag(kClaimOpen), view.claimOpen, "claim open");
  CheckEq(Tag(kExpiry), view.expiry, "expiry");
}

}  // namespace

int main() {
  {
    TEST_CASE("no coldkey asks for one");
    for (int64_t rao : {int64_t{0}, int64_t{3'241'000'000}}) {
      const PayoutLineView view = Line(true, false, rao, Schedule());
      Check(view.kind == LineKind::SetColdkey, "SetColdkey");
      Check(!view.showTimes && !view.showClaim, "no times and no Claim without a coldkey");
    }
    Check(Line(true, false, 0, std::nullopt).kind == LineKind::SetColdkey, "SetColdkey without a schedule");
  }
  {
    TEST_CASE("nothing shows while the coldkey is unknown");
    Check(Line(false, false, 0, Schedule()) == PayoutLineView{}, "hidden");
    Check(Line(false, true, 3'241'000'000, Schedule()) == PayoutLineView{}, "hidden with a wallet read pending");
  }
  {
    TEST_CASE("nothing claimable explains the schedule without Claim");
    const PayoutLineView view = Line(true, true, 0, Schedule());
    Check(view.kind == LineKind::Schedule, "Schedule");
    CheckTimes(view);
    Check(!view.showClaim, "no Claim");
  }
  {
    TEST_CASE("something claimable adds Claim");
    const PayoutLineView view = Line(true, true, 3'241'000'000, Schedule());
    Check(view.kind == LineKind::Schedule, "Schedule");
    CheckTimes(view);
    Check(view.showClaim, "Claim");
  }
  {
    TEST_CASE("without the schedule the explanation stands alone");
    const PayoutLineView view = Line(true, true, 1, std::nullopt);
    Check(view.kind == LineKind::Schedule && !view.showTimes, "no times");
    Check(view.epochEnd.empty() && view.claimOpen.empty() && view.expiry.empty(), "no time texts");
    Check(view.showClaim, "Claim still shows");
  }
  {
    TEST_CASE("an ended epoch waits for the next read");
    Check(!Line(true, true, 0, Schedule(), kEnd).showTimes, "no times at the close");
    CheckTimes(Line(true, true, 0, Schedule(), kEnd - 1));
  }
  {
    TEST_CASE("the times are local dates and times from the epoch data");
    CheckEq("2026-10-13 00:00", FormatScheduleTime(kEnd, 0), "end, UTC");
    CheckEq("2026-10-15 00:00", FormatScheduleTime(kClaimOpen, 0), "claim open, UTC");
    CheckEq("2026-12-14 23:59", FormatScheduleTime(kExpiry, 0), "expiry, UTC");
    // Berlin in summer time, then in winter time by the expiry
    CheckEq("2026-10-13 02:00", FormatScheduleTime(kEnd, 120), "end, Berlin");
    CheckEq("2026-12-15 00:59", FormatScheduleTime(kExpiry, 60), "expiry, Berlin");
    // west of UTC the close is the evening before
    CheckEq("2026-10-12 17:00", FormatScheduleTime(kEnd, -420), "end, Los Angeles");
    CheckEq("1969-12-31 23:59", FormatScheduleTime(-60'000, 0), "before the epoch");

    const PayoutLineView view = PayoutLineFor(
        true, true, 0, Schedule(), kNow, [](int64_t millis) { return FormatScheduleTime(millis, 120); });
    CheckEq("2026-10-13 02:00", view.epochEnd, "line end");
    CheckEq("2026-10-15 02:00", view.claimOpen, "line claim open");
    CheckEq("2026-12-15 01:59", view.expiry, "line expiry (the page's offset is per instant)");
  }
  {
    TEST_CASE("the final USDC payout line shows only while USDC is pending");
    using namespace urnw::solana;
    LegacyReads reads;
    reads.wallets = true;
    reads.payout = true;
    reads.payments = true;
    const SolanaPanelView pending = SolanaPanelFor(LegacyState::Ready, reads, std::nullopt, 3'870'000'000);
    Check(pending.showWaitingLine && !pending.showCard, "the line, with no payout wallet");
    CheckEq("3.87", pending.pendingUsd, "the amount");
    Check(!SolanaPanelFor(LegacyState::Ready, reads, std::nullopt, 0).showWaitingLine, "nothing pending");
    Check(!SolanaPanelFor(LegacyState::Ready, reads, std::nullopt, 4'000'000).showWaitingLine, "a sub-cent remainder");
    Check(!SolanaPanelFor(LegacyState::Loading, reads, std::nullopt, 3'870'000'000).showWaitingLine, "not before the reads");
  }

  std::cout << gCases << " cases, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
