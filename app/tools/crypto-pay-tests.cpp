// Executable spec for the crypto payment rules (App/CryptoPayPresentation.h,
// crypto-pay CONTRACT §4.3, §4.4, §6): the payment-method row (which methods
// show, the remembered method and its fallback to Card, the plan a coin buys,
// the labels and the event stores), the invoice's display for every state of
// the sdk's controller, the quote countdown, the gate in front of every
// payment uri, the QR's layout for real payment uris, the "Payment in
// progress" row, and a crypto year's plan rows (the notice's date, "Does not
// renew"). The rules the sdk owns (urnet::paymentMethods,
// paymentMethodAllowsPlan, checkCryptoPaymentUri, cryptoTermNoticeDue) are the
// sdk's own tests'; the gate's use of the uri check is pinned here with a fake.
//
//   c++ -std=c++20 -Wall -Wextra -Werror -I ../src/App -I ../third_party/qrcodegen crypto-pay-tests.cpp ../third_party/qrcodegen/qrcodegen.cpp -o /tmp/crypto-pay-tests && /tmp/crypto-pay-tests
//
// With URNW_CRYPTO_PAY_TESTS_SDK it also builds App/CryptoPaySdk.h against the
// generated header and reads the sdk's own structs through it, which pins the
// field names and types the app reads to the wrapper. The header needs
// nlohmann/json; both are system includes because the generated code does not
// build with -Wextra -Werror:
//
//   ... -DURNW_CRYPTO_PAY_TESTS_SDK -isystem <dir of urnetwork_sdk.hpp> -isystem <dir of nlohmann/> ...
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <qrcodegen.hpp>

#include "CryptoPayPresentation.h"

#if defined(URNW_CRYPTO_PAY_TESTS_SDK)
#include "CryptoPaySdk.h"
#endif

using namespace urnw::cryptopay;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << "\n";
  }
}

void CheckEq(const std::string& want, const std::string& got, const std::string& what) {
  Check(want == got, what + ": want \"" + want + "\", got \"" + got + "\"");
}

void CheckEq(int64_t want, int64_t got, const std::string& what) {
  Check(want == got, what + ": want " + std::to_string(want) + ", got " + std::to_string(got));
}

std::string Join(const std::vector<std::string>& values) {
  std::string joined;
  for (const std::string& value : values) joined += (joined.empty() ? "" : ",") + value;
  return joined;
}

std::string Join(const std::vector<int>& values) {
  std::string joined;
  for (const int value : values) joined += (joined.empty() ? "" : ",") + std::to_string(value);
  return joined;
}

std::string Quote(QuoteLine line) {
  switch (line) {
    case QuoteLine::Hidden: return "hidden";
    case QuoteLine::Counting: return "counting";
    case QuoteLine::Expired: return "expired";
  }
  return "?";
}

std::string Route(InvoiceRoute route) {
  switch (route) {
    case InvoiceRoute::None: return "none";
    case InvoiceRoute::Starting: return "starting";
    case InvoiceRoute::Open: return "open";
    case InvoiceRoute::Paid: return "paid";
    case InvoiceRoute::Unavailable: return "unavailable";
    case InvoiceRoute::Refused: return "refused";
    case InvoiceRoute::Unknown: return "unknown";
  }
  return "?";
}

// a fake of urnet::checkCryptoPaymentUri that records what it was asked
struct FakeCheck {
  bool answer = true;
  bool throws = false;
  int calls = 0;
  std::string asset, uri, address;
  bool operator()(const std::string& a, const std::string& u, const std::string& d) {
    ++calls;
    asset = a;
    uri = u;
    address = d;
    if (throws) throw std::runtime_error("urnet: the check failed");
    return answer;
  }
};

// A text of `count` characters from a base58-looking alphabet: the shape of a
// real address, not one (the spec never names a real receive address).
std::string Fill(char first, size_t count) {
  static const std::string alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
  std::string text(1, first);
  for (size_t at = 1; at < count; ++at) text += alphabet[(at * 7 + 3) % alphabet.size()];
  return text;
}

std::string Bech32Fill(const std::string& prefix, size_t count) {
  static const std::string alphabet = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
  std::string text = prefix;
  for (size_t at = prefix.size(); at < count; ++at) text += alphabet[(at * 11 + 5) % alphabet.size()];
  return text;
}

// The invoice of a Monero yearly plan, open and waiting.
CryptoInvoiceView MoneroInvoice() {
  CryptoInvoiceView view;
  view.phase = kPhaseOpen;
  view.intentId = "0b6f";
  view.asset = kAssetXmr;
  view.plan = kPlanYearly;
  view.address = "8sample";
  view.amount = "0.072387";
  view.ticker = "XMR";
  view.uri = "monero:8sample?tx_amount=0.072387&tx_description=URnetwork%20Pro%20yearly";
  view.priceUsd = 40;
  view.expiresAt = "2026-10-20T18:30:00Z";
  view.secondsLeft = 1799;
  view.status = kStatusPending;
  view.remaining = "0.072387";
  view.confirmationsRequired = 1;
  return view;
}

