// Executable spec for the past-due notice (App/PaymentPastDue.h): it shows
// exactly while the balance carries payment_past_due, and "Update payment
// method" opens ur.io's Manage Subscription with the one-time code
// percent-encoded as one query value, the bare url without a code, and never
// a url from the wire.
//
//   c++ -std=c++20 -Wall -Wextra -Werror -I ../src/App payment-past-due-tests.cpp -o /tmp/payment-past-due-tests && /tmp/payment-past-due-tests
//
// With URNW_PAYMENT_PAST_DUE_TESTS_SDK it also builds App/PaymentPastDueSdk.h
// against the generated header and reads the server's balance json through
// it: the field from a server that sends it, none from an older server, and
// the auth code's answers. That pins the field names and types the app reads
// to the wrapper. The header needs nlohmann/json, and both are system includes
// because the generated code does not build with -Wextra -Werror:
//
//   ... -DURNW_PAYMENT_PAST_DUE_TESTS_SDK -isystem <dir of urnetwork_sdk.hpp> -isystem <dir of nlohmann/> ...
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <map>
#include <string>

#include "PaymentPastDue.h"
#include "UrlQuery.h"

#if defined(URNW_PAYMENT_PAST_DUE_TESTS_SDK)
#include "PaymentPastDueSdk.h"
#endif

using namespace urnw::pastdue;

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

const std::string kBase = "https://ur.io/app/subscription";

// The url's query as ur.io reads it (URLSearchParams): every parameter,
// decoded.
std::map<std::string, std::string> Query(const std::string& url) {
  return urnw::ParseUrlQuery(url);
}

// The url carries exactly one parameter, auth_code, whose decoded value is
// `code`, and nothing after it: no second parameter and no fragment.
void CheckCarries(const std::string& url, const std::string& code, const std::string& what) {
  Check(url.rfind(kBase + "?auth_code=", 0) == 0, what + ": the url is ur.io's with auth_code: " + url);
  Check(url.find('#') == std::string::npos, what + ": no fragment: " + url);
  Check(url.find('&') == std::string::npos, what + ": no second parameter: " + url);
  Check(url.find('?') == url.rfind('?'), what + ": one query: " + url);
  Check(url.find(' ') == std::string::npos, what + ": no space: " + url);
  const auto query = Query(url);
  Check(query.size() == 1, what + ": one parameter");
  const auto found = query.find("auth_code");
  CheckEq(code, found == query.end() ? std::string("<none>") : found->second, what + ": the code ur.io reads");
}

void Visibility() {
  // the field's presence is the rule: no time rule, no state
  PaymentPastDueView absent;
  Check(!NoticeShows(absent), "no payment_past_due: no notice");
  PaymentPastDueView present;
  present.present = true;
  present.store = "stripe";
  present.failTime = "2026-10-20T18:30:00Z";
  Check(NoticeShows(present), "payment_past_due: the notice");
  PaymentPastDueView old;
  old.present = true;
  old.store = "stripe";
  old.failTime = "2020-01-01T00:00:00Z";
  Check(NoticeShows(old), "an old fail time still shows: the server ends the notice, not the app");
  PaymentPastDueView bare;
  bare.present = true;
  Check(NoticeShows(bare), "present with empty fields still shows");
}

