// Executable spec for the app's one percent-encoder and query parser
// (App/UrlQuery.h), shared by the pay sheet and the wallet-connect and
// sign-in bridges; the ur.io/checkout envelope is the SDK's.
//
//   c++ -std=c++20 -I ../src/App url-query-tests.cpp \
//       -o /tmp/url-query-tests && /tmp/url-query-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>

#include "UrlQuery.h"

using namespace urnw;

namespace {

int gFailures = 0;
int gCases = 0;

void CheckEq(std::string const& want, std::string const& got, std::string const& what) {
  ++gCases;
  if (want != got) {
    ++gFailures;
    std::cout << "  FAIL " << what << ": want \"" << want << "\", got \"" << got << "\"\n";
  }
}

}  // namespace

int main() {
  // RFC 3986 unreserved characters pass; everything else is %XX
  CheckEq("AZaz09-_.~", PercentEncode("AZaz09-_.~"), "unreserved characters pass");
  CheckEq("urnetwork%3A%2F%2Fpay%2Fdone", PercentEncode("urnetwork://pay/done"), "a return url");
  CheckEq("a%20b%2Bc%26d%3De", PercentEncode("a b+c&d=e"), "space, plus, amp, equals");
  CheckEq("%C3%A9", PercentEncode("\xC3\xA9"), "utf-8 bytes");

  // form decoding: the pages build their hand-backs with URLSearchParams
  CheckEq("Could not load Stripe.", PercentDecode("Could+not+load+Stripe."), "'+' is a space");
  CheckEq("a b+c&d=e", PercentDecode(PercentEncode("a b+c&d=e")), "round trip");
  CheckEq("100%", PercentDecode("100%"), "a trailing percent is kept");
  CheckEq("%zz", PercentDecode("%zz"), "a malformed escape is kept");

  auto params = ParseUrlQuery("urnetwork://pay/error?errorMessage=Card+declined%21&x=1");
  CheckEq("Card declined!", params["errorMessage"], "the pay page's error message");
  CheckEq("1", params["x"], "a second pair");
  CheckEq("", ParseUrlQuery("urnetwork://pay/done").count("errorMessage") ? "present" : "",
          "no query, no pairs");
  auto query = ParseQueryString("nonce=abc&data=x%2By&flag");
  CheckEq("abc", query["nonce"], "a query string pair");
  CheckEq("x+y", query["data"], "an escaped plus stays a plus");
  CheckEq("", query.count("flag") ? "present" : "", "a pair without '=' is skipped");

  std::cout << (gCases - gFailures) << "/" << gCases << " url query checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