void MethodRow() {
  const std::vector<std::string> cardOnly = {kMethodCard};
  const std::vector<std::string> all = {kMethodCard, kAssetUsdcSolana, kAssetZec, kAssetXmr};
  const std::vector<std::string> monero = {kMethodCard, kAssetXmr};

  Check(!MethodRowShows({}), "no methods: no row");
  Check(!MethodRowShows(cardOnly), "Card alone (no coin listed): no row, the surface as before");
  Check(MethodRowShows(monero), "a listed coin shows the row");
  Check(MethodRowShows(all), "every coin listed shows the row");
  Check(!MethodRowShows({"card", "doge"}), "a method this app does not know is no coin");

  Check(IsCoin(kAssetUsdcSolana) && IsCoin(kAssetZec) && IsCoin(kAssetXmr), "the three coins");
  Check(!IsCoin(kMethodCard) && !IsCoin("") && !IsCoin("XMR"), "Card, nothing and a ticker are no coin");

  // the remembered method while it is listed, else Card
  CheckEq(kAssetXmr, MethodToSelect(monero, kAssetXmr), "a remembered coin that is listed");
  CheckEq(kMethodCard, MethodToSelect(monero, kAssetZec), "a remembered coin the server no longer lists");
  CheckEq(kMethodCard, MethodToSelect(cardOnly, kAssetXmr), "a remembered coin with every coin off");
  CheckEq(kMethodCard, MethodToSelect(monero, ""), "nothing remembered");
  CheckEq(kMethodCard, MethodToSelect(monero, kMethodCard), "Card remembered");
  CheckEq(kMethodCard, MethodToSelect({}, kAssetXmr), "no list at all");
  CheckEq(kMethodCard, MethodToSelect(all, "doge"), "a remembered method this app does not know");
  CheckEq(kAssetUsdcSolana, MethodToSelect(all, kAssetUsdcSolana), "USDC remembered and listed");
  Check(MethodOffered(cardOnly, kMethodCard), "Card is always offered");
  Check(!MethodOffered({kMethodCard, "xmr "}, kAssetXmr), "a coin is listed exactly");

  // the plan a coin buys, and what the row asks the sdk about
  CheckEq(kPlanYearlyOnboarding, CryptoPlanFor(true), "an active welcome offer: the offer");
  CheckEq(kPlanYearly, CryptoPlanFor(false), "no offer: yearly");
  CheckEq("yearly_onboarding", Join(PlanSurfaceItems(true)), "the items with the offer");
  CheckEq("yearly", Join(PlanSurfaceItems(false)), "the items without the offer");

  // labels and event stores
  CheckEq("pay_method_card", MethodLabelKey(kMethodCard), "Card's label");
  CheckEq("pay_method_usdc", MethodLabelKey(kAssetUsdcSolana), "USDC's label");
  CheckEq("pay_method_zcash", MethodLabelKey(kAssetZec), "Zcash's label");
  CheckEq("pay_method_monero", MethodLabelKey(kAssetXmr), "Monero's label");
  CheckEq("stripe", MethodEventStore(kMethodCard), "Card's events are Stripe's");
  CheckEq("solana", MethodEventStore(kAssetUsdcSolana), "USDC's events");
  CheckEq("zcash", MethodEventStore(kAssetZec), "Zcash's events");
  CheckEq("monero", MethodEventStore(kAssetXmr), "Monero's events");
  Check(PurchaseHasTrial(kMethodCard, true), "the card's yearly plan has the trial");
  Check(!PurchaseHasTrial(kMethodCard, false), "the card's monthly plan has none");
  Check(!PurchaseHasTrial(kAssetXmr, true), "a coin's year has no trial");
}

void InvoiceWords() {
  CheckEq("crypto_network_usdc", NetworkLineKey(kAssetUsdcSolana), "USDC's network line");
  CheckEq("crypto_network_zcash", NetworkLineKey(kAssetZec), "Zcash's network line");
  CheckEq("crypto_network_monero", NetworkLineKey(kAssetXmr), "Monero's network line");
  CheckEq("", NetworkLineKey(kMethodCard), "Card has no network line");
  CheckEq("solana:", UriSchemeFor(kAssetUsdcSolana), "USDC's scheme");
  CheckEq("zcash:", UriSchemeFor(kAssetZec), "Zcash's scheme");
  CheckEq("monero:", UriSchemeFor(kAssetXmr), "Monero's scheme");

  CheckEq("0:00", FormatCountdown(0), "zero");
  CheckEq("0:05", FormatCountdown(5), "seconds");
  CheckEq("1:05", FormatCountdown(65), "a minute");
  CheckEq("29:59", FormatCountdown(1799), "a fresh Monero quote");
  CheckEq("30:00", FormatCountdown(1800), "thirty minutes");
  CheckEq("60:00", FormatCountdown(3600), "a USDC quote's hour stays in minutes");
  CheckEq("0:00", FormatCountdown(-3), "never below zero");

  Check(!DecimalIsPositive(""), "nothing is not positive");
  Check(!DecimalIsPositive("0"), "zero");
  Check(!DecimalIsPositive("0.000000"), "zero with decimals");
  Check(DecimalIsPositive("0.000001"), "one micro");
  Check(DecimalIsPositive("12"), "a whole amount");
  Check(DecimalIsPositive(".5") && DecimalIsPositive("5."), "a bare point");
  Check(!DecimalIsPositive("-1"), "a sign is not this server's");
  Check(!DecimalIsPositive("1.2.3") && !DecimalIsPositive("1e3") && !DecimalIsPositive("0x1"),
        "anything but digits and one point");

  CheckEq("0.072387 XMR", AmountWithTicker("0.072387", "XMR"), "the amount with its ticker");
  CheckEq("0.072387", AmountWithTicker("0.072387", ""), "no ticker");
  CheckEq("", AmountWithTicker("", "XMR"), "no amount");
  CheckEq("Cake, Feather, Monerujo", JoinNames({"Cake", "Feather", "", "Monerujo"}), "the wallets");
  CheckEq("", JoinNames({}), "no wallets");
}

