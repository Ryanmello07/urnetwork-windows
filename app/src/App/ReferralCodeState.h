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
  Loading,      // no code yet: the ring
  Code,         // the code, with copy and share
  Unavailable,  // not shown yet
};

// The last code GET /account/referral-code returned. A failed read keeps the
// last reading; the 30s poll retries.
class ReferralCodeFetch {
 public:
  // Logout: nothing read for the next network yet.
  void Reset() { code_.reset(); }
  // Nothing reads again on demand yet.
  void Retry() {}
  // Both answers return whether the card must repaint. The card repaints on
  // the next balance publish.
  bool Succeed(std::optional<std::string> code) {
    code_ = std::move(code);
    return false;
  }
  // The last reading stands.
  bool Fail() { return false; }

  std::optional<std::string> const& Code() const { return code_; }

  ReferralCodeView View() const {
    return code_ && !code_->empty() ? ReferralCodeView::Code : ReferralCodeView::Loading;
  }

 private:
  std::optional<std::string> code_;
};

}  // namespace urnw
