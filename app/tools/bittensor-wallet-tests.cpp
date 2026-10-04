// Executable spec for the app-side Bittensor wallet decisions
// (App/BittensorWalletFlow.h): the chooser's three wallets, the next step per
// sdk transport, which hand-backs a flow ignores, which manual errors keep
// the form open, and the localized text for each sdk refusal code.
//
//   c++ -std=c++20 -I ../src/App bittensor-wallet-tests.cpp -o /tmp/bittensor-wallet-tests && /tmp/bittensor-wallet-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>

#include "BittensorWalletFlow.h"

using namespace urnw::bittensor;

namespace {

int gFailures = 0;
int gCases = 0;

void CheckEq(std::string const& want, std::string const& got, std::string const& what) {
  ++gCases;
  if (want != got) {
    ++gFailures;
    std::cout << "  FAIL " << what << ": want \"" << want << "\", got \"" << got << "\"\n";
  }
}

void Check(bool ok, std::string const& what) {
  ++gCases;
  if (!ok) {
    ++gFailures;
    std::cout << "  FAIL " << what << "\n";
  }
}

}  // namespace

int main() {
  // three wallets, in the sdk's order: Talisman, TAO.com (manual), WalletConnect
  Check(kChooserWalletCount == 3, "three chooser entries");
  CheckEq("talisman", WalletForChoice(0), "first is Talisman");
  CheckEq("taocom", WalletForChoice(1), "second is TAO.com");
  CheckEq("walletconnect", WalletForChoice(2), "third is WalletConnect");
  CheckEq("", WalletForChoice(-1), "close cancels");
  CheckEq("", WalletForChoice(3), "no fourth wallet");

  // the line under each entry
  CheckEq("", ChooserHintKey("talisman"), "talisman has no hint");
  CheckEq("enter_address_manually", ChooserHintKey("taocom"), "tao.com is manual entry");
  CheckEq("bittensor_walletconnect_hint", ChooserHintKey("walletconnect"), "walletconnect wallets");

  // while the browser is open
  CheckEq("bittensor_continue_in_browser", BrowserHintKey("talisman"), "talisman hint");
  CheckEq("bittensor_walletconnect_continue", BrowserHintKey("walletconnect"), "walletconnect hint");
  CheckEq("", BrowserHintKey("taocom"), "manual never opens the browser");

  // only the walletconnect page gets the app's project id
  Check(NeedsWalletConnectProjectId("walletconnect"), "walletconnect takes the project id");
  Check(!NeedsWalletConnectProjectId("talisman"), "talisman does not");
  Check(!NeedsWalletConnectProjectId("taocom"), "tao.com does not");

  // the sdk transport decides the step; a desktop never drives an extension
  Check(NextStepFor("browser_bridge") == NextStep::OpenBrowser, "talisman opens the browser");
  Check(NextStepFor("manual") == NextStep::ManualEntry, "tao.com is manual");
  Check(NextStepFor("extension") == NextStep::Unsupported, "extension is web-only");
  Check(NextStepFor("") == NextStep::Unsupported, "unknown transport");

  // the app keeps its own hand-back link
  CheckEq("urnetwork://bittensor-sign-message", std::string(kRedirectLink), "redirect link");
  CheckEq("windows", std::string(kPlatform), "platform");

  // another flow's tab or a replay never ends this flow
  Check(IsForeignReturn("purpose_mismatch"), "purpose mismatch is foreign");
  Check(IsForeignReturn("not_bittensor_return"), "a non-bittensor link is foreign");
  Check(IsForeignReturn("not_awaiting_wallet"), "a late return is foreign");
  Check(!IsForeignReturn("message_mismatch"), "a different challenge ends the flow");
  Check(!IsForeignReturn("wallet_error"), "a wallet error ends the flow");

  // the manual form stays open for a typo
  Check(IsCorrectable("invalid_signature"), "bad signature is correctable");
  Check(IsCorrectable("invalid_ss58_address"), "bad address is correctable");
  Check(IsCorrectable("address_mismatch"), "other address is correctable");
  Check(!IsCorrectable("challenge_expired"), "expiry needs a new challenge");

  // refusal codes -> store keys
  CheckEq("bittensor_error_invalid_signature", ErrorKey("invalid_signature", "x"), "invalid signature");
  CheckEq("bittensor_error_challenge_expired", ErrorKey("challenge_expired", "x"), "expired");
  CheckEq("bittensor_error_message_mismatch", ErrorKey("message_mismatch", "x"), "message mismatch");
  CheckEq("earnings_wallet_mismatch", ErrorKey("address_mismatch", "x"), "address mismatch");
  CheckEq("invalid_ss58_address", ErrorKey("invalid_ss58_address", "x"), "invalid address");
  CheckEq("", ErrorKey("wallet_error", "x"), "the wallet's own message");
  CheckEq("wallet_connect_failed", ErrorKey("no_challenge", "wallet_connect_failed"), "fallback");

  std::cout << (gCases - gFailures) << "/" << gCases << " bittensor wallet checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
