// Executable spec for the Solana payout wallet's pure logic (the restored
// Solana connect option on Earnings): the address check a pasted address
// passes before it is sent to the server, the short form, which account wallet
// is the Solana payout wallet, the USDC still waiting, what the pane shows from
// those, and the connect sheet's state machine (App/SolanaWalletPresentation.h),
// plus where the wallet bridge's returns go and when a late challenge may still
// open the bridge (App/WalletBridgeRoute.h) - run
// against the SAME sources the app compiles, on any host with a C++20 compiler.
//
// The WinUI halves (WalletPage's card and overflows, SolanaWalletSheets' dialog)
// cannot be built off Windows at all; what is verified here is every decision
// they make before they touch a XAML object.
//
//   c++ -std=c++20 -I ../src/App solana-wallet-tests.cpp \
//       ../src/App/SolanaWalletPresentation.cpp \
//       -o /tmp/solana-wallet-tests && /tmp/solana-wallet-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "SolanaWalletPresentation.h"
#include "WalletBridgeRoute.h"

using namespace urnw::solana;

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

void CheckEq(const std::string& expected, const std::string& actual,
             const std::string& what) {
  if (expected != actual) {
    Fail(what + ": expected \"" + expected + "\", got \"" + actual + "\"");
  }
}

void CheckEq(long long expected, long long actual, const std::string& what) {
  if (expected != actual) {
    Fail(what + ": expected " + std::to_string(expected) + ", got " +
         std::to_string(actual));
  }
}

struct Case {
  explicit Case(const char* name) {
    gCurrentCase = name;
    ++gCases;
  }
};
#define TEST_CASE(name) Case case_##__LINE__(name)

// Real, well-known Solana public keys, so the syntax check runs on the real shape.
constexpr const char* kUsdcMint = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";      // 44
constexpr const char* kTokenProgram = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA";  // 43
constexpr const char* kSystemProgram = "11111111111111111111111111111111";            // 32
// a Bittensor coldkey (ss58, 48 characters): not a Solana address
constexpr const char* kColdkey = "5F3sa2TJAWMqDhXG6jhV4N8ko9SxwGy8TpaNS1repo5EYjQX";

// U+2026 as UTF-8, the SDK's ellipsis
const std::string kEllipsis = "\xE2\x80\xA6";

LegacyWallet Wallet(const std::string& id, const std::string& chain, const std::string& address,
                    bool active = true, const std::string& circle = std::string()) {
  LegacyWallet wallet;
  wallet.id = id;
  wallet.blockchain = chain;
  wallet.address = address;
  wallet.active = active;
  wallet.circleWalletId = circle;
  return wallet;
}

HeldPayment Payment(int64_t nanoCents, bool completed, bool canceled) {
  HeldPayment payment;
  payment.payoutNanoCents = nanoCents;
  payment.completed = completed;
  payment.canceled = canceled;
  return payment;
}

std::string StateName(ConnectState state) {
  switch (state) {
    case ConnectState::Idle: return "Idle";
    case ConnectState::OpeningBrowser: return "OpeningBrowser";
    case ConnectState::Checking: return "Checking";
    case ConnectState::Ready: return "Ready";
    case ConnectState::Linking: return "Linking";
    case ConnectState::Failed: return "Failed";
    case ConnectState::Linked: return "Linked";
  }
  return "?";
}

void CheckState(ConnectState expected, const ConnectMachine& m, const std::string& what) {
  CheckEq(StateName(expected), StateName(m.state), what);
}

// A machine whose manual address the server has accepted.
ConnectMachine ReadyMachine() {
  ConnectMachine m;
  Debounced(m, kUsdcMint, /*askServer=*/true);
  Verdict(m, ServerVerdict::Valid);
  return m;
}

// ---- addresses --------------------------------------------------------------

void AddressTests() {
  {
    TEST_CASE("theFixturesAreTheLengthsTheySayTheyAre");
    CheckEq(44, static_cast<long long>(std::string(kUsdcMint).size()), "the USDC mint");
    CheckEq(43, static_cast<long long>(std::string(kTokenProgram).size()), "the token program");
    CheckEq(32, static_cast<long long>(std::string(kSystemProgram).size()), "the system program");
  }
  {
    TEST_CASE("realPublicKeysLookLikeSolanaAddresses");
    Check(LooksLikeSolanaAddress(kUsdcMint), "44 characters");
    Check(LooksLikeSolanaAddress(kTokenProgram), "43 characters");
    Check(LooksLikeSolanaAddress(kSystemProgram), "32 characters");
  }
  {
    TEST_CASE("theLengthBoundsAreInclusive");
    Check(!LooksLikeSolanaAddress(std::string(31, '1')), "31 is too short");
    Check(LooksLikeSolanaAddress(std::string(32, '1')), "32 is the shortest");
    Check(LooksLikeSolanaAddress(std::string(44, 'z')), "44 is the longest");
    Check(!LooksLikeSolanaAddress(std::string(45, 'z')), "45 is too long");
  }
  {
    TEST_CASE("theFourLookalikesAreNotBase58");
    for (char lookalike : std::string("0OIl")) {
      std::string address = kUsdcMint;
      address[5] = lookalike;
      Check(!LooksLikeSolanaAddress(address),
            std::string("rejects a '") + lookalike + "' inside a real key");
    }
    for (char other : std::string("+/-_=.")) {
      std::string address = kUsdcMint;
      address[5] = other;
      Check(!LooksLikeSolanaAddress(address),
            std::string("rejects a '") + other + "' (base64 and punctuation)");
    }
  }
  {
    TEST_CASE("surroundingWhitespaceIsTrimmedButInnerIsNot");
    Check(LooksLikeSolanaAddress(std::string("  ") + kUsdcMint + "\n"), "a padded paste passes");
    Check(LooksLikeSolanaAddress(std::string("\t") + kSystemProgram + " \r\n"),
          "trimmed before the length is counted");
    std::string spaced = kUsdcMint;
    spaced[20] = ' ';
    Check(!LooksLikeSolanaAddress(spaced), "a space inside is not trimmed away");
    Check(!LooksLikeSolanaAddress(""), "empty");
    Check(!LooksLikeSolanaAddress("   \t "), "whitespace only");
    CheckEq("abc", Trim(" \t abc \r\n"), "Trim");
    CheckEq("", Trim("  "), "Trim of whitespace");
  }
  {
    TEST_CASE("otherChainsAddressesAreNotSolanaOnes");
    Check(!LooksLikeSolanaAddress(kColdkey), "a Bittensor coldkey (48 characters)");
    Check(!LooksLikeSolanaAddress("0x71C7656EC7ab88b098defB751B7401B5f6d8976F"),
          "a Polygon address (0x hex)");
  }
}

