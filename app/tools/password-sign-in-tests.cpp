// Executable spec for Change password (Common/PasswordSignIn.h): only an
// account with an email or phone password sign-in is offered the reset link.
//
//   c++ -std=c++20 -I ../src/Common password-sign-in-tests.cpp -o /tmp/password-sign-in-tests && /tmp/password-sign-in-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>

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

void TestSsoOnlyAccountHasNoReset() {
  const std::optional<std::string> email = "user@example.com";
  Check(PasswordResetUserAuth(email, "google", std::vector<std::string>{"google"}).empty(),
        "a Google-only account is not offered a password reset");
  Check(PasswordResetUserAuth(email, "apple", std::vector<std::string>{"apple"}).empty(),
        "an Apple-only account is not offered a password reset");
}

void TestPasswordAccountHasReset() {
  const std::optional<std::string> email = "user@example.com";
  Check(PasswordResetUserAuth(email, "password", std::vector<std::string>{"email", "google"}) ==
            "user@example.com",
        "an email password account is offered the reset at its address");
  Check(PasswordResetUserAuth(std::string("+15555550100"), "password",
                              std::vector<std::string>{"phone"}) == "+15555550100",
        "a phone password account is offered the reset");
}

void TestOldServerShape() {
  const std::optional<std::string> email = "user@example.com";
  Check(PasswordResetUserAuth(email, "password", std::nullopt) == "user@example.com",
        "with no auth_types, a password auth_type is offered the reset");
  Check(PasswordResetUserAuth(email, "google", std::vector<std::string>{}).empty(),
        "with an empty auth_types, a google auth_type is not");
}

void TestNoAddress() {
  Check(PasswordResetUserAuth(std::nullopt, "solana", std::vector<std::string>{"solana", "email"})
            .empty(),
        "without an address there is nothing to mail the reset to");
}

}  // namespace

int main() {
  TestSsoOnlyAccountHasNoReset();
  TestPasswordAccountHasReset();
  TestOldServerShape();
  TestNoAddress();
  std::cout << (gCases - gFailures) << "/" << gCases << " password sign-in checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
