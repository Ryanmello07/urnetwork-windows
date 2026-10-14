// Executable spec for the SN payout line under the points figure on Earnings
// (App/SnPayoutPresentation.h): which line shows (no coldkey, nothing
// claimable, claimable), the current epoch's end, claim-open and expiry times
// formatted from the SDK's epoch schedule, and - with the Solana panel's
// decision (App/SolanaWalletPresentation.h) - that the final USDC payout line
// shows only while USDC is pending. And settlement v2 (APPS-SPEC.md, the UX
// rules): what the section beside the v1 tile shows for every state of the
// read, configured or not, pending as pending and unavailable never as a zero,
// in the store's words; and how the v2 claim dialog moves along with the SDK's
// claim events. Run against the same sources the app compiles, on any host
// with a C++20 compiler.
//
// The WinUI half (WalletPage's line and v2 section, their Claim and Set coldkey
// actions, the claim dialogs) cannot be built off Windows at all; what is
// verified here is every decision it makes before it touches a XAML object.
//
//   c++ -std=c++20 -I ../src/App sn-payout-tests.cpp ../src/App/SnPayoutPresentation.cpp ../src/App/SolanaWalletPresentation.cpp -o /tmp/sn-payout-tests && /tmp/sn-payout-tests
//
// With URNW_SN_PAYOUT_TESTS_SDK the SDK's v2 account and claim callback are
// the generated header's own (urnet::SnLedgerAccount, read from the json the
// device answers with through the header's conversions, and
// urnet::SnLedgerClaimCallback), which is what WalletPage instantiates the
// boundary templates with. The header needs nlohmann/json; both are system
// includes because the generated code does not build with -Wextra -Werror:
//
//   c++ -std=c++20 -Wall -Wextra -Werror -DURNW_SN_PAYOUT_TESTS_SDK -I ../src/App -isystem <dir of urnetwork_sdk.hpp> -isystem <dir of nlohmann/> sn-payout-tests.cpp ../src/App/SnPayoutPresentation.cpp ../src/App/SolanaWalletPresentation.cpp -o ...
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "SnPayoutPresentation.h"
#include "SolanaWalletPresentation.h"

#if defined(URNW_SN_PAYOUT_TESTS_SDK)
#include "urnetwork_sdk.hpp"
#endif

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

// A v2 account in pool 1: 3.741 alpha finalized, 0.5 of it paid, 3.241 claimable.
LedgerAccount Account() {
  LedgerAccount account;
  account.noId = 1;
  account.index = 42;
  account.finalizedRao = 3'741'000'000;
  account.claimableRao = 3'241'000'000;
  account.paidRao = 500'000'000;
  return account;
}

LedgerAccountView Block(int64_t noId, int64_t claimableRao, int64_t paidRao, int64_t earnedRao,
                        bool showPending = false, int64_t pendingRao = 0) {
  LedgerAccountView block;
  block.noId = noId;
  block.claimableRao = claimableRao;
  block.paidRao = paidRao;
  block.earnedRao = earnedRao;
  block.showPending = showPending;
  block.pendingRao = pendingRao;
  return block;
}

// the section with a coldkey set
LedgerSectionView Section(bool vault2Configured, LedgerFetch fetch,
                          const std::vector<LedgerAccount>& accounts, bool canClaim = true) {
  return LedgerSectionFor(true, vault2Configured, fetch, accounts, canClaim);
}

constexpr LedgerFetch kFetches[] = {LedgerFetch::Loading, LedgerFetch::Ready, LedgerFetch::Failed,
                                    LedgerFetch::NoDevice};

std::string FetchName(LedgerFetch fetch) {
  switch (fetch) {
    case LedgerFetch::Loading:
      return "Loading";
    case LedgerFetch::Ready:
      return "Ready";
    case LedgerFetch::Failed:
      return "Failed";
    case LedgerFetch::NoDevice:
      return "NoDevice";
  }
  return "?";
}