void ShortAddressTests() {
  // What the card draws: SolanaPanelView::shortAddress is this function (see
  // theCardDrawsItsShortForm), the SDK's ShortSs58 shape.
  {
    TEST_CASE("theCardsShortForm");
    CheckEq(std::string("EPjF") + kEllipsis + "Dt1v", ShortAddress(kUsdcMint), "the USDC mint");
    CheckEq(std::string("7Xk9") + kEllipsis + "3fQa",
            ShortAddress("7Xk9SAMPLEpayoutWALLETnotREAL1111113fQa"), "the preview card's sample");
    CheckEq(std::string("1111") + kEllipsis + "1111", ShortAddress(kSystemProgram),
            "the system program");
  }
  {
    TEST_CASE("twelveBytesOrFewerStayWhole");
    CheckEq("123456789012", ShortAddress("123456789012"), "12 stays whole");
    CheckEq(std::string("1234") + kEllipsis + "0123", ShortAddress("1234567890123"),
            "13 is shortened");
    CheckEq("abc", ShortAddress("abc"), "short");
    CheckEq("", ShortAddress(""), "empty");
  }
  {
    TEST_CASE("trimmedBeforeItIsShortened");
    CheckEq(std::string("EPjF") + kEllipsis + "Dt1v",
            ShortAddress(std::string("  ") + kUsdcMint + "\n"), "padding is not part of it");
  }
}

// ---- the payout wallet ------------------------------------------------------

void PayoutWalletTests() {
  {
    TEST_CASE("thePayoutIdPicksTheWallet");
    const std::vector<LegacyWallet> wallets = {
        Wallet("w1", "SOL", kTokenProgram),
        Wallet("w2", "SOL", kUsdcMint),
    };
    const auto payout = PayoutWalletFor(wallets, "w2");
    Check(payout.has_value(), "found");
    if (payout) {
      CheckEq("w2", payout->id, "the payout wallet's id");
      CheckEq(kUsdcMint, payout->address, "and its address");
    }
  }
  {
    TEST_CASE("anEmptyOrUnknownPayoutIdIsNone");
    const std::vector<LegacyWallet> wallets = {Wallet("w1", "SOL", kUsdcMint)};
    Check(!PayoutWalletFor(wallets, "").has_value(), "no payout wallet id");
    Check(!PayoutWalletFor(wallets, "w9").has_value(), "an id no wallet has");
    Check(!PayoutWalletFor({}, "w1").has_value(), "no wallets");
    // an empty wallet id never matches an empty payout id
    Check(!PayoutWalletFor({Wallet("", "SOL", kUsdcMint)}, "").has_value(),
          "an id-less wallet is never the payout wallet");
  }
  {
    TEST_CASE("custodialBittensorAndInactiveWalletsAreDropped");
    const std::vector<LegacyWallet> wallets = {
        Wallet("circle", "SOL", kUsdcMint, true, "circle-wallet-1"),
        Wallet("tao", "TAO", kColdkey),
        Wallet("gone", "SOL", kTokenProgram, /*active=*/false),
    };
    Check(!PayoutWalletFor(wallets, "circle").has_value(), "a Circle custodial wallet");
    Check(!PayoutWalletFor(wallets, "tao").has_value(), "an id pointing at a TAO wallet");
    Check(!PayoutWalletFor(wallets, "gone").has_value(), "an inactive (removed) wallet");
  }
  {
    TEST_CASE("aPolygonPayoutWalletStillCounts");
    const std::vector<LegacyWallet> wallets = {
        Wallet("matic", "MATIC", "0x71C7656EC7ab88b098defB751B7401B5f6d8976F")};
    const auto payout = PayoutWalletFor(wallets, "matic");
    Check(payout.has_value(), "a legacy Polygon user's USDC goes there");
  }
}

