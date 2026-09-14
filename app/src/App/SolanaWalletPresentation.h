// Everything the Solana payout wallet decides BEFORE it touches a XAML object:
// whether a pasted address is worth asking the server about, its short form,
// which of the account's wallets is the Solana payout wallet, the USDC still
// waiting to be paid out, what the Earnings pane shows from those, the connect
// sheet's state machine, and the store key a failure renders with.
//
// Why it exists: USDC payouts continue until the migration to Bittensor
// completes, and a network whose payouts are held for want of a wallet is
// emailed "Connect a wallet - N USDC waiting". The Earnings pane lost its
// Solana connect flow with the Subtensor rework (e14049c); this is the logic of
// the restored flow, reached from the "Connect Solana wallet" overflow beside
// "Connect Bittensor wallet".
//
// It is all here, and all pure, for the reason ExtenderPresentation.h gives:
// the windows solution has no test project and a WinUI 3 app cannot be built
// off Windows, so every decision expressed on plain values is verified by
// tools/solana-wallet-tests.cpp on any host with a C++20 compiler. The SDK's
// AccountWallet and AccountPayment are mirrored as plain views rather than
// included; WalletPage.cpp copies the fields across at its boundary.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace urnw::solana {

// ---- addresses --------------------------------------------------------------

// `value` without leading or trailing ASCII whitespace.
std::string Trim(std::string value);

// A Solana public key as a user pastes one: once trimmed, 32 to 44 characters
// of the base58 alphabet (no 0, O, I or l). Syntax only - POST
// /wallet/validate-address is the validator - but a paste that fails it is
// answered at once, with no request, like the Bittensor entry's ss58 check.
bool LooksLikeSolanaAddress(const std::string& text);

// "EPjF…Dt1v": the first and last four characters around an ellipsis, the
// SDK's ShortSs58 form (sdk sn_util.go: trimmed first, and returned whole at 12
// bytes or fewer). The pane renders through EarningsSheets.h ShortAddress,
// which calls the SDK; this copy keeps the tests free of the SDK.
std::string ShortAddress(const std::string& address);

// ---- the payout wallet ------------------------------------------------------

inline constexpr const char* kBlockchainSolana = "SOL";
inline constexpr const char* kBlockchainBittensor = "TAO";

// As much of the SDK's AccountWallet as the payout wallet needs.
struct LegacyWallet {
  std::string id;              // wallet_id
  std::string blockchain;      // "SOL" | "MATIC" | "TAO"
  std::string address;         // wallet_address
  std::string circleWalletId;  // non-empty for a Circle custodial wallet
  bool active = false;

  bool operator==(const LegacyWallet&) const = default;
};

// The Solana payout wallet, if there is one: the active, non-custodial, non-TAO
// wallet whose id is the network's (non-empty) payout wallet id. TAO account
// wallets - the old address-only Bittensor connect - are left out, since the
// Bittensor block shows the coldkey. A MATIC payout wallet is kept: a legacy
// Polygon user's USDC goes there, and it renders under the same heading.
std::optional<LegacyWallet> PayoutWalletFor(const std::vector<LegacyWallet>& wallets,
                                            const std::string& payoutId);

// As much of the SDK's AccountPayment as the waiting total needs.
struct HeldPayment {
  int64_t payoutNanoCents = 0;
  bool completed = false;
  bool canceled = false;
};

// The USDC not yet paid out: every payment neither completed nor canceled
// (GET /account/payments). The email's figure is one payout run's; this is all
// of them.
int64_t PendingUsdcNanoCents(const std::vector<HeldPayment>& payments);

// USD from nano cents: 1 USD is 1e9 of them (sdk.go NanoCentsToUsd), the scale
// urnet::nanoCentsToUsd uses, mirrored so this unit needs no SDK.
double NanoCentsToUsd(int64_t nanoCents);

// "3.87": two decimals, rounded half away from zero to the cent in integer
// arithmetic, so a total never prints as 3.869999.
std::string FormatUsd(int64_t nanoCents);

// Whether a total is worth a line: it still reads above zero at two decimals.
// A sub-cent remainder would otherwise print "0.00 USDC waiting".
bool HasPendingUsd(int64_t nanoCents);

// The state of the three reads behind the card (the account's wallets, the
// payout wallet id, the payments), taken together.
enum class LegacyState { Loading, Ready, Failed };

// What the Earnings pane shows from them. Loading and Failed show neither the
// card nor the line: this wallet is secondary to the Bittensor block, and a
// failed read is logged rather than shouted. With a payout wallet, the card,
// with its waiting line only when something is waiting. Without one, the
// waiting line above the Bittensor action, only when something is waiting -
// the emailed user's situation.
struct SolanaPanelView {
  bool showCard = false;
  LegacyWallet wallet;           // the card's wallet, when showCard
  bool showCardPending = false;  // the card's "N USDC waiting"
  bool showWaitingLine = false;  // the line above the Bittensor action
  std::string pendingUsd;        // FormatUsd of the total, for either line
};