#if defined(URNW_SN_PAYOUT_TESTS_SDK)
using SdkLedgerAccount = urnet::SnLedgerAccount;
using SdkLedgerClaimCallback = urnet::SnLedgerClaimCallback;
#else
// The SDK's v2 account and claim callback, by the generated wrapper's names and
// types.
struct SdkLedgerAccount {
  std::string vault{};
  int64_t no_id{};
  int64_t index{};
  int64_t finalized_rao{};
  int64_t claimable_rao{};
  int64_t paid_rao{};
  bool pending{};
  int64_t pending_rao{};
};
struct SdkLedgerClaimCallback {
  std::function<void(int64_t no_id, int64_t index, std::string tx_hash, int64_t amount_rao)>
      confirmed;
  std::function<void()> done;
  std::function<void(int64_t no_id, int64_t index, std::string message)> failed;
  std::function<void(int64_t no_id, int64_t index, std::string tx_hash)> sent;
};
#endif

void CheckRow(const LedgerClaimRow& row, int64_t noId, int64_t index, int64_t amountRao,
              LedgerClaimState state, const std::string& txHash, const std::string& message,
              const std::string& what) {
  Check(row.noId == noId && row.index == index, what + ": the account");
  Check(row.amountRao == amountRao,
        what + ": amount " + std::to_string(row.amountRao) + ", want " + std::to_string(amountRao));
  Check(row.state == state, what + ": state");
  CheckEq(txHash, row.txHash, what + ": tx hash");
  CheckEq(message, row.message, what + ": message");
}

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

  // ---- settlement v2 (APPS-SPEC.md, UX rules) --------------------------------
  {
    TEST_CASE("v2: no section without a coldkey");
    for (const LedgerFetch fetch : kFetches) {
      for (const bool configured : {false, true}) {
        Check(LedgerSectionFor(false, configured, fetch, {Account()}, true) == LedgerSectionView{},
              "hidden, " + FetchName(fetch) + (configured ? ", configured" : ""));
      }
    }
  }
  {
    TEST_CASE("v2: an account shows Claimable, Paid and Earned, with Claim");
    const LedgerSectionView view = Section(true, LedgerFetch::Ready, {Account()});
    Check(view.shown && !view.loading && !view.poolTitles, "shown, one block, not loading");
    Check(view.note == LedgerNote::None, "no note");
    Check(view.accounts == std::vector<LedgerAccountView>{Block(1, 3'241'000'000, 500'000'000,
                                                                3'741'000'000)},
          "claimable 3.241, paid 0.5, earned the finalized 3.741, nothing pending");
    Check(view.showClaim && view.claimEnabled, "Claim, enabled");
  }
  {
    TEST_CASE("v2: pending shows as pending, awaiting finalization");
    LedgerAccount account = Account();
    account.pending = true;
    account.pendingRao = 3'861'000'000;
    Check(Section(true, LedgerFetch::Ready, {account}).accounts ==
              std::vector<LedgerAccountView>{
                  Block(1, 3'241'000'000, 500'000'000, 3'741'000'000, true, 120'000'000)},
          "+0.12 pending over the finalized 3.741, which stays the earned figure");
    account.pendingRao = account.finalizedRao;
    Check(!Section(true, LedgerFetch::Ready, {account}).accounts[0].showPending,
          "a pending root that does not raise the account");
    account.pendingRao = account.finalizedRao - 1;
    Check(!Section(true, LedgerFetch::Ready, {account}).accounts[0].showPending,
          "a pending cumulative below the finalized one");
    account.pending = false;
    account.pendingRao = account.finalizedRao + 1;
    Check(!Section(true, LedgerFetch::Ready, {account}).accounts[0].showPending,
          "no pending root");

    // only in the pending root so far: zeros, which are real, and the pending line
    LedgerAccount fresh;
    fresh.noId = 1;
    fresh.index = 7;
    fresh.pending = true;
    fresh.pendingRao = 50'000'000;
    const LedgerSectionView view = Section(true, LedgerFetch::Ready, {fresh});
    Check(view.accounts == std::vector<LedgerAccountView>{Block(1, 0, 0, 0, true, 50'000'000)},
          "pending only");
    Check(view.showClaim && !view.claimEnabled, "nothing to claim yet");
  }
  {
    TEST_CASE("v2: an unavailable ledger says so, never a zero");
    const LedgerSectionView view = Section(true, LedgerFetch::Failed, {Account()});
    Check(view.shown && view.note == LedgerNote::Unavailable, "unavailable");
    Check(view.accounts.empty(), "no figure, not even the last read's");
    Check(view.showClaim && !view.claimEnabled, "Claim stays disabled");
    Check(!view.loading, "not loading");
  }
  {
    TEST_CASE("v2: loading keeps the last read's accounts");
    const LedgerSectionView first = Section(true, LedgerFetch::Loading, {});
    Check(first.shown && first.loading && first.accounts.empty(), "the first read");
    Check(first.note == LedgerNote::None, "no note while loading");
    Check(first.showClaim && !first.claimEnabled, "Claim waits for the accounts");
    const LedgerSectionView again = Section(true, LedgerFetch::Loading, {Account()});
    Check(again.loading && again.accounts.size() == 1, "a reload keeps the accounts on screen");
    Check(again.claimEnabled, "and their Claim");
  }
  {
    TEST_CASE("v2: no account yet, and no device session");
    const LedgerSectionView none = Section(true, LedgerFetch::Ready, {});
    Check(none.shown && none.note == LedgerNote::NoAccount && none.accounts.empty(), "no account");
    Check(none.showClaim && !none.claimEnabled, "nothing to claim");
    const LedgerSectionView noDevice = Section(true, LedgerFetch::NoDevice, {Account()}, false);
    Check(noDevice.shown && noDevice.note == LedgerNote::NeedsSession, "the session note");
    Check(noDevice.accounts.empty(), "no figure without a read");
    Check(noDevice.showClaim && !noDevice.claimEnabled, "Claim disabled");
  }
  {
    TEST_CASE("v2: each note says it in the store's words");
    // the words the Android and Linux apps use: unavailable is the shared
    // sn_ledger_unavailable, the empty state the Linux tile's, and the missing
    // session the v1 tile's
    CheckEq("sn_ledger_unavailable", LedgerNoteKey(LedgerNote::Unavailable), "unavailable");
    CheckEq("earnings_claims_need_session", LedgerNoteKey(LedgerNote::NeedsSession), "no session");
    CheckEq("no_claims_yet_linux", LedgerNoteKey(LedgerNote::NoAccount), "no account yet");
    CheckEq("", LedgerNoteKey(LedgerNote::None), "no note: no line");
    CheckEq("sn_ledger_unavailable", LedgerNoteKey(Section(true, LedgerFetch::Failed, {}).note),
            "a failed read's line");
  }
  {
    TEST_CASE("v2: before vault2 is configured, balances only and no Claim");
    const LedgerSectionView view = Section(false, LedgerFetch::Ready, {Account()});
    Check(view.shown && view.accounts.size() == 1, "the balances the ledger returned");
    Check(!view.showClaim && !view.claimEnabled, "no Claim");
    Check(Section(false, LedgerFetch::Loading, {Account()}).shown, "kept while reloading");
    Check(Section(false, LedgerFetch::Ready, {}) == LedgerSectionView{}, "no account: nothing");
    Check(Section(false, LedgerFetch::Loading, {}) == LedgerSectionView{}, "loading: nothing");
    Check(Section(false, LedgerFetch::Failed, {Account()}) == LedgerSectionView{},
          "a ledger not served yet: nothing");
    Check(Section(false, LedgerFetch::NoDevice, {Account()}) == LedgerSectionView{},
          "no device: nothing");
  }
  {
    TEST_CASE("v2: Claim needs something claimable and a device");
    Check(!Section(true, LedgerFetch::Ready, {Account()}, false).claimEnabled, "no device");
    LedgerAccount paid = Account();
    paid.claimableRao = 0;
    paid.paidRao = paid.finalizedRao;
    Check(!Section(true, LedgerFetch::Ready, {paid}).claimEnabled, "all paid");
    LedgerAccount other = Account();
    other.noId = 2;
    other.index = 3;
    other.finalizedRao = 900'000'000;
    other.claimableRao = 900'000'000;
    other.paidRao = 0;
    const LedgerSectionView two = Section(true, LedgerFetch::Ready, {paid, other});
    Check(two.poolTitles, "two pools: each block names its pool");
    Check(two.accounts == std::vector<LedgerAccountView>{Block(1, 0, 3'741'000'000, 3'741'000'000),
                                                         Block(2, 900'000'000, 0, 900'000'000)},
          "one block per account, in the ledger's order");
    Check(two.claimEnabled, "one claimable account is enough");
  }
  {
    TEST_CASE("v2: an absurd ledger answer overflows neither the section nor the dialog");
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    LedgerAccount huge = Account();
    huge.finalizedRao = kMax;
    huge.claimableRao = kMax;
    huge.paidRao = 0;
    LedgerAccount other = huge;
    other.noId = 2;
    Check(Section(true, LedgerFetch::Ready, {huge, other}).claimEnabled,
          "Claim, without summing the accounts");
    LedgerClaimProgress progress({huge, other}, 0.5);
    Check(progress.ClaimableRao() == kMax, "the dialog's total saturates");
    Check(progress.CanClaim(true) && progress.Start(), "and Claim still sends");
    progress.Confirmed(1, 42, "0xabc", kMax);
    Check(progress.ClaimableRao() == kMax, "the other account is left");
  }
  {
    TEST_CASE("v2 claim: the dialog lists the claimable accounts");
    LedgerAccount paid = Account();
    paid.noId = 3;
    paid.claimableRao = 0;
    LedgerAccount other = Account();
    other.noId = 2;
    other.index = 3;
    other.claimableRao = 900'000'000;
    const LedgerClaimProgress progress({Account(), paid, other}, 0.5);
    Check(progress.rows().size() == 2, "two rows");
    CheckRow(progress.rows()[0], 1, 42, 3'241'000'000, LedgerClaimState::Idle, "", "", "pool 1");
    CheckRow(progress.rows()[1], 2, 3, 900'000'000, LedgerClaimState::Idle, "", "", "pool 2");
    Check(progress.ClaimableRao() == 4'141'000'000, "the total");
    Check(!progress.sending() && !progress.needsGas() && progress.failure().empty(), "at rest");
    Check(progress.CanClaim(true), "Claim enabled");
    Check(!progress.CanClaim(false), "not without a device session");
    Check(!LedgerClaimProgress({paid}, 0.5).CanClaim(true), "nothing to claim");
  }
  {
    TEST_CASE("v2 claim: Sent with the transaction, Confirmed with the Claimed amount, then Done");
    LedgerAccount other = Account();
    other.noId = 2;
    other.index = 3;
    other.claimableRao = 900'000'000;
    LedgerClaimProgress progress({Account(), other}, std::nullopt);
    Check(progress.Start(), "Claim starts");
    Check(progress.sending() && !progress.CanClaim(true), "one claim at a time");
    Check(!progress.Start(), "a second press sends nothing");
    CheckRow(progress.rows()[0], 1, 42, 3'241'000'000, LedgerClaimState::Sending, "", "",
             "sending");
    progress.Sent(1, 42, "0xabc");
    CheckRow(progress.rows()[0], 1, 42, 3'241'000'000, LedgerClaimState::Sent, "0xabc", "", "sent");
    progress.Confirmed(1, 42, "0xabc", 3'241'000'001);
    CheckRow(progress.rows()[0], 1, 42, 3'241'000'001, LedgerClaimState::Confirmed, "0xabc", "",
             "confirmed with the event's amount");
    Check(progress.ClaimableRao() == 900'000'000, "a confirmed account leaves the total");
    progress.Done();
    Check(!progress.sending(), "done");
    CheckRow(progress.rows()[1], 2, 3, 900'000'000, LedgerClaimState::Idle, "", "",
             "never reported on");
    Check(progress.CanClaim(true), "the rest can be claimed again");
    Check(progress.Start(), "again");
    CheckRow(progress.rows()[0], 1, 42, 3'241'000'001, LedgerClaimState::Confirmed, "0xabc", "",
             "a confirmed account is not sent again");
    progress.Confirmed(2, 3, "0xdef", 0);
    CheckRow(progress.rows()[1], 2, 3, 900'000'000, LedgerClaimState::Confirmed, "0xdef", "",
             "an event without an amount keeps the claimable");
    progress.Done();
    Check(progress.ClaimableRao() == 0 && !progress.CanClaim(true) && !progress.Start(),
          "nothing left");
  }
  {
    TEST_CASE("v2 claim: needs_gas opens the gas top-up");
    // the SDK's preflight refusal (snNeedsGasMessage)
    const std::string needsGas =
        "needs_gas: this claim needs up to 0.0173 TAO of gas, the gas key 0x00aa has 0.0050 TAO; "
        "send TAO to 5GAS";
    LedgerClaimProgress progress({Account()}, 0.005);
    Check(!progress.needsGas() && progress.CanClaim(true),
          "a key with some TAO tries: the SDK's preflight decides");
    Check(progress.Start(), "Claim starts");
    progress.Failed(1, 42, needsGas);
    Check(progress.needsGas(), "the funding hint shows");
    CheckRow(progress.rows()[0], 1, 42, 3'241'000'000, LedgerClaimState::Failed, "", needsGas,
             "failed");
    CheckEq("add_tao_for_gas", ClaimFailureKey(progress.rows()[0].message), "the row's words");
    progress.Done();
    Check(!progress.CanClaim(true), "Claim waits for gas");
    Check(LedgerClaimProgress({Account()}, 0.0).needsGas(), "a known empty gas key");
    Check(!LedgerClaimProgress({Account()}, 0.0).CanClaim(true), "cannot claim");
    Check(!LedgerClaimProgress({Account()}, std::nullopt).needsGas(),
          "an unknown balance does not block");
  }
  {
    TEST_CASE("v2 claim: already_claimed is the account's success");
    LedgerClaimProgress progress({Account()}, 0.5);
    Check(progress.Start(), "Claim starts");
    progress.Failed(1, 42, "already_claimed: NothingToClaim after a third party's claim");
    CheckRow(progress.rows()[0], 1, 42, 3'241'000'000, LedgerClaimState::Confirmed, "", "",
             "claimed");
    Check(!progress.needsGas() && progress.failure().empty(), "no failure");
    Check(progress.ClaimableRao() == 0, "nothing left to claim");
  }
  {
    TEST_CASE("v2 claim: any other failure shows its message, and a retry clears it");
    LedgerClaimProgress progress({Account()}, 0.5);
    Check(progress.Start(), "Claim starts");
    progress.Sent(1, 42, "0xabc");
    progress.Failed(1, 42, "claim_failed: TooManyStakingHotkeys");
    CheckRow(progress.rows()[0], 1, 42, 3'241'000'000, LedgerClaimState::Failed, "0xabc",
             "claim_failed: TooManyStakingHotkeys", "failed");
    CheckEq("", ClaimFailureKey(progress.rows()[0].message), "the message as it came");
    Check(!progress.needsGas(), "no gas hint");
    progress.Done();
    Check(progress.CanClaim(true), "still claimable");
    Check(progress.Start(), "retry");
    CheckRow(progress.rows()[0], 1, 42, 3'241'000'000, LedgerClaimState::Sending, "0xabc", "",
             "sending again");
  }
  {
    TEST_CASE("v2 claim: a failure for no pool is the dialog's");
    LedgerClaimProgress progress({Account()}, 0.5);
    Check(progress.Start(), "Claim starts");
    progress.Failed(0, 0, "ledger_unavailable: the ledger answered 503");
    CheckEq("ledger_unavailable: the ledger answered 503", progress.failure(), "the dialog's line");
    Check(progress.rows().size() == 1, "no row for it");
    CheckRow(progress.rows()[0], 1, 42, 3'241'000'000, LedgerClaimState::Sending, "", "",
             "untouched");
    progress.Done();
    CheckRow(progress.rows()[0], 1, 42, 3'241'000'000, LedgerClaimState::Idle, "", "", "at rest");
    Check(progress.Start() && progress.failure().empty(), "a new claim clears it");
  }
  {
    TEST_CASE("v2 claim: an account the page did not list gets a row");
    LedgerClaimProgress progress({Account()}, 0.5);
    Check(progress.Start(), "Claim starts");
    progress.Sent(2, 7, "0xdef");
    Check(progress.rows().size() == 2, "a second row");
    CheckRow(progress.rows()[1], 2, 7, 0, LedgerClaimState::Sent, "0xdef", "", "sent");
    progress.Confirmed(2, 7, "0xdef", 900'000'000);
    CheckRow(progress.rows()[1], 2, 7, 900'000'000, LedgerClaimState::Confirmed, "0xdef", "",
             "confirmed");
    progress.Failed(3, 1, "proof_mismatch: the root does not match");
    CheckRow(progress.rows()[2], 3, 1, 0, LedgerClaimState::Failed, "",
             "proof_mismatch: the root does not match", "failed");
  }
  {
    TEST_CASE("v2 claim: the failure words");
    CheckEq("needs_gas", ClaimFailureCode("needs_gas: detail"), "code");
    CheckEq("claim_failed", ClaimFailureCode("claim_failed"), "a bare code");
    CheckEq("proof_mismatch", ClaimFailureCode("proof_mismatch : detail"), "trailing space");
    CheckEq("add_tao_for_gas", ClaimFailureKey("needs_gas: detail"), "needs_gas");
    CheckEq("chain_rpc_unreachable", ClaimFailureKey("chain_rpc_unreachable: dial tcp"),
            "rpc unreachable");
    CheckEq("chain_rpc_unreachable", ClaimFailureKey("chain_rpc_error: execution reverted"),
            "rpc error");
    CheckEq("connect_wallet_first", ClaimFailureKey("connect_wallet_first: no coldkey"),
            "no coldkey");
    CheckEq("claim_failed", ClaimFailureKey(""), "no message");
    CheckEq("claim_failed", ClaimFailureKey("claim_failed"), "no detail");
    for (const char* message :
         {"claim_failed: TooManyStakingHotkeys", "not_claimable: below the transfer floor",
          "proof_mismatch: the root does not match", "ledger_unavailable: 503"}) {
      CheckEq("", ClaimFailureKey(message), std::string("as it came: ") + message);
    }
  }
  {
    TEST_CASE("v2 claim: Sent links the transaction on the explorer");
    CheckEq("https://evm.taostats.io/tx/0xabc",
            ExplorerTxLink("https://evm.taostats.io/tx/%s", "0xabc"), "the hash in place of %s");
    CheckEq("https://explorer.example/tx/0xabc",
            ExplorerTxLink("https://explorer.example/tx", "0xabc"), "appended as a path segment");
    CheckEq("https://explorer.example/tx/0xabc",
            ExplorerTxLink("https://explorer.example/tx/", "0xabc"), "after a trailing slash");
    CheckEq("", ExplorerTxLink("", "0xabc"), "no explorer configured: no link");
    CheckEq("", ExplorerTxLink("https://evm.taostats.io/tx/%s", ""), "nothing sent yet: no link");
  }
  {
    TEST_CASE("v2 claim: the top-up asks for what a v2 claim's preflight needs");
    // the SDK's gas limit is eth_estimateGas (about 2.51M for a claim on runtime
    // 475) plus a quarter, priced at 1.1 x the gas price (legacy, the mainnet
    // default); at the plan's 5 gwei:
    const double preflightTao = 2'510'000.0 * 1.25 * 1.1 * 5e9 / 1e18;
    Check(preflightTao <= kLedgerClaimGasTao,
          "covers one claim's " + std::to_string(preflightTao) + " TAO");
    Check(kLedgerClaimGasTao == 0.02, "0.02 TAO, which the v1 dialog asks for too");
  }

  {
    TEST_CASE("v2 at the SDK's boundary: an account's fields");
    SdkLedgerAccount sdk;
    sdk.vault = "0x00000000000000000000000000000000000000aa";
    sdk.no_id = 1;
    sdk.index = 42;
    sdk.finalized_rao = 3'741'000'000;
    sdk.claimable_rao = 3'241'000'000;
    sdk.paid_rao = 500'000'000;
    sdk.pending = true;
    sdk.pending_rao = 3'861'000'000;
    LedgerAccount want = Account();
    want.pending = true;
    want.pendingRao = 3'861'000'000;
    Check(LedgerAccountFrom(sdk) == want, "every field");
#if defined(URNW_SN_PAYOUT_TESTS_SDK)
    // what the device's SnLedgerAccounts answers with, through the header
    const auto result =
        nlohmann::json::parse(R"({"accounts":[{"vault":"0x00000000000000000000000000000000000000aa",
            "no_id":1,"index":42,"finalized_rao":3741000000,"claimable_rao":3241000000,
            "paid_rao":500000000,"pending":true,"pending_rao":3861000000}],
            "coldkey_ss58":"5SAMPLE"})")
            .get<urnet::SnLedgerAccountsResult>();
    Check(result.accounts.has_value() && result.accounts->size() == 1, "the header's list");
    if (result.accounts && result.accounts->size() == 1) {
      Check(LedgerAccountFrom(result.accounts->front()) == want, "the header's account");
    }
    const auto failed =
        nlohmann::json::parse(
            R"({"accounts":[],"error":{"code":"ledger_unavailable","message":"503"}})")
            .get<urnet::SnLedgerAccountsResult>();
    Check(failed.error.has_value() &&
              failed.error->code == std::optional<std::string>("ledger_unavailable"),
          "the header's error code");
#endif
  }
  {
    TEST_CASE("v2 at the SDK's boundary: the claim callback hands each event on");
    std::vector<std::string> heard;
    LedgerClaimEvents events;
    events.sent = [&heard](int64_t noId, int64_t index, std::string txHash) {
      heard.push_back("sent " + std::to_string(noId) + " " + std::to_string(index) + " " + txHash);
    };
    events.confirmed = [&heard](int64_t noId, int64_t index, std::string txHash,
                                int64_t amountRao) {
      heard.push_back("confirmed " + std::to_string(noId) + " " + std::to_string(index) + " " +
                      txHash + " " + std::to_string(amountRao));
    };
    events.failed = [&heard](int64_t noId, int64_t index, std::string message) {
      heard.push_back("failed " + std::to_string(noId) + " " + std::to_string(index) + " " +
                      message);
    };
    events.done = [&heard]() { heard.push_back("done"); };
    const SdkLedgerClaimCallback callback = LedgerClaimCallbackFor<SdkLedgerClaimCallback>(events);
    callback.sent(1, 42, "0xabc");
    callback.confirmed(1, 42, "0xabc", 3'241'000'000);
    callback.failed(0, 0, "not_claimable: nothing to claim");
    callback.done();
    Check(heard == std::vector<std::string>{"sent 1 42 0xabc", "confirmed 1 42 0xabc 3241000000",
                                            "failed 0 0 not_claimable: nothing to claim", "done"},
          "in order, with their values");
    const SdkLedgerClaimCallback quiet = LedgerClaimCallbackFor<SdkLedgerClaimCallback>({});
    quiet.sent(1, 42, "0xabc");
    quiet.confirmed(1, 42, "0xabc", 1);
    quiet.failed(1, 42, "claim_failed");
    quiet.done();
    Check(heard.size() == 4, "events without a listener go nowhere");
  }

  std::cout << gCases << " cases, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