void Url() {
  CheckEq(kBase, kManageSubscriptionUrl, "the base is ur.io's Manage Subscription");

  // a server code: base64url, "=" padded (server model.AuthCodeCreate), the
  // url byte for byte apple's manageSubscriptionOnWebURL
  CheckEq(kBase + "?auth_code=Zm9vYmFyLWJhel9xdXg%3D", UpdatePaymentUrl("Zm9vYmFyLWJhel9xdXg="),
          "a server code: '-' and '_' stay, '=' is %3D");
  CheckCarries(UpdatePaymentUrl("Zm9vYmFyLWJhel9xdXg="), "Zm9vYmFyLWJhel9xdXg=", "a server code");

  // the characters that would add a parameter or a fragment, or read as a
  // space, each encoded
  CheckEq(kBase + "?auth_code=a%26b", UpdatePaymentUrl("a&b"), "'&' is %26");
  CheckEq(kBase + "?auth_code=a%3Db", UpdatePaymentUrl("a=b"), "'=' is %3D");
  CheckEq(kBase + "?auth_code=a%2Bb", UpdatePaymentUrl("a+b"), "'+' is %2B, never a space");
  CheckEq(kBase + "?auth_code=a%23b", UpdatePaymentUrl("a#b"), "'#' is %23");
  CheckEq(kBase + "?auth_code=a%3Fb", UpdatePaymentUrl("a?b"), "'?' is %3F");
  CheckEq(kBase + "?auth_code=a%20b", UpdatePaymentUrl("a b"), "a space is %20");
  CheckEq(kBase + "?auth_code=a%25b", UpdatePaymentUrl("a%b"), "'%' is %25");
  CheckEq(kBase + "?auth_code=a%2Fb", UpdatePaymentUrl("a/b"), "'/' is %2F");
  CheckEq(kBase + "?auth_code=%C3%BC", UpdatePaymentUrl("\xC3\xBC"), "a byte outside ASCII is %XX");
  const std::string hostile = "x&auth_code=evil#frag?y=1 +z";
  CheckCarries(UpdatePaymentUrl(hostile), hostile, "a code shaped like more parameters");

  // the white space around a code is not part of it (apple trims it too)
  CheckEq(kBase + "?auth_code=abc", UpdatePaymentUrl("  abc\r\n"), "the white space around a code goes");
  CheckEq(kBase + "?auth_code=a%20b", UpdatePaymentUrl("\ta b "), "white space inside a code stays, encoded");

  // no code (a failure or a blank answer): the bare url, and ur.io asks the
  // user to sign in
  CheckEq(kBase, UpdatePaymentUrl(""), "no code: the bare url");
  CheckEq(kBase, UpdatePaymentUrl(" \t\r\n"), "a blank code: the bare url");
  Check(Query(UpdatePaymentUrl("")).empty(), "the bare url has no parameter");

  // the code's life, as apple's and the auth code sheet's
  Check(kAuthCodeDurationMinutes == 5, "the code lasts five minutes");
  Check(kAuthCodeUses == 1, "the code is good for one use");
}

#if defined(URNW_PAYMENT_PAST_DUE_TESTS_SDK)
urnet::SubscriptionBalanceResult Balance(const char* json) {
  return nlohmann::json::parse(json).get<urnet::SubscriptionBalanceResult>();
}

