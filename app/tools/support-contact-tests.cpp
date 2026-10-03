// Executable spec for where Settings sends a user who wants to reach the
// project (App/SupportContact.h): the support address is offered wherever the
// Discord invite is, since Discord is unreachable in some regions, and its row
// links the address with mailto - run against the SAME header the app
// compiles, on any host with a C++20 compiler.
//
//   c++ -std=c++20 -I ../src/App support-contact-tests.cpp \
//       -o /tmp/support-contact-tests && /tmp/support-contact-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <iostream>
#include <string>

#include "SupportContact.h"

using namespace urnw::support;

namespace {

int gFailures = 0;
int gCases = 0;
std::string gCurrentCase;

void Fail(const std::string& message) {
  ++gFailures;
  std::cout << "  FAIL [" << gCurrentCase << "] " << message << "\n";
}

void Check(bool condition, const std::string& message) {
  if (!condition) Fail(message);
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
    TEST_CASE("stay in touch offers the support address beside Discord");
    Check(OffersSupportEmailWithDiscord(kStayInTouchLinks),
          "kStayInTouchLinks offers Discord without the support address");
    const auto discord =
        std::find(kStayInTouchLinks.begin(), kStayInTouchLinks.end(), StayInTouchLink::Discord);
    const auto email = std::find(kStayInTouchLinks.begin(), kStayInTouchLinks.end(),
                                 StayInTouchLink::SupportEmail);
    Check(discord != kStayInTouchLinks.end(), "Discord row present");
    Check(email == discord + 1, "the support address sits right under Discord");
  }
  {
    TEST_CASE("Discord alone fails the rule");
    const std::array<StayInTouchLink, 3> discordOnly = {
        StayInTouchLink::Discord, StayInTouchLink::DePinHub, StayInTouchLink::DePinHub};
    Check(!OffersSupportEmailWithDiscord(discordOnly), "Discord without the address passed");
  }
  {
    TEST_CASE("the support row links the address with mailto");
    Check(kSupportEmail == L"support@ur.io", "support address");
    Check(kSupportEmailUrl == L"mailto:support@ur.io", "support url");
    Check(SupportEmailMarkdown(L"Email support at") ==
              L"Email support at [support@ur.io](mailto:support@ur.io)",
          "markdown for the label");
  }

  std::cout << gCases << " cases, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
