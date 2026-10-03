// Executable spec for the delete-account outcome (Common/DeleteAccountOutcome.h):
// a deletion the server refuses (HTTP 200 with an error in the result) keeps
// the user signed in and shows the generic error with the server's reason;
// only a result with no error signs out. Run against the SAME header the app
// compiles, on any host with a C++20 compiler.
//
//   c++ -std=c++20 -I ../src/Common delete-account-tests.cpp \
//       -o /tmp/delete-account-tests && /tmp/delete-account-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>

#include "DeleteAccountOutcome.h"

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

const std::string kGeneric = "Sorry, there was an error deleting your account.";

void TestServerRefusalKeepsSession() {
  const std::string reason = "Could not cancel your Google Play subscription. Please try again.";
  auto outcome = DecideDeleteAccount(nullptr, true, true, reason);
  Check(!outcome.deleted, "a result with a server error is not a deletion (no sign out)");
  Check(outcome.detail == reason, "a server error carries the server's message");
  Check(DeleteAccountErrorText(kGeneric, outcome.detail) == kGeneric + "\n" + reason,
        "the server's message follows the generic error on its own line");
}

void TestServerRefusalWithoutMessage() {
  auto outcome = DecideDeleteAccount(nullptr, true, true, "  ");
  Check(!outcome.deleted, "a server error with a blank message is not a deletion");
  Check(DeleteAccountErrorText(kGeneric, outcome.detail) == kGeneric,
        "a blank server message falls back to the generic error alone");
}

void TestSuccess() {
  auto outcome = DecideDeleteAccount(nullptr, true, false, "");
  Check(outcome.deleted, "a result with no error is a deletion");
}

void TestTransportAndMissingResult() {
  const std::string err = "connection reset";
  auto transport = DecideDeleteAccount(&err, false, false, "");
  Check(!transport.deleted, "a transport error is not a deletion");
  Check(transport.detail == err, "a transport error carries its message");
  auto missing = DecideDeleteAccount(nullptr, false, false, "");
  Check(!missing.deleted, "no result is not a deletion");
  Check(DeleteAccountErrorText(kGeneric, missing.detail) == kGeneric, "no result shows the generic error");
}

}  // namespace

int main() {
  TestServerRefusalKeepsSession();
  TestServerRefusalWithoutMessage();
  TestSuccess();
  TestTransportAndMissingResult();
  std::cout << (gCases - gFailures) << "/" << gCases << " delete-account checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
