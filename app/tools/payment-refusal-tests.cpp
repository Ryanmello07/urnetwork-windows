// Executable spec for what the app's own payment screens say when the server
// refuses (App/PaymentRefusal.h): the upgrade sheet's hosted checkout session
// and Manage subscription in Settings. A code with a line of its own reads that
// line alone, invalid_request and start_failed read the screen's own line
// alone, and any other code, or none, reads the screen's own line with the
// server's words under it.
//
//   c++ -std=c++20 -I ../src/App payment-refusal-tests.cpp -o /tmp/payment-refusal-tests && /tmp/payment-refusal-tests
//
// With URNW_PAYMENT_REFUSAL_TESTS_SDK the refusals are the generated header's
// own structs (urnet::StripeCreateCheckoutSessionError and
// urnet::StripeCreateCustomerPortalError), parsed from the server's json
// through the header's conversions. The header needs nlohmann/json; both are
// system includes because the generated code does not build with -Wextra
// -Werror:
//
//   c++ -std=c++20 -Wall -Wextra -Werror -DURNW_PAYMENT_REFUSAL_TESTS_SDK -I ../src/App -isystem <dir of urnetwork_sdk.hpp> -isystem <dir of nlohmann/> payment-refusal-tests.cpp -o ...
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <optional>
#include <string>

#include "PaymentRefusal.h"

#if defined(URNW_PAYMENT_REFUSAL_TESTS_SDK)
#include "urnetwork_sdk.hpp"
#endif

using urnw::PaymentRefusalMessage;
using urnw::PaymentRefusalText;
using urnw::PaymentRefusalTextFor;

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

// The spec's wide text is ASCII.
std::string Narrow(const std::wstring& value) {
  std::string narrow;
  for (const wchar_t character : value) narrow += static_cast<char>(character);
  return narrow;
}

void CheckText(const PaymentRefusalText& want, const PaymentRefusalText& got,
               const std::string& what) {
  CheckEq(want.key, got.key, what + ", the line");
  CheckEq(want.detail, got.detail, what + ", the words under it");
}

#if defined(URNW_PAYMENT_REFUSAL_TESTS_SDK)
using CheckoutSessionError = urnet::StripeCreateCheckoutSessionError;
using CustomerPortalError = urnet::StripeCreateCustomerPortalError;
#else
// The fields of the sdk's refusals, by the generated wrapper's names and types.
struct CheckoutSessionError {
  std::optional<std::string> code;
  std::string message;
};
struct CustomerPortalError {
  std::optional<std::string> code;
  std::string message;
};
#endif

}  // namespace

