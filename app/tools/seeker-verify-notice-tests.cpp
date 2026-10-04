// Executable spec for what the Earnings page says when the Seeker verification
// settles (App/SeekerVerifyNotice.h): a server that checked the wallet and
// found no Seeker or Saga token says so with seeker_token_not_found instead of
// the generic claim error, and a known server error code picks the localized
// notice over the server's English message - run against the SAME header the
// app compiles, on any host with a C++20 compiler.
//
//   c++ -std=c++20 -I ../src/App seeker-verify-notice-tests.cpp -o /tmp/seeker-verify-notice-tests && /tmp/seeker-verify-notice-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>

#include "SeekerVerifyNotice.h"

using urnw::SeekerVerifyNoticeFor;
using urnw::SeekerVerifyNoticeKind;
using urnw::SeekerWalletSuffix;

namespace {

int gFailures = 0;
int gCases = 0;

std::string KindName(SeekerVerifyNoticeKind kind) {
  switch (kind) {
    case SeekerVerifyNoticeKind::Verified: return "Verified";
    case SeekerVerifyNoticeKind::NotHolder: return "NotHolder";
    case SeekerVerifyNoticeKind::Reason: return "Reason";
    case SeekerVerifyNoticeKind::Failed: return "Failed";
  }
  return "?";
}

void Check(const char* name, SeekerVerifyNoticeKind expected, SeekerVerifyNoticeKind actual) {
  ++gCases;
  if (expected != actual) {
    ++gFailures;
    std::cout << "  FAIL [" << name << "] expected " << KindName(expected) << ", got "
              << KindName(actual) << "\n";
  }
}

void CheckText(const char* name, const std::string& expected, const std::string& actual) {
  ++gCases;
  if (expected != actual) {
    ++gFailures;
    std::cout << "  FAIL [" << name << "] expected \"" << expected << "\", got \"" << actual
              << "\"\n";
  }
}

}  // namespace

int main() {
  // the root cause: success=false with no message is the server's "no token"
  // answer, and it was shown as the generic claim error
  Check("answered not a holder", SeekerVerifyNoticeKind::NotHolder,
        SeekerVerifyNoticeFor(true, false, ""));

  Check("verified", SeekerVerifyNoticeKind::Verified, SeekerVerifyNoticeFor(true, true, ""));
  Check("server message", SeekerVerifyNoticeKind::Reason,
        SeekerVerifyNoticeFor(true, false, "Invalid signature"));
  Check("transport error", SeekerVerifyNoticeKind::Reason,
        SeekerVerifyNoticeFor(false, false, "connection refused"));
  Check("no answer, no reason", SeekerVerifyNoticeKind::Failed,
        SeekerVerifyNoticeFor(false, false, ""));
  // a success that carries an error message is not a verification
  Check("success with error", SeekerVerifyNoticeKind::Reason,
        SeekerVerifyNoticeFor(true, true, "lookup failed"));

  // the server's error code picks the localized notice over its English
  // message, which was shown as is
  Check("code token not found", SeekerVerifyNoticeKind::NotHolder,
        SeekerVerifyNoticeFor(true, false, "No Seeker or Saga token found",
                              "seeker_token_not_found"));
  Check("code invalid signature", SeekerVerifyNoticeKind::Failed,
        SeekerVerifyNoticeFor(true, false, "Invalid signature", "seeker_invalid_signature"));
  Check("code lookup failed", SeekerVerifyNoticeKind::Failed,
        SeekerVerifyNoticeFor(true, false, "Lookup failed", "seeker_lookup_failed"));
  // an unknown or empty code keeps the message, or the not-holder answer
  Check("unknown code", SeekerVerifyNoticeKind::Reason,
        SeekerVerifyNoticeFor(true, false, "Try later", "seeker_new_reason"));
  Check("unknown code, no message", SeekerVerifyNoticeKind::NotHolder,
        SeekerVerifyNoticeFor(true, false, "", "seeker_new_reason"));
  Check("empty code", SeekerVerifyNoticeKind::Reason,
        SeekerVerifyNoticeFor(true, false, "Invalid signature", ""));

  CheckText("wallet suffix", "4QH6Z2D",
            SeekerWalletSuffix("2DMMamkkxQ6zDMBtkFp8KH7FoWzBMBA1CGTYwom4QH6Z2D"));
  CheckText("short wallet", "abc", SeekerWalletSuffix("abc"));

  std::cout << (gCases - gFailures) << "/" << gCases << " seeker verify notice checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
