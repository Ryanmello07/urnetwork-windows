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
using Clock = urnw::ResendCooldown::Clock;

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
  // what each code send answers
  VerifySendNotice sendNotice;

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

size_t CountSends(const std::vector<std::string>& calls) {
  size_t count = 0;
  for (auto const& call : calls) {
    if (call.rfind("sendCode ", 0) == 0) ++count;
  }
  return count;
}

// An added email or phone is added only once its code is verified: until then
// the flow is not Done, whatever the code send answered, so Settings'
// AddAuthSheet (which reports added on Done) cannot report it.
void AddedSignInIsNotDoneUntilVerified() {
  FakeSession session;
  const Clock::time_point start{};
  GuestConversion conversion(session, [start] { return start; });
  conversion.SubmitSignIn("user@example.com", "correct horse battery");
  session.AnswerAdd();
  Expect("added: code step", conversion.Step() == GuestConversionStep::EnterCode);
  Expect("added: one code sent", CountSends(session.calls) == 1);
  session.verifyError = "Invalid code.";
  conversion.SubmitCode("000000");
  Expect("added: wrong code is not done", conversion.Step() == GuestConversionStep::EnterCode);
  session.verifyError.clear();
  conversion.SubmitCode("123456");
  Expect("added: done once verified", conversion.Step() == GuestConversionStep::Done);
}

// After a rate-limited send, Resend sends nothing until the server's retry
// time has passed on the injected clock; the notice counts the minutes down,
// then clears.
void ResendWaitsOutTheRateLimit() {
  Clock::time_point now{std::chrono::seconds(1000)};
  FakeSession session;
  session.sendNotice = urnw::VerifySendNoticeFor(false, std::string(urnw::kVerifySendErrorCodeRateLimited),
                                                 "rate limited", 120);
  GuestConversion conversion(session, [&now] { return now; });
  conversion.SubmitSignIn("user@example.com", "correct horse battery");
  session.AnswerAdd();
  Expect("rate limit: one send", CountSends(session.calls) == 1);
  Expect("rate limit: cooling down", conversion.CoolingDown());
  Expect("rate limit: no resend", !conversion.CanResend());
  auto shown = conversion.ShownNotice();
  Expect("rate limit: 2 minutes",
         shown && shown->kind == VerifySendNoticeKind::RateLimited && shown->minutes == 2);

  conversion.Resend();  // during the cooldown: refused
  Expect("rate limit: resend refused", CountSends(session.calls) == 1);

  now += std::chrono::seconds(61);
  shown = conversion.ShownNotice();
  Expect("rate limit: 1 minute left", shown && shown->minutes == 1);
  conversion.Resend();
  Expect("rate limit: still refused", CountSends(session.calls) == 1);

  now = Clock::time_point{std::chrono::seconds(1120)};  // the retry time
  Expect("rate limit: passed", !conversion.CoolingDown() && conversion.CanResend());
  Expect("rate limit: notice cleared", !conversion.ShownNotice().has_value());
  session.sendNotice = VerifySendNotice{};
  conversion.Resend();
  Expect("rate limit: resend after the retry time", CountSends(session.calls) == 2);
  shown = conversion.ShownNotice();
  Expect("rate limit: then sent", shown && shown->kind == VerifySendNoticeKind::Sent);
}

// A failed send or a server message is shown as is, and Resend stays
// available (no cooldown).
void SendErrorsAreShownAndResendStaysAvailable() {
  FakeSession session;
  session.sendNotice = urnw::VerifySendNoticeFor(
      false, std::string(urnw::kVerifySendErrorCodeSendFailed), std::string(), 0);
  const Clock::time_point start{};
  GuestConversion conversion(session, [start] { return start; });
  conversion.SubmitSignIn("user@example.com", "correct horse battery");
  session.AnswerAdd();
  auto shown = conversion.ShownNotice();
  Expect("send failed: shown", shown && shown->kind == VerifySendNoticeKind::SendFailed);
  Expect("send failed: resend available", conversion.CanResend());

  session.sendNotice = urnw::VerifySendNoticeFor(false, "user_auth_invalid", "Invalid phone.", 0);
  conversion.Resend();
  shown = conversion.ShownNotice();
  Expect("server message: shown", shown && shown->kind == VerifySendNoticeKind::ServerMessage &&
                                      shown->message == "Invalid phone.");

  session.sendNotice = urnw::VerifySendNoticeFor(true, std::string(), std::string(), 0);
  conversion.Resend();
  shown = conversion.ShownNotice();
  Expect("transport: send failed", shown && shown->kind == VerifySendNoticeKind::SendFailed);
  Expect("send errors: three sends", CountSends(session.calls) == 3);
  Expect("send errors: still on the code step",
         conversion.Step() == GuestConversionStep::EnterCode);
}

// Reopening the sheet drops a running cooldown with the rest of the state.
void ResetClearsTheCooldown() {
  FakeSession session;
  session.sendNotice = urnw::VerifySendNoticeFor(
      false, std::string(urnw::kVerifySendErrorCodeRateLimited), std::string(), 600);
  const Clock::time_point start{};
  GuestConversion conversion(session, [start] { return start; });
  conversion.SubmitSignIn("user@example.com", "correct horse battery");
  session.AnswerAdd();
  Expect("reset: cooling down first", conversion.CoolingDown());
  conversion.Reset();
  Expect("reset: cooldown cleared", !conversion.CoolingDown());
  Expect("reset: notice cleared", !conversion.ShownNotice().has_value());
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
  AddedSignInIsNotDoneUntilVerified();
  ResendWaitsOutTheRateLimit();
  SendErrorsAreShownAndResendStaysAvailable();
  ResetClearsTheCooldown();
  PurchaseContinuesAfterTheConversion();
  CancelledConversionDoesNotContinue();
  std::cout << (gCases - gFailures) << "/" << gCases << " guest conversion cases passed\n";
  return gFailures == 0 ? 0 : 1;
}
