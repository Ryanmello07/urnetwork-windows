// Executable spec for the referral invitation the share actions copy
// (App/ReferralShare.h, support inbox 1698): the localized message, then the
// code's ur.io/c link on its own line, which opens the Android app (or Play
// with the install referrer) and web signup - run against the SAME header the
// app compiles, on any host with a C++20 compiler.
//
//   c++ -std=c++20 -I ../src/App referral-share-tests.cpp \
//       -o /tmp/referral-share-tests && /tmp/referral-share-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>

#include "ReferralShare.h"

using urnw::ReferralLinkUrl;
using urnw::ReferralShareText;

namespace {

int gFailures = 0;
int gCases = 0;
std::string gCurrentCase;

void Fail(const std::string& message) {
  ++gFailures;
  std::cout << "  FAIL [" << gCurrentCase << "] " << message << "\n";
}

void CheckEqual(const std::string& expected, const std::string& actual, const std::string& what) {
  if (expected != actual) Fail(what + ": expected \"" + expected + "\", got \"" + actual + "\"");
}

void CheckEqual(const std::wstring& expected, const std::wstring& actual, const std::string& what) {
  if (expected != actual) Fail(what + ": the wide strings differ");
}

struct Case {
  explicit Case(const char* name) {
    gCurrentCase = name;
    ++gCases;
  }
};
#define TEST_CASE(name) Case case_##__LINE__(name)

constexpr const wchar_t* kMessage =
    L"Join me on URnetwork! Get the app and enter referral code AB12CD when you sign up.";

}  // namespace

int main() {
  {
    TEST_CASE("the link is the code's ur.io/c link");
    CheckEqual("https://ur.io/c?bonus=AB12CD", ReferralLinkUrl("ur.io", "AB12CD"), "link");
    CheckEqual("https://ur.io/c?bonus=9f1c-22ab", ReferralLinkUrl("ur.io", "9f1c-22ab"),
               "an older, longer code");
  }
  {
    TEST_CASE("no code, no link");
    CheckEqual("", ReferralLinkUrl("ur.io", ""), "empty code");
  }
  {
    TEST_CASE("a code never adds a parameter to the link");
    CheckEqual("https://ur.io/c?bonus=A%26auth_code%3Dx", ReferralLinkUrl("ur.io", "A&auth_code=x"),
               "encoded");
  }
  {
    TEST_CASE("the invitation is the message, then the link on its own line");
    CheckEqual(std::wstring(kMessage) + L"\nhttps://ur.io/c?bonus=AB12CD",
               ReferralShareText(kMessage, L"https://ur.io/c?bonus=AB12CD"), "share text");
  }
  {
    TEST_CASE("without a link the message stands alone");
    CheckEqual(std::wstring(kMessage), ReferralShareText(kMessage, L""), "no link");
  }

  std::cout << gCases << " cases, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
