// Executable spec for the Manage Subscription row's visibility
// (App/ManageSubscription.h): the Stripe customer portal shows only for a
// Stripe subscription, never for a free network or a store subscription
// (UPGRADE.md D7).
//
//   c++ -std=c++20 -I ../src/App manage-subscription-tests.cpp -o /tmp/manage-subscription-tests && /tmp/manage-subscription-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>

#include "ManageSubscription.h"

using urnw::ShowsManageSubscription;

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

}  // namespace

int main() {
  // the families urnet::classifySubscriptionStore returns
  Check(ShowsManageSubscription("stripe"), "a Stripe subscription opens the portal");
  Check(!ShowsManageSubscription(""), "a free network has nothing to manage");
  Check(!ShowsManageSubscription("apple"), "an App Store subscription is managed in the App Store");
  Check(!ShowsManageSubscription("google"), "a Play subscription is managed in Play");
  Check(!ShowsManageSubscription("other"), "a crypto rail has no portal");
  std::cout << (gCases - gFailures) << "/" << gCases << " manage subscription checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
