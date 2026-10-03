// What the Earnings page says when Api.verifySeekerHolder answers (android
// SeekerVerifyNotice.fromVerifyResult), decided pure so
// tools/seeker-verify-notice-tests.cpp can pin it on any host against the
// header the app compiles.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <string>

namespace urnw {

enum class SeekerVerifyNoticeKind {
  Verified,   // successfully_claimed_multiplier
  NotHolder,  // seeker_token_not_found, with the end of the wallet address
  Reason,     // error_claiming_multiplier_with_reason, with the failure text
  Failed,     // error_claiming_multiplier: no answer and no reason
};

// The verify request settled. `answered` is a result arrived (the server
// checked the wallet); `failure` is the transport error or the server's own
// message. A server that answers success=false without a message checked the
// wallet and found no Seeker or Saga token: that is not a failed request, and
// it used to be shown as the generic claim error.
inline SeekerVerifyNoticeKind SeekerVerifyNoticeFor(bool answered, bool success,
                                                    std::string const& failure) {
  if (!failure.empty()) return SeekerVerifyNoticeKind::Reason;
  if (!answered) return SeekerVerifyNoticeKind::Failed;
  return success ? SeekerVerifyNoticeKind::Verified : SeekerVerifyNoticeKind::NotHolder;
}

inline constexpr std::size_t kSeekerWalletSuffixLength = 7;

// The end of a (base58, ascii) wallet address shown after "…".
inline std::string SeekerWalletSuffix(std::string const& walletAddress) {
  if (walletAddress.size() <= kSeekerWalletSuffixLength) return walletAddress;
  return walletAddress.substr(walletAddress.size() - kSeekerWalletSuffixLength);
}

}  // namespace urnw
