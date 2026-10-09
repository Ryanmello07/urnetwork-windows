// Executable spec for what the verify step says about the verification code
// (App/VerifySendNotice.h): a code the server reports it did not send (send
// failed, or rate limited) must not read "code sent" - run against the SAME
// header the app compiles, on any host with a C++20 compiler.
//
//   c++ -std=c++20 -I ../src/App verify-send-notice-tests.cpp -o /tmp/verify-send-notice-tests && /tmp/verify-send-notice-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

#include "VerifySendNotice.h"

using urnw::PasswordResetNoticeKey;
using urnw::ResendCooldown;
using urnw::VerifySendNotice;
using urnw::VerifySendNoticeFor;
using urnw::VerifySendNoticeKey;
using urnw::VerifySendNoticeKind;

namespace {

int gFailures = 0;
int gCases = 0;

std::string KindName(VerifySendNoticeKind kind) {
  switch (kind) {
    case VerifySendNoticeKind::Sent: return "Sent";
    case VerifySendNoticeKind::RateLimited: return "RateLimited";
    case VerifySendNoticeKind::SendFailed: return "SendFailed";
    case VerifySendNoticeKind::ServerMessage: return "ServerMessage";
  }
  return "?";
}

void Check(const char* name, VerifySendNoticeKind expected, VerifySendNotice const& actual) {
  ++gCases;
  if (expected != actual.kind) {
    ++gFailures;
    std::cout << "  FAIL [" << name << "] expected " << KindName(expected) << ", got "
              << KindName(actual.kind) << "\n";
  }
}

void CheckValue(const char* name, std::string const& expected, std::string const& actual) {
  ++gCases;
  if (expected != actual) {
    ++gFailures;
    std::cout << "  FAIL [" << name << "] expected \"" << expected << "\", got \"" << actual
              << "\"\n";
  }
}

void CheckInt(const char* name, int64_t expected, int64_t actual) {
  ++gCases;
  if (expected != actual) {
    ++gFailures;
    std::cout << "  FAIL [" << name << "] expected " << expected << ", got " << actual << "\n";
  }
}

void CheckMinutes(const char* name, int64_t expected, VerifySendNotice const& actual) {
  ++gCases;
  if (expected != actual.minutes) {
    ++gFailures;
    std::cout << "  FAIL [" << name << "] expected " << expected << " minutes, got "
              << actual.minutes << "\n";
  }
}

}  // namespace

int main() {
  // the root cause: the server reported a code it did not send, and the verify
  // step said it was sent
  const auto failed = VerifySendNoticeFor(false, "verify_send_failed", "send failed", 0);
  Check("send failed", VerifySendNoticeKind::SendFailed, failed);
  CheckValue("send failed key", "error_sending_verification_code",
             std::string(VerifySendNoticeKey(failed)));

  const auto limited = VerifySendNoticeFor(false, "verify_rate_limited", "too many", 300);
  Check("rate limited", VerifySendNoticeKind::RateLimited, limited);
  CheckMinutes("rate limited minutes", 5, limited);
  CheckValue("rate limited key", "verify_code_rate_limited",
             std::string(VerifySendNoticeKey(limited)));
  CheckMinutes("partial minute rounds up", 2,
               VerifySendNoticeFor(false, "verify_rate_limited", "", 61));
  CheckMinutes("under a minute is one", 1,
               VerifySendNoticeFor(false, "verify_rate_limited", "", 1));

  // a rate limit with no retry time, or a code this app does not know
  Check("rate limited without retry", VerifySendNoticeKind::ServerMessage,
        VerifySendNoticeFor(false, "verify_rate_limited", "Too many attempts", 0));
  Check("rate limited without retry or message", VerifySendNoticeKind::SendFailed,
        VerifySendNoticeFor(false, "verify_rate_limited", "", 0));
  const auto unknown = VerifySendNoticeFor(false, "verify_new_reason", "Try later", 0);
  Check("unknown code", VerifySendNoticeKind::ServerMessage, unknown);
  CheckValue("unknown code message", "Try later", unknown.message);
  Check("unknown code without message", VerifySendNoticeKind::SendFailed,
        VerifySendNoticeFor(false, "verify_new_reason", "", 0));

  // no answer at all keeps the generic error; it never reads sent
  const auto transport = VerifySendNoticeFor(true, "", "", 0);
  Check("transport error", VerifySendNoticeKind::SendFailed, transport);
  CheckValue("transport error key", "something_went_wrong",
             std::string(VerifySendNoticeKey(transport)));

  const auto sent = VerifySendNoticeFor(false, "", "", 0);
  Check("sent", VerifySendNoticeKind::Sent, sent);
  CheckValue("sent key", "verification_code_sent", std::string(VerifySendNoticeKey(sent)));

  // a password reset link the server did not send must not read "link sent"
  CheckValue("reset send failed key", "error_sending_password_reset_link",
             std::string(PasswordResetNoticeKey(failed)));
  CheckValue("reset rate limited key", "reset_link_rate_limited",
             std::string(PasswordResetNoticeKey(limited)));
  CheckValue("reset transport error key", "error_sending_password_reset_link",
             std::string(PasswordResetNoticeKey(transport)));
  CheckValue("reset sent key", "", std::string(PasswordResetNoticeKey(sent)));

  // after a rate limit, Resend / Send stays off until the retry time passes,
  // and the notice counts the minutes down (injected clock, no waiting)
  {
    using namespace std::chrono_literals;
    const auto t0 = ResendCooldown::Clock::time_point{} + 1000h;
    ResendCooldown cooldown;
    CheckInt("cooldown idle can send", 1, cooldown.CanSend(t0));
    cooldown.Start(limited, t0);
    CheckInt("cooldown blocks at start", 0, cooldown.CanSend(t0));
    CheckInt("cooldown minutes at start", 5, cooldown.Minutes(t0));
    CheckInt("cooldown minutes after 61s", 4, cooldown.Minutes(t0 + 61s));
    CheckInt("cooldown blocks a second before", 0, cooldown.CanSend(t0 + 299s));
    CheckInt("cooldown last minute reads one", 1, cooldown.Minutes(t0 + 299s));
    CheckInt("cooldown blocks a partial second before", 0, cooldown.CanSend(t0 + 299500ms));
    CheckInt("cooldown passes at the retry time", 1, cooldown.CanSend(t0 + 300s));
    CheckInt("cooldown minutes after", 0, cooldown.Minutes(t0 + 300s));

    // a rate limit without a retry time, or another notice, does not block
    cooldown.Start(VerifySendNoticeFor(false, "verify_rate_limited", "Too many", 0), t0);
    CheckInt("no retry time does not block", 1, cooldown.CanSend(t0));
    cooldown.Start(limited, t0);
    cooldown.Start(sent, t0 + 1s);
    CheckInt("a sent notice clears", 1, cooldown.CanSend(t0 + 1s));
  }

  std::cout << (gCases - gFailures) << "/" << gCases << " verify send notice checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
