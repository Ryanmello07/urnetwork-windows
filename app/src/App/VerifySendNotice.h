// What the verify step says about the verification code the server was asked
// to send (sign in with password, network create, resend), decided pure so
// tools/verify-send-notice-tests.cpp can pin it on any host against the header
// the app compiles.
//
// The server reports a code it did not send as AuthVerifySendError
// (verification_required.send_error, or AuthVerifySendResult.error when the
// request sets result_errors). Without it the app said a code was sent when it
// was not. A password reset link the server did not send comes back the same
// way (AuthPasswordResetResult.error).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace urnw {

enum class VerifySendNoticeKind {
  Sent,           // verification_code_sent
  RateLimited,    // verify_code_rate_limited (plural on minutes)
  SendFailed,     // error_sending_verification_code
  ServerMessage,  // the server's own message
};

struct VerifySendNotice {
  VerifySendNoticeKind kind = VerifySendNoticeKind::Sent;
  // minutes until a new code can be requested (RateLimited)
  int64_t minutes = 0;
  // seconds until a new code can be requested (RateLimited)
  int64_t retryAfterSeconds = 0;
  // the server's message (ServerMessage)
  std::string message;
  // the request itself failed (no answer): the app's generic error
  bool transport = false;
};

inline constexpr std::string_view kVerifySendErrorCodeSendFailed = "verify_send_failed";
inline constexpr std::string_view kVerifySendErrorCodeRateLimited = "verify_rate_limited";

// `code`, `message` and `retryAfterSeconds` are the AuthVerifySendError fields
// (all empty / 0 when there was none). An error with an unknown code, or a rate
// limit without a retry time, shows the server's message, else the send error.
inline VerifySendNotice VerifySendNoticeFor(bool transportError, std::string const& code,
                                            std::string const& message,
                                            int64_t retryAfterSeconds) {
  VerifySendNotice notice;
  if (transportError) {
    notice.kind = VerifySendNoticeKind::SendFailed;
    notice.transport = true;
    return notice;
  }
  if (code == kVerifySendErrorCodeRateLimited && retryAfterSeconds > 0) {
    notice.kind = VerifySendNoticeKind::RateLimited;
    notice.minutes = (retryAfterSeconds + 59) / 60;
    notice.retryAfterSeconds = retryAfterSeconds;
    return notice;
  }
  if (code == kVerifySendErrorCodeSendFailed) {
    notice.kind = VerifySendNoticeKind::SendFailed;
    return notice;
  }
  if (code.empty() && message.empty()) return notice;  // sent
  if (message.empty()) {
    notice.kind = VerifySendNoticeKind::SendFailed;
  } else {
    notice.kind = VerifySendNoticeKind::ServerMessage;
    notice.message = message;
  }
  return notice;
}

// The string key the notice is shown with; empty for ServerMessage, which shows
// `message` as is.
inline std::string_view VerifySendNoticeKey(VerifySendNotice const& notice) {
  switch (notice.kind) {
    case VerifySendNoticeKind::Sent: return "verification_code_sent";
    case VerifySendNoticeKind::RateLimited: return "verify_code_rate_limited";
    case VerifySendNoticeKind::SendFailed:
      // a request that got no answer keeps the generic error it always had
      return notice.transport ? "something_went_wrong" : "error_sending_verification_code";
    case VerifySendNoticeKind::ServerMessage: return "";
  }
  return "";
}

// The string key a password reset notice is shown with; empty for Sent (the
// caller's own "link sent" line) and ServerMessage. A request that got no
// answer did not send a link either.
inline std::string_view PasswordResetNoticeKey(VerifySendNotice const& notice) {
  switch (notice.kind) {
    case VerifySendNoticeKind::Sent: return "";
    case VerifySendNoticeKind::RateLimited: return "reset_link_rate_limited";
    case VerifySendNoticeKind::SendFailed: return "error_sending_password_reset_link";
    case VerifySendNoticeKind::ServerMessage: return "";
  }
  return "";
}

// Keeps a Resend / Send control off after the server refused a code for too
// many attempts, until its retry time passes. The clock is passed in so the
// countdown is testable; the app passes steady_clock::now().
class ResendCooldown {
 public:
  using Clock = std::chrono::steady_clock;

  // Arms the cooldown for a rate-limited notice with a retry time; any other
  // notice clears it.
  void Start(VerifySendNotice const& notice, Clock::time_point now) {
    if (notice.kind == VerifySendNoticeKind::RateLimited && notice.retryAfterSeconds > 0) {
      retryAt_ = now + std::chrono::seconds(notice.retryAfterSeconds);
    } else {
      retryAt_.reset();
    }
  }

  void Clear() { retryAt_.reset(); }

  // whether a cooldown was started and has not been cleared (it may have
  // passed; see CanSend)
  bool Armed() const { return retryAt_.has_value(); }

  int64_t RemainingSeconds(Clock::time_point now) const {
    if (!retryAt_ || *retryAt_ <= now) return 0;
    const auto remaining = *retryAt_ - now;
    // a partial second still blocks
    return (std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count() + 999) /
           1000;
  }

  bool CanSend(Clock::time_point now) const { return RemainingSeconds(now) == 0; }

  // the minutes the rate-limit notice shows: the remaining time rounded up, at
  // least one while the cooldown runs, zero once it has passed
  int64_t Minutes(Clock::time_point now) const {
    const int64_t remaining = RemainingSeconds(now);
    return remaining == 0 ? 0 : (remaining + 59) / 60;
  }

 private:
  std::optional<Clock::time_point> retryAt_;
};

}  // namespace urnw