void UriGate() {
  const std::string address = "8sample";
  const std::string uri = "monero:8sample?tx_amount=0.072387&tx_description=URnetwork%20Pro%20yearly";
  {
    FakeCheck check;
    Check(CryptoUriAllowed(kAssetXmr, uri, address, check), "the sdk's check passes: drawn");
    CheckEq(1, check.calls, "the sdk is asked once");
    CheckEq(kAssetXmr, check.asset, "the sdk is asked about the asset");
    CheckEq(uri, check.uri, "the sdk is asked about the uri itself");
    CheckEq(address, check.address, "the sdk is asked about the invoice's address");
  }
  {
    FakeCheck check;
    check.answer = false;
    Check(!CryptoUriAllowed(kAssetXmr, uri, address, check), "the sdk's check refuses: never drawn");
  }
  {
    FakeCheck check;
    check.throws = true;
    Check(!CryptoUriAllowed(kAssetXmr, uri, address, check), "a check that throws refuses");
  }
  {
    FakeCheck check;
    Check(CryptoUriAllowed(kAssetXmr, "MONERO:8sample?tx_amount=1", address, check),
          "a scheme in capitals is the same scheme");
  }
  struct Refused {
    const char* why;
    std::string asset;
    std::string uri;
    std::string address;
  };
  for (const Refused& c : {
           Refused{"another asset's scheme", kAssetXmr, "zcash:u1sample?amount=1", address},
           Refused{"Monero's scheme on a Zcash invoice", kAssetZec, uri, "u1sample"},
           Refused{"a web link", kAssetUsdcSolana, "https://pay.example/solana", address},
           Refused{"no uri", kAssetXmr, "", address},
           Refused{"the scheme alone", kAssetXmr, "monero:", address},
           Refused{"no address", kAssetXmr, uri, ""},
           Refused{"no asset", "", uri, address},
           Refused{"an asset this app does not know", "doge", "doge:Dsample", address},
           Refused{"a space", kAssetXmr, "monero:8sample?tx_description=URnetwork Pro", address},
           Refused{"a line break", kAssetXmr, "monero:8sample\n?tx_amount=1", address},
           Refused{"a tab", kAssetXmr, "monero:8sample\t", address},
           Refused{"a control character", kAssetXmr, std::string("monero:8sample") + '\x01', address},
           Refused{"DEL", kAssetXmr, std::string("monero:8sample") + '\x7F', address},
           Refused{"a byte outside ASCII", kAssetXmr, "monero:8sample?tx_description=caf\xC3\xA9", address},
       }) {
    FakeCheck check;
    Check(!CryptoUriAllowed(c.asset, c.uri, c.address, check), std::string("refused: ") + c.why);
    CheckEq(0, check.calls, std::string("refused before the sdk is asked: ") + c.why);
  }
}

void Routes() {
  CryptoInvoiceView view;
  CheckEq("none", Route(RouteFor(view)), "no invoice");
  view.phase = "none";
  CheckEq("none", Route(RouteFor(view)), "the sdk's none");
  view.phase = kPhaseStarting;
  CheckEq("starting", Route(RouteFor(view)), "a start in flight");
  view = MoneroInvoice();
  CheckEq("open", Route(RouteFor(view)), "an open invoice");
  view.status = kStatusPaid;
  CheckEq("paid", Route(RouteFor(view)), "an open invoice the server credited");
  view.phase = kPhasePaid;
  CheckEq("paid", Route(RouteFor(view)), "paid");
  view = MoneroInvoice();
  view.phase = kPhaseFailed;
  view.status = kStatusUnknown;
  CheckEq("unknown", Route(RouteFor(view)), "an invoice the server no longer knows");
  view = CryptoInvoiceView{};
  view.phase = kPhaseFailed;
  view.errorCode = kErrorAssetUnavailable;
  CheckEq("unavailable", Route(RouteFor(view)), "a coin that cannot be quoted now");
  view.errorCode = "rate_limited";
  CheckEq("refused", Route(RouteFor(view)), "any other refusal");
  view.errorCode = kErrorGuestSignInRequired;
  CheckEq("refused", Route(RouteFor(view)), "a guest's refusal is the surface's");
  view.errorCode = "";
  CheckEq("refused", Route(RouteFor(view)), "a failure without a code (no answer)");
}