int main() {
  const std::string words = "The server's English words.";
  const std::string screenKey = "something_went_wrong";

  // the codes with a line of their own: that line alone, without the words
  struct Expected {
    const char* code;
    const char* key;
  };
  for (const Expected& c : {
           Expected{"already_subscribed", "site_payment_error_already_subscribed"},
           Expected{"plan_unavailable", "site_payment_error_plan_unavailable"},
           Expected{"checkout_unavailable", "checkout_error_unavailable"},
           Expected{"no_customer", "site_subscription_error_no_customer"},
           Expected{"store_unavailable", "site_subscription_error_store_unavailable"},
       }) {
    CheckText({c.key, ""}, PaymentRefusalTextFor(c.code, words, screenKey),
              std::string("the code ") + c.code);
  }

  // a defect of this app's request, and a start to try again: the screen's
  // own line alone
  for (const char* code : {"invalid_request", "start_failed"}) {
    CheckText({screenKey, ""}, PaymentRefusalTextFor(code, words, screenKey),
              std::string("the code ") + code);
  }

  // any other code, none (an older server), or a code spelled otherwise (the
  // server sends them lowercase, and they compare exactly): the screen's own
  // line with the server's words under it
  for (const char* code : {"", "rate_limited", "item_unavailable", "offer_unavailable",
                           "guest_sign_in_required", "ALREADY_SUBSCRIBED", "No_Customer",
                           " start_failed"}) {
    CheckText({screenKey, words}, PaymentRefusalTextFor(code, words, screenKey),
              std::string("the code \"") + code + "\"");
  }

  // a refusal without words has nothing to put under the line
  CheckText({screenKey, ""}, PaymentRefusalTextFor("rate_limited", "", screenKey),
            "an unknown code without words");
  CheckText({screenKey, ""}, PaymentRefusalTextFor("", "", screenKey), "no code and no words");

  // the screen's own line is the caller's
  const std::string otherScreenKey = "sample_screen_line";
  CheckText({otherScreenKey, ""}, PaymentRefusalTextFor("start_failed", words, otherScreenKey),
            "another screen's start_failed");
  CheckText({otherScreenKey, words}, PaymentRefusalTextFor("", words, otherScreenKey),
            "another screen's refusal without a code");
  CheckText({"site_payment_error_already_subscribed", ""},
            PaymentRefusalTextFor("already_subscribed", words, otherScreenKey),
            "another screen's already_subscribed");

  // what the screen shows: the line alone, or the words on the line under it
  CheckEq("A line.", Narrow(PaymentRefusalMessage(L"A line.", L"")), "a line without words");
  CheckEq("A line.\nThe words.", Narrow(PaymentRefusalMessage(L"A line.", L"The words.")),
          "a line with words");

  // the sdk's refusals as the screens read them: an optional code beside the
  // message, none from an older server
  {
    CheckoutSessionError refused;
    refused.code = "already_subscribed";
    refused.message = words;
    CheckText({"site_payment_error_already_subscribed", ""},
              PaymentRefusalTextFor(refused, screenKey), "a checkout session refusal");
    CheckoutSessionError older;
    older.message = words;
    CheckText({screenKey, words}, PaymentRefusalTextFor(older, screenKey),
              "a checkout session refusal without a code");
    CustomerPortalError portal;
    portal.code = "no_customer";
    portal.message = words;
    CheckText({"site_subscription_error_no_customer", ""}, PaymentRefusalTextFor(portal, screenKey),
              "a billing portal refusal");
    CustomerPortalError olderPortal;
    olderPortal.message = words;
    CheckText({screenKey, words}, PaymentRefusalTextFor(olderPortal, screenKey),
              "a billing portal refusal without a code");
  }

#if defined(URNW_PAYMENT_REFUSAL_TESTS_SDK)
  // the server's json, through the header's conversions
  {
    const urnet::StripeCreateCustomerPortalResult portal = nlohmann::json::parse(R"({
      "error": {"code": "store_unavailable", "message": "The server's English words."}
    })").get<urnet::StripeCreateCustomerPortalResult>();
    Check(portal.error.has_value(), "the billing portal's refusal parses");
    if (portal.error) {
      CheckText({"site_subscription_error_store_unavailable", ""},
                PaymentRefusalTextFor(*portal.error, screenKey), "the billing portal's json");
    }
    const urnet::StripeCreateCustomerPortalResult olderPortal = nlohmann::json::parse(R"({
      "error": {"message": "The server's English words."}
    })").get<urnet::StripeCreateCustomerPortalResult>();
    Check(olderPortal.error.has_value(), "an older billing portal refusal parses");
    if (olderPortal.error) {
      CheckText({screenKey, words}, PaymentRefusalTextFor(*olderPortal.error, screenKey),
                "an older billing portal's json");
    }
    const urnet::StripeCreateCheckoutSessionResult session = nlohmann::json::parse(R"({
      "error": {"code": "plan_unavailable", "message": "The server's English words."}
    })").get<urnet::StripeCreateCheckoutSessionResult>();
    Check(session.error.has_value(), "the checkout session's refusal parses");
    if (session.error) {
      CheckText({"site_payment_error_plan_unavailable", ""},
                PaymentRefusalTextFor(*session.error, screenKey), "the checkout session's json");
    }
  }
#endif

  std::cout << (gCases - gFailures) << "/" << gCases << " payment refusal checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
