// How the redeem sheet answers a balance-code redeem, from the SDK's
// classification (urnet::classifyBalanceCodeRedeem, sdk payment_catalog.go).
//
// The server sends ONE payload ("Unknown balance code.") for a code that never
// existed and for a code this network already redeemed, and a lost response
// looks like a transport failure even when the redeem committed. So a first
// classification that is not "redeemed" is classified again with the
// network's redeemed-code list before the sheet says anything: a retry after
// a lost-but-credited response reads "already redeemed", never "invalid"
// (UPGRADE.md D2). An answer with neither a transfer balance nor an error is
// "unknown" (the SDK's rule), the same as a transport failure.
//
// Dependency-free so tools/balance-code-redeem-tests.cpp runs it without the SDK.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>

namespace urnw {

enum class BalanceCodeRedeemNotice {
  Redeemed,         // credited by this call: the success state
  AlreadyRedeemed,  // this network already has it: the data is on the balance
  Invalid,          // the server refused it and this network never redeemed it
  Unknown,          // no answer: the redeem may have committed, check the balance
};

// the SDK's BalanceCodeRedeemOutcome* values
inline BalanceCodeRedeemNotice BalanceCodeRedeemNoticeFor(const std::string& outcome) {
  if (outcome == "redeemed") return BalanceCodeRedeemNotice::Redeemed;
  if (outcome == "already_redeemed") return BalanceCodeRedeemNotice::AlreadyRedeemed;
  if (outcome == "invalid") return BalanceCodeRedeemNotice::Invalid;
  return BalanceCodeRedeemNotice::Unknown;
}

// Whether the sheet must read the redeemed-code list and classify again
// before it shows anything (every outcome but a credited redeem).
inline bool BalanceCodeRedeemNeedsCodeList(const std::string& outcome) {
  return BalanceCodeRedeemNoticeFor(outcome) != BalanceCodeRedeemNotice::Redeemed;
}

}  // namespace urnw
