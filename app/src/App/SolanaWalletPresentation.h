// Everything the Solana payout wallet decides BEFORE it touches a XAML object:
// whether a pasted address is worth asking the server about, its short form,
// which of the account's wallets is the Solana payout wallet, the USDC still
// waiting to be paid out, how the three reads behind the card commit and what
// the Earnings pane shows from them, the connect sheet's state machine, and the
// store key a failure renders with.
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
inline constexpr const char* kBlockchainPolygon = "MATIC";
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
// Polygon user's USDC goes there, and hiding it would put the waiting line up,
// inviting them to replace a working payout wallet. Its card is titled
// "Wallet", not "Solana wallet" (SolanaPanelView::solana).
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

// ---- the three reads behind the card ----------------------------------------
//
// The account's wallets (GET /account/wallets), the payout wallet id (GET
// /account/payout-wallet) and the payments (GET /account/payments) are read
// independently, and what the pane shows is decided once all three have
// answered, ok or not:
//
//   * the wallets read failed: nothing, since which wallet is the payout wallet
//     is unknown;
//   * the payout read failed: the last known payout wallet id stands, and with
//     none known there is no card;
//   * the payments read failed: the card, without its figure;
//   * the waiting line needs all three: it claims both that there is no payout
//     wallet and how much is waiting;
//   * an answer with an empty payout id keeps the known one: a transient nil must
//     not make the payout wallet look unset (a removed wallet is inactive, so it
//     drops out anyway).
//
// A plain reload keeps the view on screen until its reads commit. A write (a
// link, a payout switch, a removal) hides it until then, and so does another
// network, whose wallets and payout wallet id are cleared at once.

// Which of the three reads answered ok.
struct LegacyReads {
  bool wallets = false;
  bool payout = false;
  bool payments = false;
};

// Loading until a load's reads have all answered; Ready from then on.
enum class LegacyState { Loading, Ready };

// What the Earnings pane shows. With a payout wallet, the card, with its
// waiting line only when something is waiting. Without one, the waiting line
// above the Bittensor action, only when something is waiting - the emailed
// user's situation.
struct SolanaPanelView {
  bool showCard = false;
  LegacyWallet wallet;           // the card's wallet, when showCard
  // the card's title and its address's accessible prefix: "Solana wallet", or
  // "Wallet" for a legacy Polygon payout wallet, whose address is 0x hex
  bool solana = true;
  std::string shortAddress;      // what the card draws: ShortAddress(wallet.address)
  bool showCardPending = false;  // the card's "N USDC waiting"
  bool showWaitingLine = false;  // the line above the Bittensor action
  std::string pendingUsd;        // FormatUsd of the total, for either line
};

// Ready once all three answered, ok or not. `payoutWallet` is PayoutWalletFor the
// fresh payout id or, when that read failed, the last known one.
SolanaPanelView SolanaPanelFor(LegacyState state, const LegacyReads& reads,
                               const std::optional<LegacyWallet>& payoutWallet,
                               int64_t pendingNanoCents);

// which read an answer belongs to
enum class LegacyRead { Wallets, Payout, Payments };

// One read's payload. Only the field of its read is used, and nothing of a
// failed read.
struct LegacyAnswer {
  std::vector<LegacyWallet> wallets;  // LegacyRead::Wallets
  std::string payoutId;               // LegacyRead::Payout, "" when the network has none
  std::vector<HeldPayment> payments;  // LegacyRead::Payments
};

// What the pane last committed, for one network: the card's inputs.
struct LegacyCommitted {
  std::string networkId;        // the network these belong to
  bool ready = false;           // a load committed since the view was last hidden
  LegacyReads reads;            // which reads that load answered ok
  std::vector<LegacyWallet> wallets;
  std::string payoutWalletId;   // the last known payout wallet id
  int64_t pendingNanoCents = 0;
};

// A load starts for `networkId`. A view of another network is cleared (its
// wallets, payout wallet id, total and reads) and hidden; `reset` - a write just
// happened - hides the view until this load commits; a plain reload leaves it
// on screen.
void BeginLegacyLoad(LegacyCommitted& view, const std::string& networkId, bool reset);

// One load's answers, until all three are in.
class LegacyLoad {
 public:
  LegacyLoad() = default;
  explicit LegacyLoad(uint32_t generation) : generation_(generation) {}

  uint32_t generation() const { return generation_; }

  // `which` answered for the load numbered `generation`; `ok` is false for a
  // failed read, whose payload is ignored. False, recording nothing, for another
  // load's answer or for a read that has already answered.
  bool Answer(uint32_t generation, LegacyRead which, bool ok, LegacyAnswer answer = {});

  // All three reads have answered, ok or not.
  bool Complete() const;

  // Once all three have answered: commits into `view` the part of each read that
  // answered ok (an empty payout id keeps the known one), records which did, and
  // makes the view ready. A view of another network is cleared first. False,
  // with `view` untouched, before all three have answered.
  bool Commit(LegacyCommitted& view, const std::string& networkId) const;

 private:
  uint32_t generation_ = 0;
  LegacyReads answered_;
  LegacyReads ok_;
  LegacyAnswer answer_;
};

// What the pane shows from a committed view: SolanaPanelFor over its parts.
SolanaPanelView SolanaPanelFor(const LegacyCommitted& view);

// After a wallet is linked: POST /account/payout-wallet is needed unless the new
// wallet already is the payout wallet. The server makes a new non-TAO wallet the
// payout wallet by itself only when the network has none.
bool NeedsPayoutSwitch(const std::string& newWalletId, const std::string& payoutWalletId);

// The payout wallet id that switch is decided on: a fresh GET
// /account/payout-wallet, never the card's id, which may be stale (the payout
// wallet can move on the web). A failed read counts as none, so the switch runs:
// POST /account/payout-wallet is idempotent for this network's wallet, while a
// skipped switch could leave USDC going to a wallet the user replaced.
std::string PayoutIdForSwitch(bool readOk, const std::string& readPayoutId);

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
//                                                         180 s -> Failed (no detail)
//   Checking        checking_wallet_address               valid -> Ready; invalid or
//                                                         unanswered -> Idle, with its line
//   Ready           Connect enabled                       Connect -> Linking
//   Linking         connecting_to_wallet; providers,      a wallet id -> Linked;
//                   Connect and the entry disabled        an error -> Failed(detail);
//                                                         20 s -> Failed (no detail)
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
// The store key of a connect, link or remove failure, the rule every app shares:
// error_connecting_wallet_with_reason (formatted with the detail) when there is a
// detail, something_went_wrong when there is none - as after a watchdog gave up.
const char* FailureKey(const std::string& detail);

}  // namespace urnw::solana
