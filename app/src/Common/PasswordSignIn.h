// Whether the account can change its password, and at which address, decided
// pure so tools/password-sign-in-tests.cpp can pin it on any host against the
// header the app compiles.
//
// Change password mails a reset link, and the server's reset rewrites the
// account's email and phone password sign-ins, the server's user_auths. The
// account's user_auth is only the identity it was created with: a Google or
// Apple account has one but no password, so the reset "succeeded" and changed
// nothing, and an account that added an email and password later has none, so
// it was never offered the reset (inbox 7962, 7969). The link goes to a
// password sign-in's address; user_auth is read only when the sdk or server
// lists none.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace urnw::account {

// An email or phone password sign-in of the account: one entry of the
// server's user_auths, as the sdk's urnet::NetworkUserUserAuth carries it.
struct PasswordSignIn {
  std::string userAuth;
  // "email" or "phone"
  std::string authType;
};

// The password sign-ins of the sdk's user_auths (urnet::NetworkUserUserAuthList),
// in order, a missing address as empty. Templated on the list so the tests
// need no sdk header.
template <typename UserAuthList>
std::vector<PasswordSignIn> PasswordSignIns(const std::optional<UserAuthList>& userAuths) {
  std::vector<PasswordSignIn> signIns;
  if (!userAuths) return signIns;
  for (const auto& userAuth : *userAuths) {
    signIns.push_back(PasswordSignIn{userAuth.user_auth.value_or(std::string()), userAuth.auth_type});
  }
  return signIns;
}

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
// not offered: a password sign-in's address, an email before a phone, or,
// when there are none listed, user_auth if the account has a password sign-in.
inline std::string PasswordResetUserAuth(const std::optional<std::string>& userAuth,
                                         const std::string& authType,
                                         const std::optional<std::vector<std::string>>& authTypes,
                                         const std::vector<PasswordSignIn>& userAuths) {
  for (const std::string passwordAuthType : {"email", "phone"}) {
    for (const PasswordSignIn& signIn : userAuths) {
      if (signIn.authType == passwordAuthType && !signIn.userAuth.empty()) return signIn.userAuth;
    }
  }
  if (!userAuth || userAuth->empty() || !HasPasswordSignIn(authType, authTypes)) return {};
  return *userAuth;
}

}  // namespace urnw::account
