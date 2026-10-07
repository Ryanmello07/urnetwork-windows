// Executable spec for what the upgrade sheet says when its checkout page fails
// (App/CheckoutBridgeError.h): the checkout page's codes this app knows read in
// its own words, any other failure in the page's text, and one with no text
// says something went wrong.
//
//   c++ -std=c++20 -I ../src/App checkout-bridge-error-tests.cpp -o /tmp/checkout-bridge-error-tests && /tmp/checkout-bridge-error-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>

#include "CheckoutBridgeError.h"

using urnw::CheckoutFailureText;
using urnw::CheckoutFailureTextFor;

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
  const std::string english = "The page's English text.";

  // the page's codes (urnet::CheckoutBridgeError*) this app says in its own words
  struct Expected {
    const char* code;
    const char* key;
  };
  for (const Expected& c : {
           Expected{"checkout_unavailable", "checkout_error_unavailable"},
           Expected{"stripe_unavailable", "checkout_error_payment_form_unavailable"},
       }) {
    const CheckoutFailureText text = CheckoutFailureTextFor(c.code, english);
    CheckEq(c.key, text.key, std::string("the key for ") + c.code);
    CheckEq("", text.pageText, std::string("no page text for ") + c.code);
  }

  // the page's other failures, a code this app does not know, a page before the
  // codes (-1), and the pay page's failure, which has no code: the page's text
  for (const char* code : {"invalid_request", "checkout_error", "checkout_paused", "-1", ""}) {
    const CheckoutFailureText text = CheckoutFailureTextFor(code, english);
    CheckEq("", text.key, std::string("no key for \"") + code + "\"");
    CheckEq(english, text.pageText, std::string("the page's text for \"") + code + "\"");
  }

  // a failure without a text, a known code or not
  CheckEq("something_went_wrong", CheckoutFailureTextFor("checkout_error", "").key, "no text");
  CheckEq("something_went_wrong", CheckoutFailureTextFor("", "").key, "no code and no text");
  CheckEq("checkout_error_unavailable", CheckoutFailureTextFor("checkout_unavailable", "").key,
          "a known code without a text");

  std::cout << (gCases - gFailures) << "/" << gCases << " checkout bridge error checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
