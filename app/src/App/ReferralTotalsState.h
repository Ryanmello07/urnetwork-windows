// The referral count read behind "Total referrals" (the Account pane's
// referral rows and the Refer and earn page), decided pure so
// tools/referral-totals-tests.cpp can pin it on any host against the header
// the app compiles.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

namespace urnw {

// What a "Total referrals" figure shows.
enum class ReferralTotalsView {
  Loading,      // no answer yet
  Count,        // the count the server returned
  Unavailable,  // the read failed with no count to show: the error and Try again
};

// The last count GET /account/referral-code returned. The figure used to start
// at 0 and ignore a failed read, so a failure read as "Total referrals: 0" (and
// "+0 GiB/day"). A failed background read keeps a count already shown.
class ReferralTotalsFetch {
 public:
  // Logout: nothing read for the next network yet.
  void Reset() {
    total_ = 0;
    counted_ = false;
    failed_ = false;
  }
  // The user asked to read again: back to Loading until the answer lands.
  void Retry() { failed_ = false; }
  void Succeed(int64_t total) {
    total_ = total;
    counted_ = true;
    failed_ = false;
  }
  void Fail() { failed_ = true; }

  int64_t Total() const { return total_; }

  ReferralTotalsView View() const {
    if (counted_) return ReferralTotalsView::Count;
    return failed_ ? ReferralTotalsView::Unavailable : ReferralTotalsView::Loading;
  }

 private:
  int64_t total_ = 0;
  bool counted_ = false;
  bool failed_ = false;
};

}  // namespace urnw