// The rules of CONTRACT §6.2.1, state by state.
void Display() {
  // payable: open, pending, time left
  CryptoInvoiceView view = MoneroInvoice();
  CryptoInvoiceDisplay display = DisplayFor(view, true);
  CheckEq("crypto_network_monero", display.networkLineKey, "the network line comes first");
  Check(!display.busy, "an open, polled invoice is not loading");
  Check(display.showAmount && display.showQr && display.showAddress, "payable: the amount, the QR, the address");
  CheckEq("0.072387 XMR", display.amountText, "send exactly the amount with its ticker");
  CheckEq("8sample", display.address, "to this address");
  Check(!display.showMemo && !display.showUsdcWarning && !display.showZcashNote,
        "Monero has no memo, no USDC warning and no Zcash note");
  CheckEq("counting", Quote(display.quote), "the countdown");
  CheckEq("29:59", display.countdown, "m:ss");
  Check(display.showRefresh && display.refreshEnabled, "Refresh quote while nothing was received");
  Check(display.showSteps, "the steps");
  CheckEq(0, display.step, "waiting");
  CheckEq("crypto_status_waiting", display.statusKey, "waiting for your payment");
  Check(!display.statusCounts && !display.showRemaining && !display.review && !display.paid,
        "nothing else to say while waiting");
  Check(display.showWallets, "the wallets that work");
  Check(display.showPayAnotherWay, "another way while nothing was received");
  Check(!display.showUnavailable && !display.showRefusal, "no failure");

  // the sdk's check refused the uri: no QR (and no Open in wallet), the rest stays
  display = DisplayFor(view, false);
  Check(display.showAmount && display.showAddress && !display.showQr,
        "a refused uri is never drawn or opened; the amount and the address are sent by hand");

  // requoted on another device (the sdk blanks the uri when a status brings
  // another amount): no QR and no Open in wallet, the new amount and the
  // address to send by hand, whatever the caller says of the uri
  {
    CryptoInvoiceView requoted = view;
    requoted.uri = "";
    requoted.amount = "0.071111";
    display = DisplayFor(requoted, true);
    Check(display.showAmount && display.showAddress && !display.showQr, "an empty uri is never drawn or opened");
    CheckEq("0.071111 XMR", display.amountText, "the new amount, to send by hand");
    FakeCheck check;
    Check(!CryptoUriAllowed(kAssetXmr, requoted.uri, requoted.address, check), "the gate refuses an empty uri");
    CheckEq(0, check.calls, "without asking the sdk");
  }

  // a requote in flight: the button stays, disabled
  view.refreshing = true;
  display = DisplayFor(view, true);
  Check(display.showRefresh && !display.refreshEnabled, "one requote at a time");
  view.refreshing = false;

  // expired: the countdown reached zero before the next poll said so
  view.secondsLeft = 0;
  display = DisplayFor(view, true);
  CheckEq("expired", Quote(display.quote), "a pending quote at zero reads expired");
  Check(!display.showAmount && !display.showQr && !display.showAddress && !display.showMemo,
        "expired: no amount, no QR, no Open in wallet");
  Check(display.showRefresh && display.refreshEnabled, "and Refresh quote");
  Check(!display.showSteps && !display.showWallets, "nothing else to pay with");
  Check(display.showPayAnotherWay, "another way stays open");

  // the server's expired
  view = MoneroInvoice();
  view.status = kStatusExpired;
  view.secondsLeft = 0;
  display = DisplayFor(view, true);
  CheckEq("expired", Quote(display.quote), "expired");
  Check(!display.showAmount && !display.showQr && display.showRefresh && display.showPayAnotherWay,
        "expired: refresh or another way, nothing to send");
  view.secondsLeft = 120;
  display = DisplayFor(view, true);
  CheckEq("expired", Quote(display.quote), "the server's expired whatever the clock says");
  Check(!display.showAmount, "and still nothing to send");

  // not polled yet: a resumed invoice before its first status
  view = MoneroInvoice();
  view.status = "";
  display = DisplayFor(view, true);
  Check(display.busy, "a brief loading line");
  Check(!display.showAmount && !display.showQr && !display.showAddress && !display.showMemo,
        "nothing payable until the first status");
  CheckEq("hidden", Quote(display.quote), "no countdown");
  Check(!display.showRefresh && !display.showPayAnotherWay, "money may be on its way: no requote, no other way");
  Check(!display.showSteps && !display.showWallets, "nothing else either");
  view.secondsLeft = 0;
  display = DisplayFor(view, true);
  CheckEq("hidden", Quote(display.quote), "an unpolled invoice at zero is not expired");

  // seen, short of the amount: the address for the top-up, never the QR or
  // the full amount (scanning again would overpay)
  view = MoneroInvoice();
  view.status = kStatusSeen;
  view.received = "0.06";
  view.remaining = "0.012387";
  view.secondsLeft = 600;
  display = DisplayFor(view, true);
  Check(display.showAddress && !display.showQr && !display.showAmount, "seen: the address, no QR, no full amount");
  Check(display.showRemaining, "how much more to send");
  CheckEq("0.012387 XMR", display.remainingText, "the remainder with its ticker");
  CheckEq("hidden", Quote(display.quote), "no countdown once money arrived");
  Check(!display.showRefresh && !display.showPayAnotherWay, "no requote, no other way once money arrived");
  CheckEq(1, display.step, "payment seen");
  CheckEq("crypto_status_seen", display.statusKey, "payment seen");
  Check(display.showSteps && display.showWallets, "the steps and the wallets");
  view.remaining = "0";
  display = DisplayFor(view, true);
  Check(!display.showRemaining, "nothing more to send");

  // confirming: the steps
  view = MoneroInvoice();
  view.status = kStatusConfirming;
  view.received = "0.072387";
  view.remaining = "0";
  view.confirmations = 0;
  view.confirmationsRequired = 1;
  display = DisplayFor(view, true);
  Check(!display.showAmount && !display.showQr && !display.showAddress && !display.showWallets,
        "confirming: nothing to send");
  CheckEq("hidden", Quote(display.quote), "no countdown while confirming");
  Check(!display.showRefresh && !display.showPayAnotherWay, "only Close");
  Check(display.showSteps, "the steps");
  CheckEq(2, display.step, "confirming");
  CheckEq("crypto_status_confirming", display.statusKey, "confirming n of N");
  Check(display.statusCounts, "with its counts");
  CheckEq(0, display.confirmations, "n");
  CheckEq(1, display.confirmationsRequired, "N");

  // paid
  view.phase = kPhasePaid;
  view.status = kStatusPaid;
  display = DisplayFor(view, true);
  Check(display.paid && display.showSteps, "paid: the steps, done");
  CheckEq(3, display.step, "paid");
  CheckEq("crypto_status_paid", display.statusKey, "paid, thank you");
  Check(!display.statusCounts && !display.showAmount && !display.showRefresh && !display.showPayAnotherWay,
        "nothing left to do");
  view.phase = kPhaseOpen;
  display = DisplayFor(view, true);
  Check(display.paid, "an open invoice whose status is paid is paid");

  // review: the copy, and nothing else to do
  view = MoneroInvoice();
  view.status = kStatusReview;
  view.reviewReason = "underpaid";
  view.errorCode = "underpaid";
  display = DisplayFor(view, true);
  Check(display.review, "review");
  CheckEq("crypto_status_review", display.statusKey, "the review copy");
  Check(!display.showSteps && !display.showAmount && !display.showQr && !display.showAddress &&
            !display.showRefresh && !display.showWallets && !display.showPayAnotherWay &&
            !display.showUnavailable && !display.showRefusal,
        "review: nothing else to do");
  CheckEq(-1, display.step, "no step in review");

  // USDC: the memo with the amount, the network warning with the address
  view = MoneroInvoice();
  view.asset = kAssetUsdcSolana;
  view.ticker = "USDC";
  view.amount = "40.012345";
  view.memo = "Rsample";
  view.uri = "solana:Msample?amount=40.012345";
  view.address = "Msample";
  display = DisplayFor(view, true);
  CheckEq("crypto_network_usdc", display.networkLineKey, "USDC on Solana");
  CheckEq("40.012345 USDC", display.amountText, "USDC's amount");
  Check(display.showMemo && display.memo == "Rsample", "the memo, for a transfer sent by hand");
  Check(display.showUsdcWarning, "the Solana-only warning, with the address");
  view.memo = "";
  display = DisplayFor(view, true);
  Check(!display.showMemo && display.showUsdcWarning, "no memo to show; the warning stays");
  view.secondsLeft = 0;
  display = DisplayFor(view, true);
  Check(!display.showUsdcWarning, "no address, no warning, once the quote expired");

  // Zcash: the exchange note while a payment is asked for
  view = MoneroInvoice();
  view.asset = kAssetZec;
  view.ticker = "ZEC";
  display = DisplayFor(view, true);
  CheckEq("crypto_network_zcash", display.networkLineKey, "Zcash, shielded");
  Check(display.showZcashNote && !display.showUsdcWarning, "the shielded-address note");
  view.status = kStatusSeen;
  display = DisplayFor(view, true);
  Check(display.showZcashNote, "and for the top-up");
  view.status = kStatusConfirming;
  display = DisplayFor(view, true);
  Check(!display.showZcashNote, "not once the payment is complete");

  // starting: busy
  view = CryptoInvoiceView{};
  view.phase = kPhaseStarting;
  view.asset = kAssetXmr;
  display = DisplayFor(view, true);
  Check(display.busy && !display.showAmount && !display.showSteps && !display.showRefresh &&
            !display.showPayAnotherWay,
        "starting: busy, nothing else");
  CheckEq("crypto_network_monero", display.networkLineKey, "the network line while starting");

  // the last requote failed
  view = MoneroInvoice();
  view.status = kStatusExpired;
  view.secondsLeft = 0;
  view.errorCode = kErrorAssetUnavailable;
  display = DisplayFor(view, true);
  Check(display.showUnavailable && !display.showRefusal, "a coin that cannot be quoted now");
  Check(display.showRefresh && display.showPayAnotherWay, "a retry, or another way");
  view.errorCode = kErrorQuoteExpired;
  display = DisplayFor(view, true);
  Check(!display.showUnavailable && !display.showRefusal, "an expired quote is the quote line's");
  view.errorCode = "rate_limited";
  display = DisplayFor(view, true);
  Check(!display.showUnavailable && display.showRefusal, "any other code is a refusal");
  view.errorCode = "";
  view.errorMessage = "Post \"https://api.example/pay/crypto/requote\": EOF";
  display = DisplayFor(view, true);
  Check(display.showRefusal, "a requote with no answer from the server");

  // the server no longer knows the invoice: the failure, and another way
  view = MoneroInvoice();
  view.phase = kPhaseFailed;
  view.status = kStatusUnknown;
  display = DisplayFor(view, true);
  Check(display.showRefusal && display.showPayAnotherWay, "unknown: the failure, and another way");
  Check(!display.showAmount && !display.showQr && !display.showAddress && !display.showSteps &&
            !display.showRefresh,
        "unknown: nothing to pay");

  // the steps each asset passes through
  CheckEq("0,1,2,3", Join(StepsFor(kAssetXmr, 1)), "Monero: waiting, seen, confirming, paid");
  CheckEq("0,1,2,3", Join(StepsFor(kAssetZec, 10)), "Zcash: the same");
  CheckEq("0,3", Join(StepsFor(kAssetUsdcSolana, 1)), "USDC: waiting, paid");
  CheckEq("0,1,3", Join(StepsFor(kAssetXmr, 0)), "no confirmations known: no confirming step");
}

