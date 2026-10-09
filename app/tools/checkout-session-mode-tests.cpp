// Executable spec for the upgrade sheet's checkout session shape
// (App/CheckoutSessionMode.h): an embedded session is redirect_on_completion
// "never" and opens on the inline bridge url, so the payment hands back from
// Stripe's onComplete; a hosted session leaves redirect_on_completion unset.
//
//   c++ -std=c++20 -I ../src/App checkout-session-mode-tests.cpp -o /tmp/checkout-session-mode-tests && /tmp/checkout-session-mode-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>

#include "CheckoutSessionMode.h"

using urnw::CheckoutSessionModeFor;

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

}  // namespace

int main() {
  // embedded: Stripe completes through onComplete, never the return_url
  const auto embedded = CheckoutSessionModeFor(true);
  CheckEq("embedded", embedded.uiMode, "embedded ui_mode");
  CheckEq("never", embedded.redirectOnCompletion, "embedded redirect_on_completion");
  Check(embedded.inlineBridge, "embedded opens the inline bridge url (onComplete hand-back)");

  // hosted: the browser checkout, unchanged
  const auto hosted = CheckoutSessionModeFor(false);
  CheckEq("hosted", hosted.uiMode, "hosted ui_mode");
  CheckEq("", hosted.redirectOnCompletion, "hosted redirect_on_completion");
  Check(!hosted.inlineBridge, "hosted does not open the bridge");

  std::cout << (gCases - gFailures) << "/" << gCases << " checkout session mode cases passed\n";
  return gFailures == 0 ? 0 : 1;
}
