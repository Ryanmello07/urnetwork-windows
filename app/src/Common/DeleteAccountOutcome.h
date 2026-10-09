// What a finished Api::networkDelete call means for the session.
//
// The server answers a refused deletion with HTTP 200 and an error in the
// result (a failed Stripe or Google Play cancellation, or an App Store
// subscription that still renews, with a message for the user). The account
// still exists then, so the app must stay signed in, keep the sheet open for a
// retry and say why. Only a result with no error and no transport error is a
// deletion; signing out after anything else strands the user outside an
// account that is still there.
//
// Pure, with no Windows or SDK headers, so tools/delete-account-tests.cpp runs
// it on any host with a C++20 compiler.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>

namespace urnw::account {

struct DeleteAccountOutcome {
  // the account is gone: sign out
  bool deleted = false;
  // the reason to show under the generic error, or empty
  std::string detail;
};

// transportError: the call failed before a result (nullptr when it did not).
// hasResult: a result was parsed. serverError: the result carries an error.
inline DeleteAccountOutcome DecideDeleteAccount(std::string const* transportError, bool hasResult,
                                                bool serverError,
                                                std::string const& serverMessage) {
  if (transportError) return {false, *transportError};
  if (!hasResult) return {false, std::string()};
  if (serverError) return {false, serverMessage};
  return {true, std::string()};
}

// The generic error, with the reason on its own line when there is one.
inline std::string DeleteAccountErrorText(std::string const& generic, std::string const& detail) {
  auto first = detail.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return generic;
  auto last = detail.find_last_not_of(" \t\r\n");
  return generic + "\n" + detail.substr(first, last - first + 1);
}

}  // namespace urnw::account