void PaymentInProgress() {
  CryptoInvoiceView view = MoneroInvoice();
  Check(ShowsPaymentInProgress(view) && InvoiceInProgress(view), "a saved invoice waiting for its payment");
  view.status = "";
  Check(InvoiceInProgress(view), "a resumed invoice before its first status");
  view.status = kStatusSeen;
  Check(InvoiceInProgress(view), "money seen");
  view.status = kStatusConfirming;
  Check(InvoiceInProgress(view), "confirming");
  view.status = kStatusReview;
  Check(InvoiceInProgress(view), "in review: the row opens the review copy");
  view.status = kStatusExpired;
  Check(InvoiceInProgress(view), "an expired quote can still be refreshed");
  view.status = kStatusPaid;
  Check(!InvoiceInProgress(view), "paid: the success path, not a row");
  view.phase = kPhaseFailed;
  view.status = kStatusUnknown;
  Check(!InvoiceInProgress(view), "an invoice the server no longer knows");
  Check(!InvoiceInProgress(CryptoInvoiceView{}), "no invoice");
  view = CryptoInvoiceView{};
  view.phase = kPhaseStarting;
  Check(!InvoiceInProgress(view), "a start in flight is the sheet's");
  view.phase = kPhaseFailed;
  view.errorCode = kErrorAssetUnavailable;
  Check(!InvoiceInProgress(view), "a failed start");

  // the row's second line
  view = MoneroInvoice();
  CheckEq("crypto_status_waiting", InProgressNoteKey(view), "waiting for the payment");
  view.status = "";
  CheckEq("", InProgressNoteKey(view), "nothing before the first status: money may be on its way");
  view.status = kStatusPending;
  view.secondsLeft = 0;
  CheckEq("crypto_invoice_quote_expired", InProgressNoteKey(view), "a quote that ran out");
  view.status = kStatusExpired;
  CheckEq("crypto_invoice_quote_expired", InProgressNoteKey(view), "the server's expired");
  view.status = kStatusSeen;
  CheckEq("crypto_status_seen", InProgressNoteKey(view), "money seen, whatever the clock says");
  view.status = kStatusConfirming;
  CheckEq("crypto_status_confirming", InProgressNoteKey(view), "confirming, with its counts");
  view.status = kStatusReview;
  CheckEq("crypto_status_review", InProgressNoteKey(view), "in review");
  view.status = kStatusPaid;
  CheckEq("", InProgressNoteKey(view), "no row once paid");
}

