// What the upgrade sheet says when its checkout page fails
// (BalanceSheets.cpp UpgradeSheet::HandleCheckoutCallback).
//
// The ur.io checkout page hands a failure back as
// urnetwork://checkout?errorCode=<code>&errorMessage=<its English text>, and the
// SDK's urnet::parseCheckoutRedirect passes the code on as sent
// (CheckoutRedirect::ErrorCode, one of urnet::CheckoutBridgeError*). A code
// this app knows reads in its own words; any other failure reads in the page's
// text (the page's invalid_request and checkout_error, whose text says what, a
// code this app does not know, and the -1 of pages before the codes), and one
// without a text reads something_went_wrong. The pay page's failure carries no
// code and reads the same way.
//
// WinRT/SDK-free so tools/checkout-bridge-error-tests.cpp runs it on any host.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <string_view>

namespace urnw {

// The words for a failed hand-back: a store key, or, when the key is empty,
// the page's own text.
struct CheckoutFailureText {
  std::string key;
  std::string pageText;
};

inline CheckoutFailureText CheckoutFailureTextFor(std::string_view code,
                                                  const std::string& errorMessage) {
  if (code == "checkout_unavailable") return {"checkout_error_unavailable", ""};
  if (code == "stripe_unavailable") return {"checkout_error_payment_form_unavailable", ""};
  if (!errorMessage.empty()) return {"", errorMessage};
  return {"something_went_wrong", ""};
}

}  // namespace urnw
