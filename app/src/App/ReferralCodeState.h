// The referral code read behind the referral card (ReferralCard.h), decided
// pure so tools/referral-code-tests.cpp can pin it on any host against the
// header the app compiles.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string>
#include <utility>

namespace urnw {

// What the referral card shows where the code goes.
enum class ReferralCodeView {
  Loading,      // no answer yet: the ring
  Code,         // the code, with copy and share
  Unavailable,  // answered without a code (a failed read): the error and Try again
};

// The last code GET /account/referral-code returned and whether any answer has
// arrived since the read was (re)started. A failed background poll keeps a
// code already shown; with no code it ends the ring instead of spinning until
// a later poll happens to succeed.
class ReferralCodeFetch {
 public:
  // Logout: nothing read for the next network yet.
  void Reset() {
    code_.reset();
    answered_ = false;
  }
  // The user asked to read again: the card goes back to the ring until the
  // answer lands.
  void Retry() { answered_ = false; }
  // Both answers return whether the card must repaint: every settled read
  // changes what it shows or confirms it, and the card waits on this read.
  bool Succeed(std::optional<std::string> code) {
    code_ = std::move(code);
    answered_ = true;
    return true;
  }
  // The last reading stands.
  bool Fail() {
    answered_ = true;
    return true;
  }

  std::optional<std::string> const& Code() const { return code_; }

  ReferralCodeView View() const {
    if (code_ && !code_->empty()) return ReferralCodeView::Code;
    return answered_ ? ReferralCodeView::Unavailable : ReferralCodeView::Loading;
  }

 private:
  std::optional<std::string> code_;
  bool answered_ = false;
};

}  // namespace urnw