void Times() {
  struct Parsed {
    const char* text;
    int64_t seconds;
  };
  for (const Parsed& c : {
           Parsed{"1970-01-01T00:00:00Z", 0},
           Parsed{"2026-10-20T18:30:00Z", 1792521000},
           Parsed{"2026-10-20T20:30:00+02:00", 1792521000},
           Parsed{"2026-10-20T13:30:00-05:00", 1792521000},
           Parsed{"2026-10-20T18:30:00.123456Z", 1792521000},
           Parsed{"2026-10-20t18:30:00z", 1792521000},
           Parsed{"2028-02-29T00:00:00Z", 1835395200},
           Parsed{"2000-03-01T12:00:00Z", 951912000},
           Parsed{"1969-12-31T23:59:59Z", -1},
       }) {
    const std::optional<int64_t> parsed = ParseRfc3339(c.text);
    Check(parsed.has_value(), std::string("parses: ") + c.text);
    if (parsed) CheckEq(c.seconds, *parsed, c.text);
  }
  for (const char* text : {"", "2026-10-20", "2026-10-20T18:30:00", "2026-10-20 18:30:00Z",
                           "2026-02-29T00:00:00Z", "2026-13-01T00:00:00Z", "2026-04-31T00:00:00Z",
                           "2026-10-20T24:00:00Z", "2026-10-20T18:60:00Z", "2026-10-20T18:30:00.Z",
                           "2026-10-20T18:30:00+0200", "2026-10-20T18:30:00Zjunk", "26-10-20T18:30:00Z",
                           "2026-1O-20T18:30:00Z"}) {
    Check(!ParseRfc3339(text).has_value(), std::string("refused: \"") + text + "\"");
  }

  Check(!StoreFamilyRenews("crypto"), "a crypto year never renews");
  for (const char* family : {"stripe", "apple", "google", "other", ""}) {
    Check(StoreFamilyRenews(family), std::string("renews or is not crypto: \"") + family + "\"");
  }
  Check(DoesNotRenewShows("crypto", false), "a crypto year: does not renew");
  Check(!DoesNotRenewShows("crypto", true), "the term notice says the rest");
  Check(!DoesNotRenewShows("stripe", false) && !DoesNotRenewShows("", false) &&
            !DoesNotRenewShows("other", false),
        "only a crypto year");
}

