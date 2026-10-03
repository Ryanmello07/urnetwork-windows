// Executable spec for the feedback form's Send button (App/FeedbackSendState.h):
// while the request is out it is disabled and reads "Sending…"; either answer
// puts back an enabled "Send" - run against the SAME header the app compiles,
// on any host with a C++20 compiler.
//
//   c++ -std=c++20 -I ../src/App feedback-send-tests.cpp \
//       -o /tmp/feedback-send-tests && /tmp/feedback-send-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>

#include "FeedbackSendState.h"

using urnw::FeedbackSendButtonFor;

namespace {

int gFailures = 0;

void Check(bool ok, const std::string& what) {
  if (!ok) {
    ++gFailures;
    std::cout << "  FAIL " << what << "\n";
  }
}

void CheckLabel(std::string_view expected, bool sending, const std::string& what) {
  const std::string_view actual = FeedbackSendButtonFor(sending).labelKey;
  Check(expected == actual,
        what + ": expected \"" + std::string(expected) + "\", got \"" + std::string(actual) + "\"");
}

}  // namespace

int main() {
  // the request is out: the button says so, and cannot send twice
  CheckLabel("feedback_sending", true, "label while sending");
  Check(!FeedbackSendButtonFor(true).enabled, "Send is disabled while sending");

  // idle, and after either answer: an enabled Send (the retry after a failure)
  CheckLabel("send", false, "label when idle");
  Check(FeedbackSendButtonFor(false).enabled, "Send is enabled when idle");

  // the label is never the send label while sending, whatever the key
  Check(FeedbackSendButtonFor(true).labelKey != FeedbackSendButtonFor(false).labelKey,
        "sending and idle labels differ");

  if (gFailures) {
    std::cout << gFailures << " failure(s)\n";
    return 1;
  }
  std::cout << "feedback send button: all cases pass\n";
  return 0;
}
