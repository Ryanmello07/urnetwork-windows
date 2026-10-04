// Executable spec for the redeem sheet's answer (App/BalanceCodeRedeem.h): the
// SDK's urnet::classifyBalanceCodeRedeem outcome picks the notice, and only a
// credited redeem skips the redeemed-code list. The server refuses an unknown
// code and one this network already redeemed with the SAME payload, so a retry
// after a lost-but-credited response must read "already redeemed", never
// "invalid" (UPGRADE.md D2).
//
//   c++ -std=c++20 -I ../src/App balance-code-redeem-tests.cpp -o /tmp/balance-code-redeem-tests && /tmp/balance-code-redeem-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>

#include "BalanceCodeRedeem.h"

using urnw::BalanceCodeRedeemNeedsCodeList;
using urnw::BalanceCodeRedeemNotice;
using urnw::BalanceCodeRedeemNoticeFor;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << "\n";
  }
}

}  // namespace

int main() {
  Check(BalanceCodeRedeemNoticeFor("redeemed") == BalanceCodeRedeemNotice::Redeemed,
        "redeemed is the success state");
  Check(BalanceCodeRedeemNoticeFor("already_redeemed") == BalanceCodeRedeemNotice::AlreadyRedeemed,
        "already_redeemed says the data is on the balance");
  Check(BalanceCodeRedeemNoticeFor("invalid") == BalanceCodeRedeemNotice::Invalid,
        "invalid is a refusal");
  Check(BalanceCodeRedeemNoticeFor("unknown") == BalanceCodeRedeemNotice::Unknown,
        "unknown keeps the check-your-balance copy");
  Check(BalanceCodeRedeemNoticeFor("") == BalanceCodeRedeemNotice::Unknown,
        "an empty outcome is never success or invalid");
  Check(BalanceCodeRedeemNoticeFor("something_new") == BalanceCodeRedeemNotice::Unknown,
        "an outcome this build does not know is unknown");

  Check(!BalanceCodeRedeemNeedsCodeList("redeemed"), "a credited redeem needs no list");
  Check(BalanceCodeRedeemNeedsCodeList("invalid"), "a refusal consults the redeemed-code list");
  Check(BalanceCodeRedeemNeedsCodeList("unknown"), "a lost response consults the redeemed-code list");

  std::cout << (gCases - gFailures) << "/" << gCases << " balance code redeem checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
