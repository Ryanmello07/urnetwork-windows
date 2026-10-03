// The feedback form's Send button while a send is out, decided pure so
// tools/feedback-send-tests.cpp can pin it on any host against the header the
// app compiles.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string_view>

namespace urnw {

// What the Send button shows.
struct FeedbackSendButton {
  // false while the request is out, so one press is one report
  bool enabled;
  // store key of the button label (and its automation name)
  std::string_view labelKey;
};

// The button only went disabled while the request was out and kept reading
// "Send", so a slow send looked like a dead button. It reads "Sending…" until
// the answer lands; either answer puts "Send" back (on a failure the text
// stays, so Send is the retry).
inline FeedbackSendButton FeedbackSendButtonFor(bool sending) {
  if (sending) return {false, "feedback_sending"};
  return {true, "send"};
}

}  // namespace urnw