void Qr() {
  QrLayout empty = QrLayoutFor(0, 320);
  Check(empty.moduleCount == 0 && empty.frameSide == 0, "no code draws nothing");
  empty = QrLayoutFor(25, 0);
  Check(empty.moduleCount == 0 && empty.frameSide == 0, "no room draws nothing");
  const QrLayout small = QrLayoutFor(21, 320);
  CheckEq(11, static_cast<int64_t>(small.moduleSize), "whole dip per module");
  CheckEq(231, static_cast<int64_t>(small.codeSide), "the code's side");
  CheckEq(44, static_cast<int64_t>(small.quietZone), "four modules of quiet zone");
  CheckEq(319, static_cast<int64_t>(small.frameSide), "inside the frame");
  const QrLayout tiny = QrLayoutFor(21, 10);
  CheckEq(1, static_cast<int64_t>(tiny.moduleSize), "never below one dip");

  // the server's uris (§3.5) at real lengths: each encodes, and each draws at
  // four dip a module or more inside the invoice's 320-dip frame
  const std::string description = "URnetwork%20Pro%20yearly%20%28welcome%20offer%29";
  struct Sample {
    const char* name;
    std::string uri;
  };
  for (const Sample& sample : {
           Sample{"Monero", "monero:" + Fill('8', 95) + "?tx_amount=0.072387&tx_description=" + description},
           Sample{"Zcash", "zcash:" + Bech32Fill("u1", 213) + "?amount=0.123456&message=" + description},
           Sample{"USDC", "solana:" + Fill('M', 44) + "?amount=30.000001&spl-token=" + Fill('E', 44) +
                              "&reference=" + Fill('R', 44) + "&label=URnetwork&message=" + description},
       }) {
    try {
      const qrcodegen::QrCode code =
          qrcodegen::QrCode::encodeText(sample.uri.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
      const QrLayout layout = QrLayoutFor(code.getSize(), 320);
      Check(4 <= layout.moduleSize, std::string(sample.name) + ": four dip a module or more (" +
                                        std::to_string(code.getSize()) + " modules)");
      Check(layout.frameSide <= 320, std::string(sample.name) + ": inside the frame");
    } catch (const std::exception& e) {
      Check(false, std::string(sample.name) + ": does not encode: " + e.what());
    }
  }
}

#if defined(URNW_CRYPTO_PAY_TESTS_SDK)
// The generated wrapper's own structs, read through CryptoPaySdk.h.
void Sdk() {
  urnet::CryptoInvoiceState state;
  state.phase = "open";
  state.intent_id = "0b6f";
  state.asset = "xmr";
  state.plan = "yearly";
  state.item_id = "";
  state.address = "8sample";
  state.amount = "0.072387";
  state.ticker = "XMR";
  state.uri = "monero:8sample?tx_amount=0.072387";
  state.memo = "";
  state.price_usd = 40;
  state.expires_at = "2026-10-20T18:30:00Z";
  state.seconds_left = 1799;
  state.status = "seen";
  state.review_reason = "";
  state.received = "0.06";
  state.remaining = "0.012387";
  state.confirmations = 0;
  state.confirmations_required = 1;
  state.refreshing = true;
  state.error_code = "asset_unavailable";
  state.error_message = "Temporarily unavailable";
  const CryptoInvoiceView view = ViewFromSdk(state);
  CheckEq("open", view.phase, "sdk: the phase");
  CheckEq("0b6f", view.intentId, "sdk: the intent");
  CheckEq("xmr", view.asset, "sdk: the asset");
  CheckEq("yearly", view.plan, "sdk: the plan");
  CheckEq("8sample", view.address, "sdk: the address");
  CheckEq("0.072387", view.amount, "sdk: the amount");
  CheckEq("XMR", view.ticker, "sdk: the ticker");
  CheckEq("monero:8sample?tx_amount=0.072387", view.uri, "sdk: the uri");
  Check(view.priceUsd == 40, "sdk: the price");
  CheckEq("2026-10-20T18:30:00Z", view.expiresAt, "sdk: the expiry");
  CheckEq(1799, view.secondsLeft, "sdk: the seconds left");
  CheckEq("seen", view.status, "sdk: the status");
  CheckEq("0.06", view.received, "sdk: received");
  CheckEq("0.012387", view.remaining, "sdk: remaining");
  CheckEq(1, view.confirmationsRequired, "sdk: confirmations required");
  Check(view.refreshing, "sdk: refreshing");
  CheckEq("asset_unavailable", view.errorCode, "sdk: the error code");
  CheckEq("Temporarily unavailable", view.errorMessage, "sdk: the error message");

  // the balance's coins and term, from the server's json
  const urnet::SubscriptionBalanceResult result = nlohmann::json::parse(R"({
    "start_balance_byte_count": 0, "balance_byte_count": 0, "open_transfer_byte_count": 0,
    "crypto_assets": [
      {"asset": "usdc_solana", "plans": ["yearly", "yearly_onboarding"], "items": ["data_1tib"],
       "confirmations_required": 1, "quote_ttl_seconds": 3600},
      {"asset": "xmr", "plans": ["yearly"], "confirmations_required": 1, "quote_ttl_seconds": 1800}
    ],
    "crypto_term": {"market": "monero", "term_end_time": "2026-11-19T00:00:00Z",
                    "grace_end_time": "2026-12-19T00:00:00Z"}
  })").get<urnet::SubscriptionBalanceResult>();
  const urnet::CryptoPaymentAssetList assets = AssetsFromSdk(result);
  CheckEq(2, static_cast<int64_t>(assets.size()), "sdk: the balance's coins");
  if (assets.size() == 2) CheckEq("xmr", assets[1].asset, "sdk: a coin's asset");
  const CryptoTermView term = TermFromSdk(result);
  CheckEq("monero", term.market, "sdk: the term's market");
  CheckEq("2026-11-19T00:00:00Z", term.termEndTime, "sdk: the term's end");
  CheckEq("2026-12-19T00:00:00Z", term.graceEndTime, "sdk: the grace's end");
  const urnet::SubscriptionBalanceResult older = nlohmann::json::parse(R"({
    "start_balance_byte_count": 0, "balance_byte_count": 0, "open_transfer_byte_count": 0
  })").get<urnet::SubscriptionBalanceResult>();
  Check(AssetsFromSdk(older).empty(), "sdk: an older server lists no coin");
  Check(TermFromSdk(older).termEndTime.empty(), "sdk: and has no term");
}
#endif

}  // namespace

