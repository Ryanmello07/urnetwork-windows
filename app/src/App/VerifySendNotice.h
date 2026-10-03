// What the verify step says about the verification code the server was asked
// to send (sign in with password, network create, resend), decided pure so
// tools/verify-send-notice-tests.cpp can pin it on any host against the header
// the app compiles.
//
// The server reports a code it did not send as AuthVerifySendError
// (verification_required.send_error, or AuthVerifySendResult.error when the
// request sets result_errors). Without it the app said a code was sent when it
// was not.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
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

}  // namespace urnw
