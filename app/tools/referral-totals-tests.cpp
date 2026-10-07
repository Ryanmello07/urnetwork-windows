// Executable spec for the referral count behind "Total referrals"
// (App/ReferralTotalsState.h): a failed read with no count is an error with
// Try again, not "0"; a failed background read keeps a count already shown;
// Try again goes back to Loading until the answer lands - run against the SAME
// header the app compiles, on any host with a C++20 compiler.
//
//   c++ -std=c++20 -I ../src/App referral-totals-tests.cpp -o /tmp/referral-totals-tests && /tmp/referral-totals-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <iostream>
#include <string>

#include "ReferralTotalsState.h"

using urnw::ReferralTotalsFetch;
using urnw::ReferralTotalsView;

namespace {

int gFailures = 0;
int gCases = 0;
std::string gCurrentCase;

void Fail(const std::string& message) {
  ++gFailures;
  std::cout << "  FAIL [" << gCurrentCase << "] " << message << "\n";
}

std::string ViewName(ReferralTotalsView view) {
  switch (view) {
    case ReferralTotalsView::Loading: return "Loading";
    case ReferralTotalsView::Count: return "Count";
    case ReferralTotalsView::Unavailable: return "Unavailable";
  }
  return "?";
}

void CheckView(ReferralTotalsView expected, const ReferralTotalsFetch& fetch,
               const std::string& what) {
  if (expected != fetch.View()) {
    Fail(what + ": expected " + ViewName(expected) + ", got " + ViewName(fetch.View()));
  }
}

void CheckTotal(int64_t expected, const ReferralTotalsFetch& fetch, const std::string& what) {
  if (expected != fetch.Total()) {
    Fail(what + ": expected " + std::to_string(expected) + ", got " +
         std::to_string(fetch.Total()));
  }
}

struct Case {
  explicit Case(const char* name) {
    gCurrentCase = name;
    ++gCases;
  }
};
#define TEST_CASE(name) Case case_##__LINE__(name)

}  // namespace

int main() {
  {
    TEST_CASE("nothing read yet is Loading, not 0 referrals");
    ReferralTotalsFetch fetch;
    CheckView(ReferralTotalsView::Loading, fetch, "before the first read");
  }
  {
    TEST_CASE("a failed first read is Unavailable, not 0 referrals");
    ReferralTotalsFetch fetch;
    fetch.Fail();
    CheckView(ReferralTotalsView::Unavailable, fetch, "after a failed read");
  }
  {
    TEST_CASE("a read shows the count, including 0");
    ReferralTotalsFetch fetch;
    fetch.Succeed(0);
    CheckView(ReferralTotalsView::Count, fetch, "after a read of 0");
    CheckTotal(0, fetch, "after a read of 0");
    fetch.Succeed(4);
    CheckTotal(4, fetch, "after a read of 4");
  }
  {
    TEST_CASE("a failed background read keeps the count shown");
    ReferralTotalsFetch fetch;
    fetch.Succeed(3);
    fetch.Fail();
    CheckView(ReferralTotalsView::Count, fetch, "after a failed poll");
    CheckTotal(3, fetch, "after a failed poll");
  }
  {
    TEST_CASE("Try again goes back to Loading, then the answer lands");
    ReferralTotalsFetch fetch;
    fetch.Fail();
    fetch.Retry();
    CheckView(ReferralTotalsView::Loading, fetch, "while the retry runs");
    fetch.Succeed(2);
    CheckView(ReferralTotalsView::Count, fetch, "after the retry");
    CheckTotal(2, fetch, "after the retry");
  }
  {
    TEST_CASE("logout drops the previous network's count");
    ReferralTotalsFetch fetch;
    fetch.Succeed(5);
    fetch.Reset();
    CheckView(ReferralTotalsView::Loading, fetch, "after logout");
    CheckTotal(0, fetch, "after logout");
  }

  std::cout << (gCases - gFailures) << "/" << gCases << " referral totals cases passed\n";
  return gFailures == 0 ? 0 : 1;
}
