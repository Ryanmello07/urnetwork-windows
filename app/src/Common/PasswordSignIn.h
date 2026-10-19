// Whether the account can change its password, decided pure so
// tools/password-sign-in-tests.cpp can pin it on any host against the header
// the app compiles.
//
// Change password mails a reset link, and the server's reset only rewrites an
// email or phone password sign-in. The account's user_auth is the identity it
// was created with: a Google or Apple account has one but no password, so the
// reset "succeeded" and changed nothing, and an account that added an email
// and password later has none, so it was never offered the reset.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace urnw::account {

// true when the account signs in with a password: the server's auth_types
// list holds "email" or "phone", or, from a server that sends no list, the
// single auth_type is "password"
inline bool HasPasswordSignIn(const std::string& authType,
                              const std::optional<std::vector<std::string>>& authTypes) {
  if (authTypes && std::any_of(authTypes->begin(), authTypes->end(),
                               [](const std::string& type) { return !type.empty(); })) {
    return std::any_of(authTypes->begin(), authTypes->end(), [](const std::string& type) {
      return type == "email" || type == "phone";
    });
  }
  return authType == "password";
}

// The address a password reset link goes to, or "" when Change password is
// not offered.
inline std::string PasswordResetUserAuth(const std::optional<std::string>& userAuth,
                                         const std::string& authType,
                                         const std::optional<std::vector<std::string>>& authTypes) {
  if (!userAuth || userAuth->empty() || !HasPasswordSignIn(authType, authTypes)) return {};
  return *userAuth;
}

}  // namespace urnw::account
