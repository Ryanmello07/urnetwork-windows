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

#include <chrono>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "GuestConversion.h"

using urnw::GuestConversion;
using urnw::GuestConversionStep;
using urnw::VerifySendNotice;
using urnw::VerifySendNoticeKind;

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
  // what every code send answers
  VerifySendNotice sendNotice;
  // the injected clock
  urnw::ResendCooldown::Clock::time_point now{};
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
    done(sendNotice);
  }
  void VerifyCode(const std::string& userAuth, const std::string& code,
                  std::function<void(std::string)> done) override {
    calls.push_back("verify " + userAuth + " " + code);
    done(verifyError);
  }
  urnw::ResendCooldown::Clock::time_point Now() override { return now; }
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

VerifySendNotice RateLimited(int64_t retryAfterSeconds) {
  return urnw::VerifySendNoticeFor(false, "verify_rate_limited", "Too many attempts.",
                                   retryAfterSeconds);
}

size_t SendCount(FakeSession const& session) {
  size_t count = 0;
  for (auto const& call : session.calls) {
    if (call.rfind("sendCode ", 0) == 0) ++count;
  }
  return count;
}

// The login verify step holds Resend off for a rate limit's retry time and
// counts the minutes down; the conversion's code step did neither, so Resend
// asked again at once and the notice kept its first minute count.
void RateLimitHoldsResendUntilItsRetryTime() {
  FakeSession session;
  GuestConversion conversion(session);
  session.sendNotice = RateLimited(150);
  conversion.SubmitSignIn("guest@example.com", "correct horse battery");
  session.AnswerAdd();
  Expect("rate limit: code step", conversion.Step() == GuestConversionStep::EnterCode);
  Expect("rate limit: one send", SendCount(session) == 1);
  conversion.Resend();
  Expect("rate limit: Resend sends no second code", SendCount(session) == 1);
  Expect("rate limit: Resend off", !conversion.CanResend());
  Expect("rate limit: cooling down", conversion.CoolingDown());
  auto notice = conversion.Notice();
  Expect("rate limit: notice", notice && notice->kind == VerifySendNoticeKind::RateLimited &&
                                   notice->minutes == 3);
  session.now += std::chrono::seconds(100);
  notice = conversion.Notice();
  Expect("rate limit: minutes count down", notice && notice->minutes == 1);
  conversion.Resend();
  Expect("rate limit: still held", SendCount(session) == 1);
  session.now += std::chrono::seconds(50);
  Expect("rate limit passed: Resend back", conversion.CanResend());
  Expect("rate limit passed: not cooling down", !conversion.CoolingDown());
  Expect("rate limit passed: notice gone", !conversion.Notice());
  session.sendNotice = VerifySendNotice{};
  conversion.Resend();
  Expect("rate limit passed: Resend sends", SendCount(session) == 2);
  notice = conversion.Notice();
  Expect("rate limit passed: sent", notice && notice->kind == VerifySendNoticeKind::Sent);
}

// A plain send failure keeps Resend usable, and reopening the sheet drops a
// running rate limit.
void SendFailureAndResetDoNotHoldResend() {
  FakeSession session;
  GuestConversion conversion(session);
  session.sendNotice = urnw::VerifySendNoticeFor(false, "verify_send_failed", "", 0);
  conversion.SubmitSignIn("guest@example.com", "correct horse battery");
  session.AnswerAdd();
  Expect("send failed: Resend usable", conversion.CanResend());
  session.sendNotice = RateLimited(600);
  conversion.Resend();
  Expect("rate limited: Resend off", !conversion.CanResend());
  conversion.Reset();
  Expect("reset: no cooldown", !conversion.CoolingDown());
  Expect("reset: no notice", !conversion.Notice());
}

// A purchase entry that sent a guest to the conversion continues to its
// checkout once the conversion is done and the guest clears; it used to close
// back to where the user started.
void PurchaseContinuesAfterTheConversion() {
  urnw::GuestUpgradeContinuation continuation;
  int checkouts = 0;
  continuation.Divert([&checkouts] { ++checkouts; });
  continuation.Poll(/*isGuest=*/false);
  Expect("continuation: not before the conversion is done", checkouts == 0);
  continuation.ConversionDone();
  continuation.ConversionClosed();
  continuation.Poll(/*isGuest=*/true);  // the balance re-read is in flight
  Expect("continuation: waits for the guest to clear", checkouts == 0);
  continuation.Poll(/*isGuest=*/false);
  Expect("continuation: opens the checkout", checkouts == 1);
  continuation.Poll(false);
  Expect("continuation: once", checkouts == 1);
  Expect("continuation: spent", !continuation.Pending());
}

void CancelledConversionDoesNotContinue() {
  urnw::GuestUpgradeContinuation continuation;
  int checkouts = 0;
  continuation.Divert([&checkouts] { ++checkouts; });
  continuation.ConversionClosed();  // closed without adding a sign-in
  continuation.Poll(false);
  Expect("cancelled: no checkout", checkouts == 0);
  Expect("cancelled: dropped", !continuation.Pending());
  // a plain "Create an account" (no purchase) has nothing to continue
  continuation.ConversionDone();
  continuation.Poll(false);
  Expect("no entry: no checkout", checkouts == 0);
}

}  // namespace

int main() {
  RefreshedLegacyGuestIsStillAGuest();
  ConversionAddsAndVerifiesOnTheSameNetwork();
  AddFailureStaysOnTheSignInStep();
  WrongCodeStaysOnTheCodeStep();
  ShortPasswordIsNotSubmitted();
  AnswerAfterResetIsDropped();
  RateLimitHoldsResendUntilItsRetryTime();
  SendFailureAndResetDoNotHoldResend();
  PurchaseContinuesAfterTheConversion();
  CancelledConversionDoesNotContinue();
  std::cout << (gCases - gFailures) << "/" << gCases << " guest conversion cases passed\n";
  return gFailures == 0 ? 0 : 1;
}