const char* LineName(PlanCardLine line) {
  switch (line) {
    case PlanCardLine::Price: return "price";
    case PlanCardLine::Secondary: return "secondary";
    case PlanCardLine::Equivalent: return "equivalent";
    case PlanCardLine::Trial: return "trial";
  }
  return "?";
}

// "price,secondary,-equivalent,..." with a minus for a line kept transparent
std::string Slots(const std::array<PlanCardSlot, 4>& slots) {
  std::string out;
  for (const PlanCardSlot& slot : slots) {
    if (!out.empty()) out += ",";
    if (!slot.shown) out += "-";
    out += LineName(slot.line);
  }
  return out;
}

// No renewal or trial wording while a coin is selected (CONTRACT §6.1).
void PlanCardCopy() {
  Check(CardTermsShown(kMethodCard), "Card keeps its trial and renewal lines");
  Check(!CardTermsShown(kAssetUsdcSolana) && !CardTermsShown(kAssetZec) && !CardTermsShown(kAssetXmr),
        "a coin buys one year once: no trial or renewal lines");
  CheckEq("price,secondary,equivalent,trial", Slots(YearlyCardSlots(true, true)),
          "Card with the offer: the first-year price, then the renewal price, the equivalent, the trial");
  CheckEq("price,secondary,equivalent,trial", Slots(YearlyCardSlots(true, false)),
          "Card without the offer: the price, the saving, the equivalent, the trial");
  CheckEq("price,equivalent,-secondary,-trial", Slots(YearlyCardSlots(false, true)),
          "a coin with the offer: no renewal price and no trial, their room kept below the equivalent");
  CheckEq("price,secondary,equivalent,-trial", Slots(YearlyCardSlots(false, false)),
          "a coin without the offer: the saving stays, the trial goes");
  // every line keeps a block, whatever the method: the card keeps its height
  for (const bool card : {true, false}) {
    for (const bool renews : {true, false}) {
      const auto slots = YearlyCardSlots(card, renews);
      int seen[4] = {0, 0, 0, 0};
      for (const PlanCardSlot& slot : slots) ++seen[static_cast<int>(slot.line)];
      Check(seen[0] == 1 && seen[1] == 1 && seen[2] == 1 && seen[3] == 1, "each line in one block");
      CheckEq("price", LineName(slots[0].line), "the price first, always");
      Check(slots[0].shown, "the price is what the coin pays: it stays");
      CheckEq("trial", LineName(slots[3].line), "the trial last, in its gold block");
      for (const PlanCardSlot& slot : slots) {
        const bool trialOrRenewal = slot.line == PlanCardLine::Trial ||
                                    (slot.line == PlanCardLine::Secondary && renews);
        if (!card && trialOrRenewal) Check(!slot.shown, "no trial or renewal wording for a coin");
        if (card) Check(slot.shown, "Card shows every line");
      }
    }
  }
  // the welcome-offer year is yearly to the events, whatever the method buys
  CheckEq(kPlanYearlyOnboarding, CryptoPlanFor(true), "the sdk is asked for the offer's year");
}

int main() {
  MethodRow();
  PlanCardCopy();
  InvoiceWords();
  UriGate();
  Routes();
  Display();
  PaymentInProgress();
  Times();
  Qr();
#if defined(URNW_CRYPTO_PAY_TESTS_SDK)
  Sdk();
#endif
  std::cout << (gCases - gFailures) << "/" << gCases << " crypto pay checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
