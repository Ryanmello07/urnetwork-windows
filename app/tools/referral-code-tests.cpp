// Executable spec for the referral code read behind the referral card
// (App/ReferralCodeState.h): a failed read with no code ends the ring with an
// error and Try again, every settled read repaints the card, a failed
// background poll keeps a code already shown, and Try again goes back to the
// ring until the answer lands - run against the SAME
// header the app compiles, on any host with a C++20 compiler.
//
//   c++ -std=c++20 -I ../src/App referral-code-tests.cpp \
//       -o /tmp/referral-code-tests && /tmp/referral-code-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <optional>
#include <string>

#include "ReferralCodeState.h"

using urnw::ReferralCodeFetch;
using urnw::ReferralCodeView;

namespace {

int gFailures = 0;
int gCases = 0;
std::string gCurrentCase;

void Fail(const std::string& message) {
  ++gFailures;
  std::cout << "  FAIL [" << gCurrentCase << "] " << message << "\n";
}

std::string ViewName(ReferralCodeView view) {
  switch (view) {
    case ReferralCodeView::Loading: return "Loading";
    case ReferralCodeView::Code: return "Code";
    case ReferralCodeView::Unavailable: return "Unavailable";
  }
  return "?";
}

void CheckView(ReferralCodeView expected, const ReferralCodeFetch& fetch, const std::string& what) {
  if (expected != fetch.View()) {
    Fail(what + ": expected " + ViewName(expected) + ", got " + ViewName(fetch.View()));
  }
}

void CheckCode(const std::string& expected, const ReferralCodeFetch& fetch, const std::string& what) {
  const std::string actual = fetch.Code().value_or(std::string("<none>"));
  if (expected != actual) Fail(what + ": expected \"" + expected + "\", got \"" + actual + "\"");
}

struct Case {
  explicit Case(const char* name) {
    gCurrentCase = name;
    ++gCases;
  }
};
#define TEST_CASE(name) Case case_##__LINE__(name)

constexpr const char* kTestCode = "TESTCODE1";

}  // namespace

int main() {
  {
    TEST_CASE("before any answer the card shows the ring");
    ReferralCodeFetch fetch;
    CheckView(ReferralCodeView::Loading, fetch, "fresh");
  }
  {
    TEST_CASE("a failed first read ends the ring");
    ReferralCodeFetch fetch;
    const bool repaint = fetch.Fail();
    CheckView(ReferralCodeView::Unavailable, fetch, "after a failure");
    if (!repaint) Fail("a failed read did not repaint the card, which keeps spinning");
  }
  {
    TEST_CASE("a successful read repaints the card at once");
    ReferralCodeFetch fetch;
    if (!fetch.Succeed(std::string(kTestCode))) {
      Fail("a successful read did not repaint the card; it waits for a balance publish");
    }
  }
  {
    TEST_CASE("a read that answers with no code ends the ring");
    ReferralCodeFetch fetch;
    fetch.Succeed(std::nullopt);
    CheckView(ReferralCodeView::Unavailable, fetch, "no code");
    fetch.Succeed(std::string());
    CheckView(ReferralCodeView::Unavailable, fetch, "empty code");
  }
  {
    TEST_CASE("a code shows");
    ReferralCodeFetch fetch;
    fetch.Succeed(std::string(kTestCode));
    CheckView(ReferralCodeView::Code, fetch, "after success");
    CheckCode(kTestCode, fetch, "code");
  }
  {
    TEST_CASE("a failed background poll keeps the code already shown");
    ReferralCodeFetch fetch;
    fetch.Succeed(std::string(kTestCode));
    fetch.Fail();
    CheckView(ReferralCodeView::Code, fetch, "after a later failure");
    CheckCode(kTestCode, fetch, "code kept");
  }
  {
    TEST_CASE("try again shows the ring until the answer lands");
    ReferralCodeFetch fetch;
    fetch.Fail();
    fetch.Retry();
    CheckView(ReferralCodeView::Loading, fetch, "retrying");
    fetch.Fail();
    CheckView(ReferralCodeView::Unavailable, fetch, "retry failed");
    fetch.Retry();
    fetch.Succeed(std::string(kTestCode));
    CheckView(ReferralCodeView::Code, fetch, "retry succeeded");
  }
  {
    TEST_CASE("try again keeps a code already shown");
    ReferralCodeFetch fetch;
    fetch.Succeed(std::string(kTestCode));
    fetch.Retry();
    CheckView(ReferralCodeView::Code, fetch, "retrying with a code");
  }
  {
    TEST_CASE("logout forgets the code and the answer");
    ReferralCodeFetch fetch;
    fetch.Succeed(std::string(kTestCode));
    fetch.Reset();
    CheckView(ReferralCodeView::Loading, fetch, "after reset");
    CheckCode("<none>", fetch, "code after reset");
  }

  std::cout << gCases << " cases, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
