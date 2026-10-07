// What the app's own payment screens say when the server refuses: the upgrade
// sheet's hosted checkout session (BalanceSheets.cpp UpgradeSheet::RequestSession,
// POST /stripe/create-checkout-session) and Manage subscription in Settings
// (SettingsPage.cpp SettingsPage::OpenCustomerPortal, POST /stripe/customer-portal).
// The rule is ur.io's (src/lib/paymentFailure.js):
//
// - a code with a line of its own reads that line alone, in the reader's
//   language (already_subscribed, plan_unavailable and checkout_unavailable
//   from a checkout; no_customer and store_unavailable from the billing portal)
// - invalid_request (a defect of this app's request) and start_failed (a start
//   to try again) read the screen's own line alone
// - any other code, or none (an older server), reads the screen's own line
//   with the server's English words under it
//
// The refusal's code is error.code as the server sent it (connect's
// PurchaseError and SubscriptionError), kept by the sdk's
// urnet::StripeCreateCheckoutSessionError and
// urnet::StripeCreateCustomerPortalError; codes compare exactly. A guest's
// guest_sign_in_required never gets here: the upgrade sheet sends a guest to
// the add-sign-in flow first (GuestConversion.h PurchaseRefusalFor). A failure
// with no answer from the server (the sdk's err) is not a refusal, and each
// screen words it as before.
//
// WinRT/SDK-free so tools/payment-refusal-tests.cpp runs it on any host.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <string_view>

namespace urnw {

// The words for a refusal: the store key of the line, and the server's words
// to show under it ("" for none).
struct PaymentRefusalText {
  std::string key;
  std::string detail;
};

// `screenKey` is the screen's own line, for a refusal without a line of its own.
inline PaymentRefusalText PaymentRefusalTextFor(std::string_view code,
                                                const std::string& serverMessage,
                                                std::string_view screenKey) {
  if (code == "already_subscribed") return {"site_payment_error_already_subscribed", ""};
  if (code == "plan_unavailable") return {"site_payment_error_plan_unavailable", ""};
  if (code == "checkout_unavailable") return {"checkout_error_unavailable", ""};
  if (code == "no_customer") return {"site_subscription_error_no_customer", ""};
  if (code == "store_unavailable") return {"site_subscription_error_store_unavailable", ""};
  if (code == "invalid_request" || code == "start_failed") return {std::string(screenKey), ""};
  return {std::string(screenKey), serverMessage};
}

// The same for the sdk's refusal (a urnet::*Error with an optional code beside
// its message), as the screens read it.
template <typename SdkError>
PaymentRefusalText PaymentRefusalTextFor(const SdkError& error, std::string_view screenKey) {
  return PaymentRefusalTextFor(error.code.value_or(std::string()), error.message, screenKey);
}

// What the screen shows: the line in the reader's language, and the server's
// words on the line under it when there are any.
inline std::wstring PaymentRefusalMessage(std::wstring line, std::wstring_view detail) {
  if (detail.empty()) return line;
  line += L"\n";
  line += detail;
  return line;
}

}  // namespace urnw