// The generated wrapper's own structs, read through PaymentPastDueSdk.h.
void Sdk() {
  // a server that reports a failing renewal; update_payment_url is the wire's
  // and must never reach the url the action opens
  const PaymentPastDueView pastDue = PastDueFromSdk(Balance(R"({
    "start_balance_byte_count": 0, "balance_byte_count": 0, "open_transfer_byte_count": 0,
    "current_subscription": {"store": "stripe", "plan": "supporter"},
    "payment_past_due": {"store": "stripe", "fail_time": "2026-10-20T18:30:00Z",
                         "update_payment_url": "https://evil.example/steal?next="}
  })"));
  Check(pastDue.present, "sdk: payment_past_due is read");
  CheckEq("stripe", pastDue.store, "sdk: the store");
  CheckEq("2026-10-20T18:30:00Z", pastDue.failTime, "sdk: the fail time");
  Check(NoticeShows(pastDue), "sdk: a balance with payment_past_due shows the notice");
  const std::string url = UpdatePaymentUrl("Zm9vYmFy");
  CheckEq(kBase + "?auth_code=Zm9vYmFy", url, "sdk: the action's url is ur.io's, whatever the wire says");
  Check(url.find("evil.example") == std::string::npos, "sdk: the wire's url never reaches the action");

  // the notice does not follow the plan: a failing renewal on a free network
  const PaymentPastDueView freePlan = PastDueFromSdk(Balance(R"({
    "start_balance_byte_count": 0, "balance_byte_count": 0, "open_transfer_byte_count": 0,
    "payment_past_due": {"store": "stripe", "fail_time": "2026-10-20T18:30:00Z",
                         "update_payment_url": "https://ur.io/app/subscription"}
  })"));
  Check(NoticeShows(freePlan), "sdk: payment_past_due without a current subscription still shows");

  // an older server: no field at all
  const PaymentPastDueView older = PastDueFromSdk(Balance(R"({
    "start_balance_byte_count": 0, "balance_byte_count": 0, "open_transfer_byte_count": 0,
    "current_subscription": {"store": "stripe", "plan": "supporter"}
  })"));
  Check(!older.present, "sdk: an older server's balance has none");
  Check(!NoticeShows(older), "sdk: an older server's balance shows no notice");
  Check(older.store.empty() && older.failTime.empty(), "sdk: an older server's balance leaves the fields empty");

  // a server that sends null
  const PaymentPastDueView nulled = PastDueFromSdk(Balance(R"({
    "start_balance_byte_count": 0, "balance_byte_count": 0, "open_transfer_byte_count": 0,
    "payment_past_due": null
  })"));
  Check(!NoticeShows(nulled), "sdk: payment_past_due null shows no notice");

  // the action's code request: one use within five minutes, nothing else
  const urnet::AuthCodeCreateArgs args = UpdatePaymentAuthCodeArgs();
  Check(args.duration_minutes && *args.duration_minutes == 5, "sdk: the code lasts five minutes");
  Check(args.uses && *args.uses == 1, "sdk: the code is good for one use");
  Check(!args.roles && !args.principal, "sdk: the code asks for no roles and no principal");

  // the code's answers
  urnet::AuthCodeCreateResult made;
  made.auth_code = "Zm9vYmFy";
  CheckEq("Zm9vYmFy", AuthCodeFromSdk(made, std::nullopt), "sdk: a code");
  CheckEq(kBase + "?auth_code=Zm9vYmFy", UpdatePaymentUrl(AuthCodeFromSdk(made, std::nullopt)),
          "sdk: a code opens ur.io signed in");
  CheckEq("", AuthCodeFromSdk(std::nullopt, std::string("no route to host")),
          "sdk: no answer from the server: no code");
  CheckEq("", AuthCodeFromSdk(made, std::string("timeout")), "sdk: a transport error: no code");
  CheckEq("", AuthCodeFromSdk(std::nullopt, std::nullopt), "sdk: no result: no code");
  urnet::AuthCodeCreateResult limited;
  limited.error = urnet::AuthCodeCreateError{};
  limited.error->auth_code_limit_exceeded = true;
  CheckEq("", AuthCodeFromSdk(limited, std::nullopt), "sdk: the auth-code limit: no code");
  urnet::AuthCodeCreateResult refusedWithCode;
  refusedWithCode.auth_code = "Zm9vYmFy";
  refusedWithCode.error = urnet::AuthCodeCreateError{};
  refusedWithCode.error->message = "refused";
  CheckEq("", AuthCodeFromSdk(refusedWithCode, std::nullopt), "sdk: a refusal wins over a code");
  urnet::AuthCodeCreateResult empty;
  CheckEq("", AuthCodeFromSdk(empty, std::nullopt), "sdk: an answer without a code: no code");
  CheckEq(kBase, UpdatePaymentUrl(AuthCodeFromSdk(limited, std::nullopt)), "sdk: no code opens the bare url");
  urnet::AuthCodeCreateResult blank;
  blank.auth_code = "   ";
  CheckEq(kBase, UpdatePaymentUrl(AuthCodeFromSdk(blank, std::nullopt)), "sdk: a blank code opens the bare url");
}
#endif

}  // namespace

int main() {
  Visibility();
  Url();
#if defined(URNW_PAYMENT_PAST_DUE_TESTS_SDK)
  Sdk();
#endif
  std::cout << (gCases - gFailures) << "/" << gCases << " payment past due checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
