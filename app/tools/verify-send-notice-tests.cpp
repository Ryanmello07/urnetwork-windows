// Executable spec for what the verify step says about the verification code
// (App/VerifySendNotice.h): a code the server reports it did not send (send
// failed, or rate limited) must not read "code sent" - run against the SAME
// header the app compiles, on any host with a C++20 compiler.
//
//   c++ -std=c++20 -I ../src/App verify-send-notice-tests.cpp \
//       -o /tmp/verify-send-notice-tests && /tmp/verify-send-notice-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <iostream>
#include <string>

#include "VerifySendNotice.h"

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

  std::cout << (gCases - gFailures) << "/" << gCases << " verify send notice checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