void PendingTests() {
  {
    TEST_CASE("onlyHeldPaymentsCount");
    const std::vector<HeldPayment> payments = {
        Payment(3'000'000'000, false, false),   // held
        Payment(870'000'000, false, false),     // held
        Payment(9'000'000'000, true, false),    // paid
        Payment(5'000'000'000, false, true),    // canceled
        Payment(7'000'000'000, true, true),     // both
    };
    CheckEq(3'870'000'000LL, PendingUsdcNanoCents(payments), "3.87 USDC held");
    CheckEq(0, PendingUsdcNanoCents({}), "nothing");
  }
  {
    TEST_CASE("twoDecimalsInIntegerArithmetic");
    CheckEq("3.87", FormatUsd(3'870'000'000), "the email's 3.87");
    CheckEq("0.00", FormatUsd(0), "zero");
    CheckEq("0.00", FormatUsd(4'999'999), "just under half a cent rounds down");
    CheckEq("0.01", FormatUsd(5'000'000), "half a cent rounds up");
    CheckEq("1234.57", FormatUsd(1'234'567'890'123), "rounded to the cent");
    CheckEq("100.00", FormatUsd(100'000'000'000), "whole dollars keep two places");
    CheckEq("-3.87", FormatUsd(-3'870'000'000), "negative");
    CheckEq("0.00", FormatUsd(-4'999'999), "a negative that rounds to zero has no sign");
  }
  {
    TEST_CASE("subCentTotalsAreNotWaiting");
    Check(!HasPendingUsd(0), "zero");
    Check(!HasPendingUsd(1), "one nano cent reads 0.00");
    Check(!HasPendingUsd(4'999'999), "just under half a cent");
    Check(HasPendingUsd(5'000'000), "0.01");
    Check(!HasPendingUsd(-3'870'000'000), "a negative total");
  }
}

void PanelTests() {
  const LegacyWallet wallet = Wallet("w1", "SOL", kUsdcMint);
  const LegacyReads all{true, true, true};
  {
    TEST_CASE("loadingShowsNothing");
    const auto view = SolanaPanelFor(LegacyState::Loading, all, wallet, 3'870'000'000);
    Check(!view.showCard && !view.showCardPending && !view.showWaitingLine,
          "no card and no line until all three reads are in");
  }
  {
    TEST_CASE("aPayoutWalletShowsTheCard");
    const auto view = SolanaPanelFor(LegacyState::Ready, all, wallet, 3'870'000'000);
    Check(view.showCard, "the card");
    Check(view.wallet == wallet, "for the payout wallet");
    Check(view.showCardPending, "with its waiting line");
    Check(!view.showWaitingLine, "and no line above the Bittensor action");
    CheckEq("3.87", view.pendingUsd, "the figure");
  }
  {
    TEST_CASE("theCardDrawsItsShortForm");
    const auto view = SolanaPanelFor(LegacyState::Ready, all, wallet, 0);
    CheckEq(std::string("EPjF") + kEllipsis + "Dt1v", view.shortAddress, "the card's address line");
    const auto sample = SolanaPanelFor(
        LegacyState::Ready, all, Wallet("s", "SOL", "7Xk9SAMPLEpayoutWALLETnotREAL1111113fQa"), 0);
    CheckEq(std::string("7Xk9") + kEllipsis + "3fQa", sample.shortAddress, "the preview card's");
    const auto none = SolanaPanelFor(LegacyState::Ready, all, std::nullopt, 0);
    CheckEq("", none.shortAddress, "no card, no address");
  }
  {
    TEST_CASE("aPolygonPayoutWalletIsNotCalledASolanaOne");
    const auto polygon = SolanaPanelFor(
        LegacyState::Ready, all, Wallet("matic", "MATIC", "0x71C7656EC7ab88b098defB751B7401B5f6d8976F"),
        3'870'000'000);
    Check(polygon.showCard, "its card still shows: hiding it would invite replacing a working wallet");
    Check(!polygon.solana, "titled Wallet, not Solana wallet");
    Check(SolanaPanelFor(LegacyState::Ready, all, wallet, 0).solana,
          "a Solana payout wallet is titled Solana wallet");
  }
  {
    TEST_CASE("theCardHidesItsLineAtZero");
    const auto view = SolanaPanelFor(LegacyState::Ready, all, wallet, 0);
    Check(view.showCard, "the card");
    Check(!view.showCardPending, "no 0.00 USDC waiting");
    Check(!view.showWaitingLine, "and no line elsewhere");
  }
  {
    TEST_CASE("noPayoutWalletShowsTheWaitingLine");
    const auto view = SolanaPanelFor(LegacyState::Ready, all, std::nullopt, 3'870'000'000);
    Check(!view.showCard, "no card");
    Check(view.showWaitingLine, "the emailed user's line");
    CheckEq("3.87", view.pendingUsd, "the figure");
  }
  {
    TEST_CASE("noPayoutWalletAndNothingWaitingShowsNothing");
    const auto none = SolanaPanelFor(LegacyState::Ready, all, std::nullopt, 0);
    Check(!none.showCard && !none.showWaitingLine, "zero");
    const auto dust = SolanaPanelFor(LegacyState::Ready, all, std::nullopt, 1);
    Check(!dust.showWaitingLine, "a sub-cent remainder");
  }
  {
    TEST_CASE("aFailedPaymentsReadKeepsTheCardWithoutItsFigure");
    const auto view = SolanaPanelFor(LegacyState::Ready, {true, true, false}, wallet, 3'870'000'000);
    Check(view.showCard, "the card");
    Check(!view.showCardPending, "without its figure");
    Check(!view.showWaitingLine, "and no line");
  }
  {
    TEST_CASE("aFailedPayoutReadKeepsTheKnownWallet");
    const auto view = SolanaPanelFor(LegacyState::Ready, {true, false, true}, wallet, 3'870'000'000);
    Check(view.showCard, "the card for the last known payout wallet");
    Check(view.showCardPending, "with its figure");
  }
  {
    TEST_CASE("aFailedPayoutReadWithNoKnownWalletClaimsNothing");
    const auto view =
        SolanaPanelFor(LegacyState::Ready, {true, false, true}, std::nullopt, 3'870'000'000);
    Check(!view.showCard, "no card");
    Check(!view.showWaitingLine, "and no line saying there is no payout wallet");
  }
  {
    TEST_CASE("aFailedWalletsReadShowsNothing");
    const auto view = SolanaPanelFor(LegacyState::Ready, {false, true, true}, wallet, 3'870'000'000);
    Check(!view.showCard && !view.showCardPending && !view.showWaitingLine, "nothing");
  }
  {
    TEST_CASE("aSwitchIsNeededUnlessItIsAlreadyThePayoutWallet");
    Check(!NeedsPayoutSwitch("", "w1"), "no wallet id: nothing to switch to");
    Check(!NeedsPayoutSwitch("w1", "w1"), "already the payout wallet");
    Check(NeedsPayoutSwitch("w2", "w1"), "a second wallet");
    Check(NeedsPayoutSwitch("w2", ""), "no payout wallet known");
  }
  {
    TEST_CASE("theSwitchIsDecidedOnAFreshRead");
    // the card still shows w1, but the payout wallet moved to w2 on the web
    Check(NeedsPayoutSwitch("w1", PayoutIdForSwitch(true, "w2")),
          "linking w1 again switches back to it");
    Check(!NeedsPayoutSwitch("w1", PayoutIdForSwitch(true, "w1")),
          "no switch when the server already adopted it");
    Check(NeedsPayoutSwitch("w1", PayoutIdForSwitch(true, "")), "no payout wallet: switch");
    CheckEq("", PayoutIdForSwitch(false, "w1"), "a failed read counts as none");
    Check(NeedsPayoutSwitch("w1", PayoutIdForSwitch(false, "w1")),
          "so a failed read switches (idempotent)");
  }
}

// ---- the three reads, committed (LegacyLoad) ------------------------------

LegacyAnswer WalletsAnswer(std::vector<LegacyWallet> wallets) {
  LegacyAnswer answer;
  answer.wallets = std::move(wallets);
  return answer;
}

LegacyAnswer PayoutAnswer(const std::string& payoutId) {
  LegacyAnswer answer;
  answer.payoutId = payoutId;
  return answer;
}

LegacyAnswer PaymentsAnswer(std::vector<HeldPayment> payments) {
  LegacyAnswer answer;
  answer.payments = std::move(payments);
  return answer;
}

// A view of `networkId` committed from one load of three ok reads.
LegacyCommitted CommittedView(const std::string& networkId, const LegacyWallet& wallet,
                              int64_t pendingNanoCents) {
  LegacyCommitted view;
  BeginLegacyLoad(view, networkId, /*reset=*/false);
  LegacyLoad load(1);
  load.Answer(1, LegacyRead::Wallets, true, WalletsAnswer({wallet}));
  load.Answer(1, LegacyRead::Payout, true, PayoutAnswer(wallet.id));
  load.Answer(1, LegacyRead::Payments, true,
              PaymentsAnswer({Payment(pendingNanoCents, false, false)}));
  load.Commit(view, networkId);
  return view;
}

void LegacyLoadTests() {
  const LegacyWallet w1 = Wallet("w1", "SOL", kUsdcMint);
  const LegacyWallet w2 = Wallet("w2", "SOL", kTokenProgram);
  {
    TEST_CASE("nothingCommitsBeforeThreeAnswers");
    LegacyCommitted view;
    LegacyLoad load(1);
    Check(load.Answer(1, LegacyRead::Wallets, true, WalletsAnswer({w1})), "the wallets");
    Check(load.Answer(1, LegacyRead::Payout, true, PayoutAnswer("w1")), "the payout wallet id");
    Check(!load.Complete(), "two of three");
    Check(!load.Commit(view, "net-a"), "no commit");
    Check(!view.ready && view.wallets.empty() && view.payoutWalletId.empty(), "the view untouched");
    Check(load.Answer(1, LegacyRead::Payments, true,
                      PaymentsAnswer({Payment(3'870'000'000, false, false)})),
          "the payments");
    Check(load.Complete(), "all three");
    Check(load.Commit(view, "net-a"), "commits");
    Check(view.ready, "ready");
    CheckEq("w1", view.payoutWalletId, "the payout wallet id");
    CheckEq(3'870'000'000LL, view.pendingNanoCents, "the total");
    const auto panel = SolanaPanelFor(view);
    Check(panel.showCard && panel.showCardPending, "the card with its figure");
  }
  {
    TEST_CASE("anAnswerForAnOldLoadIsDropped");
    LegacyLoad load(2);
    Check(!load.Answer(1, LegacyRead::Wallets, true, WalletsAnswer({w1})), "the older load's answer");
    Check(load.Answer(2, LegacyRead::Wallets, true, WalletsAnswer({w2})), "this load's");
    Check(!load.Answer(2, LegacyRead::Wallets, true, WalletsAnswer({w1})), "a read answers once");
    load.Answer(2, LegacyRead::Payout, true, PayoutAnswer("w2"));
    load.Answer(2, LegacyRead::Payments, true);
    LegacyCommitted view;
    Check(load.Commit(view, "net-a"), "commits");
    CheckEq(1, static_cast<long long>(view.wallets.size()), "one wallet");
    if (!view.wallets.empty()) CheckEq("w2", view.wallets[0].id, "this load's, not the older one's");
  }
  {
    TEST_CASE("anEmptyPayoutIdKeepsTheKnownOne");
    LegacyCommitted view = CommittedView("net-a", w1, 0);
    LegacyLoad load(2);
    load.Answer(2, LegacyRead::Wallets, true, WalletsAnswer({w1}));
    load.Answer(2, LegacyRead::Payout, true, PayoutAnswer(""));
    load.Answer(2, LegacyRead::Payments, true);
    load.Commit(view, "net-a");
    CheckEq("w1", view.payoutWalletId, "a transient nil keeps the payout wallet");
    Check(SolanaPanelFor(view).showCard, "and its card");
  }
  {
    TEST_CASE("aFailedReadCommitsNothingOfItsOwn");
    LegacyCommitted view = CommittedView("net-a", w1, 3'870'000'000);
    LegacyLoad load(2);
    load.Answer(2, LegacyRead::Wallets, true, WalletsAnswer({w1}));
    load.Answer(2, LegacyRead::Payout, false, PayoutAnswer("w9"));
    load.Answer(2, LegacyRead::Payments, false,
                PaymentsAnswer({Payment(9'000'000'000, false, false)}));
    load.Commit(view, "net-a");
    CheckEq("w1", view.payoutWalletId, "the last known id stands");
    CheckEq(3'870'000'000LL, view.pendingNanoCents, "a failed read's payload is ignored");
    Check(view.reads.wallets && !view.reads.payout && !view.reads.payments, "which reads failed");
    const auto panel = SolanaPanelFor(view);
    Check(panel.showCard, "the card for the known wallet");
    Check(!panel.showCardPending, "without its figure");
  }
  {
    TEST_CASE("anotherNetworkClearsTheView");
    LegacyCommitted view = CommittedView("net-a", w1, 3'870'000'000);
    BeginLegacyLoad(view, "net-b", /*reset=*/false);
    Check(!view.ready, "hidden at once, even on a plain reload");
    Check(view.wallets.empty() && view.payoutWalletId.empty() && view.pendingNanoCents == 0,
          "net-a's wallets, id and total are gone");
    CheckEq("net-b", view.networkId, "the view is net-b's");
    LegacyLoad load(2);
    load.Answer(2, LegacyRead::Wallets, true, WalletsAnswer({w1}));
    load.Answer(2, LegacyRead::Payout, true, PayoutAnswer(""));
    load.Answer(2, LegacyRead::Payments, true);
    load.Commit(view, "net-b");
    CheckEq("", view.payoutWalletId, "net-a's id does not survive through the empty-id rule");
    Check(!SolanaPanelFor(view).showCard, "no card");

    LegacyCommitted late = CommittedView("net-a", w1, 0);
    LegacyLoad other(3);
    other.Answer(3, LegacyRead::Wallets, true, WalletsAnswer({w1}));
    other.Answer(3, LegacyRead::Payout, true, PayoutAnswer(""));
    other.Answer(3, LegacyRead::Payments, true);
    other.Commit(late, "net-b");
    CheckEq("", late.payoutWalletId, "a commit for another network clears the view first");
    CheckEq("net-b", late.networkId, "and takes it over");
  }
  {
    TEST_CASE("aPlainReloadKeepsTheViewAndAWriteHidesIt");
    LegacyCommitted view = CommittedView("net-a", w1, 0);
    BeginLegacyLoad(view, "net-a", /*reset=*/false);
    Check(view.ready && SolanaPanelFor(view).showCard, "a plain reload keeps the card on screen");
    BeginLegacyLoad(view, "net-a", /*reset=*/true);
    Check(!view.ready && !SolanaPanelFor(view).showCard, "a write hides it until the reads commit");
    CheckEq("w1", view.payoutWalletId, "without forgetting the known id");
  }
}

// ---- the connect sheet ------------------------------------------------------

void BridgeTests() {
  {
    TEST_CASE("aProviderOpensTheBrowser");
    ConnectMachine m;
    CheckState(ConnectState::Idle, m, "starts idle");
    Check(ProvidersEnabled(m) && EntryEnabled(m) && !ConnectEnabled(m), "idle controls");
    CheckEq("", StatusKey(m), "no status");
    Check(ChooseProvider(m), "accepted");
    CheckState(ConnectState::OpeningBrowser, m, "opening the browser");
    CheckEq("opening_wallet_in_browser", StatusKey(m), "says so");
    Check(!ProvidersEnabled(m), "providers disabled");
    Check(!ConnectEnabled(m), "Connect disabled");
    Check(EntryEnabled(m), "the entry stays usable");
    Check(!ChooseProvider(m), "a second round trip is refused");
  }
  {
    TEST_CASE("thePublicKeyGoesStraightToLinking");
    ConnectMachine m;
    ChooseProvider(m);
    Check(PublicKey(m), "accepted");
    CheckState(ConnectState::Linking, m, "linking, with no client-side validation");
    CheckEq("connecting_to_wallet", StatusKey(m), "says so");
    Check(!ProvidersEnabled(m) && !ConnectEnabled(m) && !EntryEnabled(m),
          "providers, Connect and the entry disabled");
  }
  {
    TEST_CASE("aBridgeErrorFailsWithItsDetail");
    ConnectMachine m;
    ChooseProvider(m);
    Check(BridgeError(m, "User rejected the request."), "accepted");
    CheckState(ConnectState::Failed, m, "failed");
    CheckEq("User rejected the request.", m.detail, "the bridge's words");
    CheckEq("error_connecting_wallet_with_reason", FailureKey(m.detail),
            "rendered with its reason");
    Check(ProvidersEnabled(m) && EntryEnabled(m), "controls re-enabled");
    CheckEq("", StatusKey(m), "no status line");
  }
  {
    TEST_CASE("theBridgeWatchdogFailsAsATimeout");
    ConnectMachine m;
    ChooseProvider(m);
    Check(Timeout(m), "accepted");
    CheckState(ConnectState::Failed, m, "failed");
    CheckEq("", m.detail, "a watchdog has no detail to give");
    CheckEq("something_went_wrong", FailureKey(m.detail), "the rule's text without a detail");
    Check(!PublicKey(m), "a key arriving after the give-up is refused");
  }
  {
    TEST_CASE("lateAnswersInIdleAreIgnored");
    ConnectMachine m;
    Check(!PublicKey(m), "a late public key");
    Check(!BridgeError(m, "late"), "a late bridge error");
    Check(!Timeout(m), "a late watchdog");
    Check(!CreateResult(m, true, "w1", ""), "a late create result");
    CheckState(ConnectState::Idle, m, "still idle");
    CheckEq("", m.detail, "no detail picked up");
  }
  {
    TEST_CASE("anyNewAttemptClearsAFailure");
    ConnectMachine m;
    ChooseProvider(m);
    BridgeError(m, "User rejected the request.");
    Check(ChooseProvider(m), "a provider again");
    CheckState(ConnectState::OpeningBrowser, m, "opening again");
    CheckEq("", m.detail, "the failure is gone");
  }
}

void ManualTests() {
  {
    TEST_CASE("aLocallyInvalidAddressIsAnsweredWithoutARequest");
    ConnectMachine m;
    Check(!Debounced(m, "0OIl0OIl0OIl0OIl0OIl0OIl0OIl0OIl", true), "no request");
    CheckEq("invalid_solana_address", CheckKey(m), "invalid at once");
    Check(CheckIsError(m), "as a refusal");
    CheckState(ConnectState::Idle, m, "idle");
    Check(!ConnectEnabled(m), "Connect disabled");
  }
  {
    TEST_CASE("aPlausibleAddressIsCheckedByTheServer");
    ConnectMachine m;
    Check(Debounced(m, std::string(" ") + kUsdcMint + " ", true), "a request");
    CheckState(ConnectState::Checking, m, "checking");
    CheckEq(kUsdcMint, m.address, "the trimmed address is the one asked about");
    CheckEq("checking_wallet_address", CheckKey(m), "says so");
    Check(!CheckIsError(m), "as progress");
    Check(!ConnectEnabled(m), "Connect disabled while checking");
    Check(ProvidersEnabled(m), "the providers stay usable");
  }
  {
    TEST_CASE("aValidVerdictReadiesConnect");
    ConnectMachine m;
    Debounced(m, kUsdcMint, true);
    Check(Verdict(m, ServerVerdict::Valid), "accepted");
    CheckState(ConnectState::Ready, m, "ready");
    CheckEq("", CheckKey(m), "the line clears");
    Check(ConnectEnabled(m), "Connect enabled");
  }
  {
    TEST_CASE("anInvalidVerdictSaysSo");
    ConnectMachine m;
    Debounced(m, kUsdcMint, true);  // the USDC mint itself: the server refuses it
    Check(Verdict(m, ServerVerdict::Invalid), "accepted");
    CheckState(ConnectState::Idle, m, "idle");
    CheckEq("invalid_solana_address", CheckKey(m), "invalid");
    Check(CheckIsError(m), "as a refusal");
    Check(!ConnectEnabled(m), "Connect disabled");
  }
  {
    TEST_CASE("anUnansweredCheckSaysSomethingWentWrong");
    ConnectMachine m;
    Debounced(m, kUsdcMint, true);
    Check(Verdict(m, ServerVerdict::Unavailable), "accepted");
    CheckState(ConnectState::Idle, m, "idle");
    CheckEq("something_went_wrong", CheckKey(m), "the transport failure's key");
    Check(CheckIsError(m), "as a refusal");
    Check(!ConnectEnabled(m), "Connect disabled");
  }
  {
    TEST_CASE("typingDropsTheVerdict");
    ConnectMachine m = ReadyMachine();
    Check(Typed(m), "accepted");
    CheckState(ConnectState::Idle, m, "back to idle");
    CheckEq("", CheckKey(m), "no verdict");
    CheckEq("", m.address, "and no address it was about");
    Check(!ConnectEnabled(m), "Connect disabled until the new text is checked");
    ConnectMachine checking;
    Debounced(checking, kUsdcMint, true);
    Typed(checking);
    Check(!Verdict(checking, ServerVerdict::Valid), "the old text's verdict is refused");
    Check(!ConnectEnabled(checking), "and enables nothing");
  }
  {
    TEST_CASE("withoutASessionTheCheckIsLocalOnly");
    ConnectMachine m;
    Check(!Debounced(m, kUsdcMint, /*askServer=*/false), "no request");
    CheckEq("", CheckKey(m), "no verdict for a plausible address");
    CheckState(ConnectState::Idle, m, "idle");
    Check(!ConnectEnabled(m), "Connect disabled");
    Check(!Debounced(m, "not an address", false), "no request");
    CheckEq("invalid_solana_address", CheckKey(m), "a local failure still shows");
  }
  {
    TEST_CASE("anEmptyFieldClearsTheVerdict");
    ConnectMachine m;
    Debounced(m, "bad", true);
    Check(!Debounced(m, "   ", true), "no request");
    CheckEq("", CheckKey(m), "no verdict");
    CheckState(ConnectState::Idle, m, "idle");
  }
  {
    TEST_CASE("connectLinksTheCheckedAddress");
    ConnectMachine unchecked;
    Check(!Submit(unchecked), "Connect without a checked address is refused");
    ConnectMachine m = ReadyMachine();
    Check(Submit(m), "accepted");
    CheckState(ConnectState::Linking, m, "linking");
    CheckEq(kUsdcMint, m.address, "the checked address");
    CheckEq("connecting_to_wallet", StatusKey(m), "says so");
    Check(!ProvidersEnabled(m) && !ConnectEnabled(m) && !EntryEnabled(m), "all disabled");
    Check(!Submit(m), "a second press is refused");
    Check(!Typed(m), "typing while linking changes nothing");
    Check(!Debounced(m, kTokenProgram, true), "nor does a late debounce");
    CheckEq(kUsdcMint, m.address, "the address being linked stays");
  }
  {
    TEST_CASE("typingWhileTheBrowserIsOutKeepsThatFlow");
    ConnectMachine m;
    ChooseProvider(m);
    Check(Debounced(m, kUsdcMint, true), "the address is still checked");
    CheckState(ConnectState::OpeningBrowser, m, "the browser round trip goes on");
    Verdict(m, ServerVerdict::Valid);
    CheckState(ConnectState::OpeningBrowser, m, "still");
    Check(!ConnectEnabled(m), "Connect waits for the round trip");
    BridgeError(m, "closed");
    Check(ConnectEnabled(m), "a failed round trip keeps the accepted address");
    Check(Submit(m), "which can still be linked");
    Check(m.detail.empty(), "the failure cleared");
  }
  {
    TEST_CASE("aNewAddressClearsAFailure");
    ConnectMachine m;
    ChooseProvider(m);
    BridgeError(m, "closed");
    Check(Debounced(m, kTokenProgram, true), "checked");
    CheckState(ConnectState::Checking, m, "checking");
    CheckEq("", m.detail, "the failure is gone");
    ConnectMachine local;
    ChooseProvider(local);
    BridgeError(local, "closed");
    Debounced(local, "nope", true);
    CheckState(ConnectState::Idle, local, "a locally invalid address clears it too");
    CheckEq("", local.detail, "no detail");
  }
}

void LinkTests() {
  {
    TEST_CASE("aWalletIdLinks");
    ConnectMachine m = ReadyMachine();
    Submit(m);
    Check(CreateResult(m, true, "w1", ""), "accepted");
    CheckState(ConnectState::Linked, m, "linked");
    Check(!ProvidersEnabled(m) && !ConnectEnabled(m) && !EntryEnabled(m), "all disabled");
    Check(!ChooseProvider(m) && !Submit(m) && !Timeout(m), "linked is final");
  }
  {
    TEST_CASE("anEmptyWalletIdIsAFailure");
    ConnectMachine m = ReadyMachine();
    Submit(m);
    Check(CreateResult(m, true, "", ""), "accepted");
    CheckState(ConnectState::Failed, m, "failed");
    CheckEq("something_went_wrong", FailureKey(m.detail), "a detail-less failure");
  }
  {
    TEST_CASE("aCreateErrorFailsWithTheServerDetail");
    ConnectMachine m;
    ChooseProvider(m);
    PublicKey(m);
    Check(CreateResult(m, false, "", "invalid wallet address"), "accepted");
    CheckState(ConnectState::Failed, m, "failed");
    CheckEq("invalid wallet address", m.detail, "the server's words");
    CheckEq("error_connecting_wallet_with_reason", FailureKey(m.detail),
            "rendered with its reason");
    Check(ProvidersEnabled(m) && EntryEnabled(m), "controls re-enabled");
  }
  {
    TEST_CASE("theCreateWatchdogFailsAsATimeout");
    ConnectMachine m = ReadyMachine();
    Submit(m);
    Check(Timeout(m), "accepted");
    CheckEq("something_went_wrong", FailureKey(m.detail), "the rule's text without a detail");
    Check(!CreateResult(m, true, "w1", ""), "the answer after giving up is dropped");
    CheckState(ConnectState::Failed, m, "still failed");
    Check(ConnectEnabled(m), "the checked address can be tried again");
  }
  {
    TEST_CASE("aFailureWithoutADetailSaysSomethingWentWrong");
    CheckEq("something_went_wrong", FailureKey(""), "no detail, as after a watchdog");
  }
  {
    TEST_CASE("aFailureWithADetailCarriesIt");
    CheckEq("error_connecting_wallet_with_reason", FailureKey("reason"), "with a detail");
  }
}

// ---- where a wallet-bridge return goes (WalletBridgeRoute.h) -----------------

std::string RouteName(urnw::bridge::PublicKeyRoute route) {
  switch (route) {
    case urnw::bridge::PublicKeyRoute::Drop: return "Drop";
    case urnw::bridge::PublicKeyRoute::AnswerConnect: return "AnswerConnect";
    case urnw::bridge::PublicKeyRoute::SignForRequest: return "SignForRequest";
    case urnw::bridge::PublicKeyRoute::SignIn: return "SignIn";
  }
  return "?";
}

std::string RouteName(urnw::bridge::SignatureRoute route) {
  switch (route) {
    case urnw::bridge::SignatureRoute::Drop: return "Drop";
    case urnw::bridge::SignatureRoute::AnswerRequest: return "AnswerRequest";
    case urnw::bridge::SignatureRoute::SignIn: return "SignIn";
  }
  return "?";
}

void BridgeRouteTests() {
  using urnw::bridge::IsSuperseded;
  using urnw::bridge::RoutePublicKey;
  using urnw::bridge::RouteSignature;
  {
    TEST_CASE("aConnectReturnNobodyWaitsForIsDropped");
    // the bridge page's "Return to URnetwork" after the sheet already linked
    CheckEq("Drop", RouteName(RoutePublicKey(false, false, false, false)), "no flow in flight");
  }
  {
    TEST_CASE("theBareConnectIsAnsweredFirst");
    CheckEq("AnswerConnect", RouteName(RoutePublicKey(false, true, true, true)),
            "a waiting connect takes the key before anything else");
  }
  {
    TEST_CASE("aSignatureRequestSignsBeforeASignIn");
    CheckEq("SignForRequest", RouteName(RoutePublicKey(false, false, true, true)),
            "the Seeker request's own message is signed");
  }
  {
    TEST_CASE("aWalletSignInStillSignsIn");
    CheckEq("SignIn", RouteName(RoutePublicKey(false, false, false, true)),
            "the login page's Solana sign-in fetches its challenge");
  }
  {
    TEST_CASE("aBittensorConnectReturnChainsNothing");
    CheckEq("Drop", RouteName(RoutePublicKey(true, true, true, true)),
            "Bittensor has no connect hop, whatever is waiting");
  }
  {
    TEST_CASE("aSignatureNobodyWaitsForNeverSignsIn");
    CheckEq("Drop", RouteName(RouteSignature(false, false)),
            "a superseded tab's signature is dropped");
    CheckEq("AnswerRequest", RouteName(RouteSignature(true, true)),
            "a waiting request is answered before a sign-in");
    CheckEq("SignIn", RouteName(RouteSignature(false, true)), "a wallet sign-in logs in");
  }
  {
    TEST_CASE("everyPublicKeyCombinationRoutesByPriority");
    for (int bits = 0; bits < 16; ++bits) {
      const bool bittensor = (bits & 1) != 0;
      const bool connect = (bits & 2) != 0;
      const bool sign = (bits & 4) != 0;
      const bool signIn = (bits & 8) != 0;
      const std::string expected = bittensor ? "Drop"
                                   : connect ? "AnswerConnect"
                                   : sign    ? "SignForRequest"
                                   : signIn  ? "SignIn"
                                             : "Drop";
      CheckEq(expected, RouteName(RoutePublicKey(bittensor, connect, sign, signIn)),
              "combination " + std::to_string(bits));
    }
  }
  {
    TEST_CASE("supersededIsTheHostsOwnWord");
    Check(IsSuperseded("superseded by a wallet connect request"), "the connect request's reason");
    Check(!IsSuperseded("User rejected the request."), "the bridge's own error is not");
    Check(!IsSuperseded(""), "an empty error is not");
    // every reason SdkHost::CancelPendingWalletFlows passes today
    for (const char* reason :
         {"superseded by a wallet sign-in", "superseded by a wallet signature request",
          "superseded by wallet network creation", "superseded by a sign-in"}) {
      Check(IsSuperseded(reason), std::string("\"") + reason + "\"");
    }
    Check(!IsSuperseded("Superseded by a sign-in"), "the prefix is exact");
  }
  {
    TEST_CASE("aSupersededFlowsChallengeNeverOpensTheBridge");
    urnw::bridge::FlowSerial flows;
    // the page's Bittensor connect starts and fetches its challenge
    const uint64_t bittensor = flows.Start();
    Check(flows.IsCurrent(bittensor), "its challenge may open the bridge while nothing else started");
    // meanwhile the Solana sheet's Phantom connect takes the bridge
    const uint64_t solana = flows.Start();
    Check(!flows.IsCurrent(bittensor),
          "the late Bittensor challenge is stale: no tab, no message, no session reset");
    Check(flows.IsCurrent(solana), "the Solana connect still owns the bridge");
    CheckEq(static_cast<long long>(solana), static_cast<long long>(flows.Current()),
            "Current is the flow that started last");
  }
  {
    TEST_CASE("aSignInsChallengeBelongsToTheFlowItWasRoutedUnder");
    urnw::bridge::FlowSerial flows;
    flows.Start();  // SignInWithSolana starts and opens Phantom
    const uint64_t routed = flows.Current();  // on_public_key routes to SignIn, fetches the challenge
    Check(flows.IsCurrent(routed), "the challenge may ask for a signature while the sign-in owns the bridge");
    flows.Start();  // a Google sign-in starts meanwhile
    Check(!flows.IsCurrent(routed), "the late Solana challenge must not open a signing tab");
  }
  {
    TEST_CASE("flowNumbersNeverRepeat");
    urnw::bridge::FlowSerial flows;
    uint64_t last = flows.Current();
    bool increasing = true;
    for (int i = 0; i < 1000; ++i) {
      const uint64_t next = flows.Start();
      if (next <= last) increasing = false;
      last = next;
    }
    Check(increasing, "every start takes a new, larger number");
    Check(flows.IsCurrent(last) && !flows.IsCurrent(last - 1), "only the last one is current");
  }
}

}  // namespace

int main() {
  std::cout << "addresses\n";
  AddressTests();
  std::cout << "short form\n";
  ShortAddressTests();
  std::cout << "payout wallet\n";
  PayoutWalletTests();
  std::cout << "USDC waiting\n";
  PendingTests();
  std::cout << "the pane\n";
  PanelTests();
  std::cout << "the three reads\n";
  LegacyLoadTests();
  std::cout << "connect: wallet app\n";
  BridgeTests();
  std::cout << "connect: manual address\n";
  ManualTests();
  std::cout << "connect: linking\n";
  LinkTests();
  std::cout << "wallet bridge routing\n";
  BridgeRouteTests();

  std::cout << "\n" << gCases << " cases, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
