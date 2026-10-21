// The sdk's crypto-pay surface (crypto-pay CONTRACT §4.2-4.5), read into this
// app's views (CryptoPayPresentation.h). The one place that names the fields
// of urnet::CryptoInvoiceState and of SubscriptionBalanceResult's
// crypto_assets / crypto_term, and the one place that calls the sdk's pure
// crypto-pay rules: urnet::paymentMethods, urnet::paymentMethodAllowsPlan,
// urnet::checkCryptoPaymentUri, urnet::cryptoWalletSuggestions and
// urnet::cryptoTermNoticeDue. Everything else in the app reads the views, so a
// change in the generated wrapper is a change here alone.
//
// Header-only and WinRT-free. It needs the generated urnetwork_sdk.hpp (and
// nlohmann/json), so tools/crypto-pay-tests.cpp builds it against a header
// that has the crypto-pay surface, wherever one is (the vendored
// app/third_party/urnetwork-sdk/<arch>, URNETWORK_SDK_INCLUDE, or the sdk
// checkout beside this repository): that pins every field name and type read
// below to the wrapper. Nothing here calls the C ABI unless the app calls it,
// so the spec links without the sdk library.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <exception>
#include <optional>
#include <string>
#include <vector>

#include <urnetwork_sdk.hpp>

#include "CryptoPayPresentation.h"

namespace urnw::cryptopay {

// The controller's state (sdk crypto_invoice_view_controller.go
// CryptoInvoiceState, json-tagged, so the wrapper's members are the json
// names), field for field.
inline CryptoInvoiceView ViewFromSdk(const urnet::CryptoInvoiceState& state) {
  CryptoInvoiceView view;
  view.phase = state.phase;
  view.intentId = state.intent_id;
  view.asset = state.asset;
  view.plan = state.plan;
  view.itemId = state.item_id;
  view.address = state.address;
  view.amount = state.amount;
  view.ticker = state.ticker;
  view.uri = state.uri;
  view.memo = state.memo;
  view.priceUsd = state.price_usd;
  view.expiresAt = state.expires_at;
  view.secondsLeft = state.seconds_left;
  view.status = state.status;
  view.reviewReason = state.review_reason;
  view.received = state.received;
  view.remaining = state.remaining;
  view.confirmations = state.confirmations;
  view.confirmationsRequired = state.confirmations_required;
  view.refreshing = state.refreshing;
  view.errorCode = state.error_code;
  view.errorMessage = state.error_message;
  return view;
}

// SubscriptionBalanceResult.crypto_term (§2.2); none from an older server.
inline CryptoTermView TermFromSdk(const urnet::SubscriptionBalanceResult& result) {
  CryptoTermView view;
  if (!result.crypto_term) return view;
  view.market = result.crypto_term->market;
  view.termEndTime = result.crypto_term->term_end_time;
  view.graceEndTime = result.crypto_term->grace_end_time;
  return view;
}

// SubscriptionBalanceResult.crypto_assets (§2.2): the coins the server sells
// now; empty from an older server and with every coin switched off.
inline urnet::CryptoPaymentAssetList AssetsFromSdk(const urnet::SubscriptionBalanceResult& result) {
  return result.crypto_assets.value_or(urnet::CryptoPaymentAssetList{});
}

// The method row's methods on a plan surface: urnet::paymentMethods over the
// balance's coins and the plan a coin buys there (Card, then each listed coin
// that sells it, in the order usdc_solana, zec, xmr). Card alone when no coin
// is listed or the sdk refuses, which hides the row.
inline std::vector<std::string> PaymentMethodsFor(const urnet::CryptoPaymentAssetList& assets,
                                                  bool offerActive) {
  if (assets.empty()) return {kMethodCard};
  try {
    const urnet::StringList items = PlanSurfaceItems(offerActive);
    if (const std::optional<urnet::StringList> methods = urnet::paymentMethods(assets, items)) {
      return *methods;
    }
  } catch (const std::exception&) {
    // a malformed list: Card alone, as an older server
  }
  return {kMethodCard};
}

// Picking a coin greys out monthly ("Monthly is card only") and moves the
// selection to yearly: no coin sells monthly (urnet::paymentMethodAllowsPlan).
inline bool MonthlyLocked(const std::string& method) {
  if (!IsCoin(method)) return false;
  try {
    return !urnet::paymentMethodAllowsPlan(method, kPlanMonthly);
  } catch (const std::exception&) {
    return true;
  }
}

// CryptoUriAllowed with the sdk's own check: whether this view's uri may be
// drawn as a QR and opened.
inline bool UriAllowed(const CryptoInvoiceView& view) {
  return CryptoUriAllowed(view.asset, view.uri, view.address,
                          [](const std::string& asset, const std::string& uri,
                             const std::string& address) {
                            return urnet::checkCryptoPaymentUri(asset, uri, address);
                          });
}

// "Wallets that work" for an asset (urnet::cryptoWalletSuggestions).
inline std::vector<std::string> WalletSuggestions(const std::string& asset) {
  try {
    if (const std::optional<urnet::StringList> names = urnet::cryptoWalletSuggestions(asset)) {
      return *names;
    }
  } catch (const std::exception&) {
  }
  return {};
}

// The renewal notice is due (urnet::cryptoTermNoticeDue): from the paid
// term's end until its grace ends, never before.
inline bool TermNoticeDue(const CryptoTermView& term) {
  if (term.termEndTime.empty() || term.graceEndTime.empty()) return false;
  urnet::CryptoTerm sdkTerm;
  sdkTerm.market = term.market;
  sdkTerm.term_end_time = term.termEndTime;
  sdkTerm.grace_end_time = term.graceEndTime;
  try {
    return urnet::cryptoTermNoticeDue(sdkTerm);
  } catch (const std::exception&) {
    return false;
  }
}

}  // namespace urnw::cryptopay
