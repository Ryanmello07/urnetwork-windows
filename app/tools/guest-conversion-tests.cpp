// Executable spec for the legacy guest conversion (App/GuestConversion.h,
// UPGRADE.md A4/D8): a refreshed guest is still a guest, and "Create an
// account" adds a sign-in to the guest's OWN network and verifies it, keeping
// its plan and balance. Run against the SAME header the app compiles, on any
// host with a C++20 compiler, with a fake session that answers synchronously.
//
//   c++ -std=c++20 -I ../src/App guest-conversion-tests.cpp \
//       -o /tmp/guest-conversion-tests && /tmp/guest-conversion-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "GuestConversion.h"

using urnw::GuestConversion;
using urnw::GuestConversionStep;
using urnw::VerifySendNotice;

namespace {

int gFailures = 0;
int gCases = 0;

void Expect(const char* name, bool ok) {
  ++gCases;
  if (!ok) {
    ++gFailures;
    std::cerr << "FAIL " << name << "\n";
  }
}

// Records every call; the addAuth answer is held until AnswerAdd.
class FakeSession : public urnw::GuestConversionSession {
 public:
  std::vector<std::string> calls;
  std::string addError;
  std::string verifyError;
  std::function<void(std::string)> pendingAdd;

  void AddSignIn(const std::string& userAuth, const std::string& password,
                 std::function<void(std::string)> done) override {
    calls.push_back("addAuth " + userAuth + " " + password);
    pendingAdd = std::move(done);
  }
  void RefreshJwt() override { calls.push_back("refreshJwt"); }
  void RefreshBalance() override { calls.push_back("refreshBalance"); }
  void SendCode(const std::string& userAuth, std::function<void(VerifySendNotice)> done) override {
    calls.push_back("sendCode " + userAuth);
    done(VerifySendNotice{});
  }
  void VerifyCode(const std::string& userAuth, const std::string& code,
                  std::function<void(std::string)> done) override {
    calls.push_back("verify " + userAuth + " " + code);
    done(verifyError);
  }
  void AnswerAdd() {
    auto done = std::move(pendingAdd);
    pendingAdd = nullptr;
    if (done) done(addError);
  }
};

void RefreshedLegacyGuestIsStillAGuest() {
  // the refreshed jwt lost the claim; the server still reports no login method
  Expect("refreshed guest (claim false, server guest)",
         urnw::IsGuestNetwork(/*guestModeClaim=*/false, /*serverGuest=*/true));
  // a claim not yet re-signed still counts (older server: no `guest`)
  Expect("guest claim, no server guest", urnw::IsGuestNetwork(true, false));
  Expect("neither is not a guest", !urnw::IsGuestNetwork(false, false));
}

void ConversionAddsAndVerifiesOnTheSameNetwork() {
  FakeSession session;
  GuestConversion conversion(session);
  conversion.SubmitSignIn("  guest@example.com ", "correct horse battery");
  Expect("adding", conversion.Step() == GuestConversionStep::AddingSignIn);
  session.AnswerAdd();
  Expect("code step", conversion.Step() == GuestConversionStep::EnterCode);
  conversion.SubmitCode(" 123456 ");
  Expect("done", conversion.Step() == GuestConversionStep::Done);
  const std::vector<std::string> expected = {
      "addAuth guest@example.com correct horse battery",
      // the network has a login method now: drop the guest state at once
      "refreshJwt",
      "refreshBalance",
      "sendCode guest@example.com",
      "verify guest@example.com 123456",
      "refreshBalance",
  };
  Expect("add, refresh, send, verify on this network; no sign-out", session.calls == expected);
  if (session.calls != expected) {
    for (auto const& call : session.calls) std::cerr << "  call: " << call << "\n";
  }
}

void AddFailureStaysOnTheSignInStep() {
  FakeSession session;
  GuestConversion conversion(session);
  session.addError = "User auth already exists.";
  conversion.SubmitSignIn("guest@example.com", "correct horse battery");
  session.AnswerAdd();
  Expect("add failure: sign-in step", conversion.Step() == GuestConversionStep::EnterSignIn);
  Expect("add failure: message", conversion.Error() == "User auth already exists.");
  Expect("add failure: no refresh, no code", session.calls.size() == 1);
}

void WrongCodeStaysOnTheCodeStep() {
  FakeSession session;
  GuestConversion conversion(session);
  conversion.SubmitSignIn("guest@example.com", "correct horse battery");
  session.AnswerAdd();
  session.verifyError = "Invalid code.";
  conversion.SubmitCode("000000");
  Expect("wrong code: code step", conversion.Step() == GuestConversionStep::EnterCode);
  Expect("wrong code: message", conversion.Error() == "Invalid code.");
}

void ShortPasswordIsNotSubmitted() {
  FakeSession session;
  GuestConversion conversion(session);
  conversion.SubmitSignIn("guest@example.com", "short");
  conversion.SubmitSignIn("   ", "correct horse battery");
  Expect("invalid sign-in: nothing sent", session.calls.empty());
}

void AnswerAfterResetIsDropped() {
  FakeSession session;
  GuestConversion conversion(session);
  conversion.SubmitSignIn("guest@example.com", "correct horse battery");
  conversion.Reset();  // the sheet was closed and reopened
  session.AnswerAdd();
  Expect("late answer: still on sign-in", conversion.Step() == GuestConversionStep::EnterSignIn);
  Expect("late answer: nothing after it", session.calls.size() == 1);
}

}  // namespace

int main() {
  RefreshedLegacyGuestIsStillAGuest();
  ConversionAddsAndVerifiesOnTheSameNetwork();
  AddFailureStaysOnTheSignInStep();
  WrongCodeStaysOnTheCodeStep();
  ShortPasswordIsNotSubmitted();
  AnswerAfterResetIsDropped();
  std::cout << (gCases - gFailures) << "/" << gCases << " guest conversion cases passed\n";
  return gFailures == 0 ? 0 : 1;
}
