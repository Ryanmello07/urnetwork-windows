// Executable spec for Change password (Common/PasswordSignIn.h): only an
// account with an email or phone password sign-in is offered the reset link,
// at that sign-in's address (inbox 7962, 7969).
//
//   c++ -std=c++20 -I ../src/Common password-sign-in-tests.cpp -o /tmp/password-sign-in-tests && /tmp/password-sign-in-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "PasswordSignIn.h"

using namespace urnw::account;

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

// the shape of the sdk's urnet::NetworkUserUserAuth (urnetwork_sdk.hpp)
struct SdkUserAuth {
  std::optional<std::string> user_auth;
  std::string auth_type{};
};

const std::vector<PasswordSignIn> kNoSignIns = {};

void TestSsoOnlyAccountHasNoReset() {
  const std::optional<std::string> email = "user@example.com";
  Check(PasswordResetUserAuth(email, "google", std::vector<std::string>{"google"}, kNoSignIns).empty(),
        "a Google-only account is not offered a password reset");
  Check(PasswordResetUserAuth(email, "apple", std::vector<std::string>{"apple"}, kNoSignIns).empty(),
        "an Apple-only account is not offered a password reset");
}

void TestPasswordAccountHasReset() {
  const std::optional<std::string> email = "user@example.com";
  Check(PasswordResetUserAuth(email, "password", std::vector<std::string>{"email"},
                              {PasswordSignIn{"user@example.com", "email"}}) == "user@example.com",
        "an email password account is offered the reset at its address");
  Check(PasswordResetUserAuth(std::string("+15555550100"), "password",
                              std::vector<std::string>{"phone"},
                              {PasswordSignIn{"+15555550100", "phone"}}) == "+15555550100",
        "a phone password account is offered the reset");
}

// An account created with a wallet, a seed phrase or as a guest that added an
// email and password later has no user_auth: it was never offered the reset.
void TestAccountThatAddedAnEmail() {
  const std::vector<PasswordSignIn> added = {PasswordSignIn{"added@example.com", "email"}};
  Check(PasswordResetUserAuth(std::nullopt, "solana", std::vector<std::string>{"email", "solana"},
                              added) == "added@example.com",
        "a wallet account that added an email is offered the reset at that email");
  Check(PasswordResetUserAuth(std::string(), "seedphrase",
                              std::vector<std::string>{"seedphrase", "email"},
                              added) == "added@example.com",
        "a seed phrase account that added an email is offered the reset at that email");
}

// A Google or Apple account that added an email and password is offered the
// reset at the added email, not at the provider's.
void TestSsoAccountThatAddedAnEmail() {
  Check(PasswordResetUserAuth(std::string("provider@example.com"), "google",
                              std::vector<std::string>{"google", "email"},
                              {PasswordSignIn{"added@example.com", "email"}}) == "added@example.com",
        "a Google account that added an email is offered the reset at the added email");
}

void TestEmailBeforePhone() {
  Check(PasswordResetUserAuth(std::string("+15555550100"), "password",
                              std::vector<std::string>{"phone", "email"},
                              {PasswordSignIn{"+15555550100", "phone"},
                               PasswordSignIn{"user@example.com", "email"}}) == "user@example.com",
        "the email password sign-in is preferred over the phone");
}

// The sdk's user_auths, absent or listed, become the password sign-ins.
void TestPasswordSignInsFromTheSdk() {
  Check(PasswordSignIns(std::optional<std::vector<SdkUserAuth>>()).empty(),
        "an sdk or server without user_auths lists no password sign-in");
  const std::vector<PasswordSignIn> signIns = PasswordSignIns(std::optional<std::vector<SdkUserAuth>>(
      std::vector<SdkUserAuth>{SdkUserAuth{std::string("added@example.com"), "email"},
                               SdkUserAuth{std::nullopt, "phone"}}));
  Check(signIns.size() == 2 && signIns[0].userAuth == "added@example.com" &&
            signIns[0].authType == "email" && signIns[1].userAuth.empty() &&
            signIns[1].authType == "phone",
        "the sdk's user_auths are listed in order, a missing address as empty");
}

void TestOldServerShape() {
  const std::optional<std::string> email = "user@example.com";
  Check(PasswordResetUserAuth(email, "password", std::nullopt, kNoSignIns) == "user@example.com",
        "with no auth_types, a password auth_type is offered the reset");
  Check(PasswordResetUserAuth(email, "google", std::vector<std::string>{}, kNoSignIns).empty(),
        "with an empty auth_types, a google auth_type is not");
  Check(PasswordResetUserAuth(email, "password", std::vector<std::string>{"email", "google"},
                              kNoSignIns) == "user@example.com",
        "without user_auths, the identity the account was created with is used");
}

void TestNoAddress() {
  Check(PasswordResetUserAuth(std::nullopt, "solana", std::vector<std::string>{"solana", "email"},
                              kNoSignIns)
            .empty(),
        "without an address there is nothing to mail the reset to");
  Check(PasswordResetUserAuth(std::nullopt, "solana", std::vector<std::string>{"email"},
                              {PasswordSignIn{"", "email"}})
            .empty(),
        "a password sign-in without an address is not one to mail");
}

}  // namespace

int main() {
  TestSsoOnlyAccountHasNoReset();
  TestPasswordAccountHasReset();
  TestAccountThatAddedAnEmail();
  TestSsoAccountThatAddedAnEmail();
  TestEmailBeforePhone();
  TestPasswordSignInsFromTheSdk();
  TestOldServerShape();
  TestNoAddress();
  std::cout << (gCases - gFailures) << "/" << gCases << " password sign-in checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