SolanaPanelView SolanaPanelFor(LegacyState state,
                               const std::optional<LegacyWallet>& payoutWallet,
                               int64_t pendingNanoCents);

// After a wallet is linked: POST /account/payout-wallet is needed unless the new
// wallet already is the payout wallet. The server makes a new non-TAO wallet the
// payout wallet by itself only when the network has none.
bool NeedsPayoutSwitch(const std::string& newWalletId, const std::string& payoutWalletId);

// ---- the connect sheet ------------------------------------------------------
//
// Two ways in, one way out. A wallet app (Phantom or Solflare, through the
// ur.io/wallet-connect bridge, connect only) hands back its public key, which
// goes straight to linking: the server validates it on create, which is what
// the android and apple originals relied on. A pasted address is checked
// locally, then by the server for SOL, and linked on Connect. Linking is
// createAccountWallet {SOL, address, USDC}; Linked closes the sheet.
//
//   state           shows                                 leaves on
//   Idle            providers, the manual entry           a provider -> OpeningBrowser;
//                                                         a plausible address -> Checking
//   OpeningBrowser  opening_wallet_in_browser;            the public key -> Linking;
//                   providers and Connect disabled        a bridge error -> Failed(detail);
//                                                         180 s -> Failed(timeout)
//   Checking        checking_wallet_address               valid -> Ready; invalid or
//                                                         unanswered -> Idle, with its line
//   Ready           Connect enabled                       Connect -> Linking
//   Linking         connecting_to_wallet; providers,      a wallet id -> Linked;
//                   Connect and the entry disabled        an error -> Failed(detail);
//                                                         20 s -> Failed(timeout)
//   Failed          the failure line; controls enabled    any new attempt clears it
//   Linked          everything disabled                   (the sheet closes)
//
// The manual field's verdict is kept beside the state rather than folded into
// it: a wallet-app attempt that fails must not throw away an address the server
// has already accepted.
enum class ConnectState { Idle, OpeningBrowser, Checking, Ready, Linking, Failed, Linked };

// the manual field's verdict
enum class AddressCheck { None, Checking, Valid, Invalid, Unavailable };

// what POST /wallet/validate-address said: valid, not valid, or nothing usable
// (a transport error)
enum class ServerVerdict { Valid, Invalid, Unavailable };

struct ConnectMachine {
  ConnectState state = ConnectState::Idle;
  std::string detail;     // Failed: the bridge's or the server's message, "" when none
  bool timedOut = false;  // Failed: a watchdog gave up on the request
  AddressCheck check = AddressCheck::None;
  std::string address;    // the trimmed manual address `check` is about
};

// Each event returns false and changes nothing when it does not apply in the
// current state, so a late answer to an abandoned request is dropped here as
// well as by the sheet's generation counters.

// Phantom or Solflare pressed. Refused while a round trip is in flight.
bool ChooseProvider(ConnectMachine& m);
// The bridge answered with the wallet's public key.
bool PublicKey(ConnectMachine& m);
// The bridge answered with an error, or the host superseded the request.
bool BridgeError(ConnectMachine& m, const std::string& detail);
// A watchdog gave up on the bridge round trip or on the create call.
bool Timeout(ConnectMachine& m);
// A keystroke in the manual field: the verdict belonged to the old text.
bool Typed(ConnectMachine& m);
// The debounce after typing fired with the field's `text`. Returns true when the
// address passed the local check and the server should be asked; with
// `askServer` false (no session: the preview) the check is local only.
bool Debounced(ConnectMachine& m, const std::string& text, bool askServer);
// The server's answer for the address being checked.
bool Verdict(ConnectMachine& m, ServerVerdict verdict);
// Connect pressed on a checked address.
bool Submit(ConnectMachine& m);
// createAccountWallet answered: `ok` with a non-empty wallet id links.
bool CreateResult(ConnectMachine& m, bool ok, const std::string& walletId,
                  const std::string& detail);

// ---- what the sheet renders from the machine
bool ProvidersEnabled(const ConnectMachine& m);
bool EntryEnabled(const ConnectMachine& m);
bool ConnectEnabled(const ConnectMachine& m);
// the status line under the providers, "" for none
const char* StatusKey(const ConnectMachine& m);
// the manual field's supporting line, "" for none
const char* CheckKey(const ConnectMachine& m);
// the supporting line is a refusal (danger) rather than progress (muted)
bool CheckIsError(const ConnectMachine& m);
// The failure line's store key: wallet_connect_failed for a timeout,
// error_connecting_wallet_with_reason (formatted with the detail) when there is
// a detail, something_went_wrong otherwise.
const char* FailureKey(const std::string& detail, bool timedOut = false);

}  // namespace urnw::solana
