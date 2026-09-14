// SPDX-License-Identifier: MPL-2.0
//
// No pch.h on purpose -- see SolanaWalletPresentation.h. App.vcxproj compiles
// this with PrecompiledHeader=NotUsing, like ExtenderPresentation.cpp.
#include "SolanaWalletPresentation.h"

#include <cstdio>
#include <string_view>
#include <utility>

namespace urnw::solana {
namespace {

constexpr bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

// the digits and the letters, minus the four that read alike: 0, O, I and l
constexpr std::string_view kBase58Alphabet =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

// a 32-byte ed25519 public key in base58
constexpr size_t kMinSolanaAddress = 32;
constexpr size_t kMaxSolanaAddress = 44;

// the SDK's ShortSs58: whole up to this many bytes
constexpr size_t kShortWhole = 12;
constexpr size_t kShortKeep = 4;

// 1 USD is 1e9 nano cents, so a cent is 1e7
constexpr uint64_t kNanoCentsPerCent = 10'000'000;

uint64_t RoundedCents(int64_t nanoCents) {
  // through unsigned, so the most negative value has a magnitude too
  const uint64_t magnitude = nanoCents < 0 ? uint64_t{0} - static_cast<uint64_t>(nanoCents)
                                           : static_cast<uint64_t>(nanoCents);
  return (magnitude + kNanoCentsPerCent / 2) / kNanoCentsPerCent;
}

bool InFlight(ConnectState state) {
  return state == ConnectState::OpeningBrowser || state == ConnectState::Linking ||
         state == ConnectState::Linked;
}

void ClearFailure(ConnectMachine& m) { m.detail.clear(); }

void Fail(ConnectMachine& m, const std::string& detail) {
  m.state = ConnectState::Failed;
  m.detail = detail;
}

// another network's view, or the first: nothing of what was committed applies
void ClearView(LegacyCommitted& view, const std::string& networkId) {
  view.networkId = networkId;
  view.ready = false;
  view.reads = LegacyReads{};
  view.wallets.clear();
  view.payoutWalletId.clear();
  view.pendingNanoCents = 0;
}

// Checking and Ready are the manual field's own states: once its verdict is
// gone or negative they fall back to Idle. A flow's state is left alone.
void SettleCheck(ConnectMachine& m, AddressCheck check) {
  m.check = check;
  if (m.state == ConnectState::Checking || m.state == ConnectState::Ready) {
    m.state = ConnectState::Idle;
  }
}

}  // namespace

// ---- addresses --------------------------------------------------------------

std::string Trim(std::string value) {
  size_t begin = 0;
  while (begin < value.size() && IsSpace(value[begin])) ++begin;
  size_t end = value.size();
  while (end > begin && IsSpace(value[end - 1])) --end;
  return value.substr(begin, end - begin);
}

bool LooksLikeSolanaAddress(const std::string& text) {
  const std::string address = Trim(text);
  if (address.size() < kMinSolanaAddress || address.size() > kMaxSolanaAddress) return false;
  for (char c : address) {
    if (kBase58Alphabet.find(c) == std::string_view::npos) return false;
  }
  return true;
}

std::string ShortAddress(const std::string& address) {
  const std::string trimmed = Trim(address);
  if (trimmed.size() <= kShortWhole) return trimmed;
  // U+2026, as UTF-8 bytes (the SDK slices bytes the same way)
  return trimmed.substr(0, kShortKeep) + "\xE2\x80\xA6" +
         trimmed.substr(trimmed.size() - kShortKeep);
}

// ---- the payout wallet ------------------------------------------------------

std::optional<LegacyWallet> PayoutWalletFor(const std::vector<LegacyWallet>& wallets,
                                            const std::string& payoutId) {
  if (payoutId.empty()) return std::nullopt;
  for (const LegacyWallet& wallet : wallets) {
    if (!wallet.circleWalletId.empty()) continue;
    if (wallet.blockchain == kBlockchainBittensor) continue;
    if (!wallet.active) continue;
    if (wallet.id == payoutId) return wallet;
  }
  return std::nullopt;
}

int64_t PendingUsdcNanoCents(const std::vector<HeldPayment>& payments) {
  int64_t total = 0;
  for (const HeldPayment& payment : payments) {
    if (!payment.completed && !payment.canceled) total += payment.payoutNanoCents;
  }
  return total;
}

double NanoCentsToUsd(int64_t nanoCents) {
  return static_cast<double>(nanoCents) / 1'000'000'000.0;
}

std::string FormatUsd(int64_t nanoCents) {
  const uint64_t cents = RoundedCents(nanoCents);
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%s%llu.%02llu",
                (nanoCents < 0 && cents > 0) ? "-" : "",
                static_cast<unsigned long long>(cents / 100),
                static_cast<unsigned long long>(cents % 100));
  return buffer;
}

bool HasPendingUsd(int64_t nanoCents) {
  return nanoCents > 0 && RoundedCents(nanoCents) > 0;
}

SolanaPanelView SolanaPanelFor(LegacyState state, const LegacyReads& reads,
                               const std::optional<LegacyWallet>& payoutWallet,
                               int64_t pendingNanoCents) {
  SolanaPanelView view;
  if (state != LegacyState::Ready || !reads.wallets) return view;
  const bool waiting = reads.payments && HasPendingUsd(pendingNanoCents);
  view.pendingUsd = FormatUsd(pendingNanoCents);
  if (payoutWallet) {
    view.showCard = true;
    view.wallet = *payoutWallet;
    view.shortAddress = ShortAddress(payoutWallet->address);
    view.showCardPending = waiting;
    return view;
  }
  view.showWaitingLine = reads.payout && waiting;
  return view;
}

void BeginLegacyLoad(LegacyCommitted& view, const std::string& networkId, bool reset) {
  if (view.networkId != networkId) {
    ClearView(view, networkId);
    return;
  }
  if (reset) view.ready = false;
}

bool LegacyLoad::Answer(uint32_t generation, LegacyRead which, bool ok, LegacyAnswer answer) {
  if (generation != generation_) return false;
  switch (which) {
    case LegacyRead::Wallets:
      if (answered_.wallets) return false;
      answered_.wallets = true;
      ok_.wallets = ok;
      if (ok) answer_.wallets = std::move(answer.wallets);
      return true;
    case LegacyRead::Payout:
      if (answered_.payout) return false;
      answered_.payout = true;
      ok_.payout = ok;
      if (ok) answer_.payoutId = std::move(answer.payoutId);
      return true;
    case LegacyRead::Payments:
      if (answered_.payments) return false;
      answered_.payments = true;
      ok_.payments = ok;
      if (ok) answer_.payments = std::move(answer.payments);
      return true;
  }
  return false;
}

bool LegacyLoad::Complete() const {
  return answered_.wallets && answered_.payout && answered_.payments;
}

bool LegacyLoad::Commit(LegacyCommitted& view, const std::string& networkId) const {
  if (!Complete()) return false;
  if (view.networkId != networkId) ClearView(view, networkId);
  if (ok_.wallets) view.wallets = answer_.wallets;
  if (ok_.payout && !answer_.payoutId.empty()) view.payoutWalletId = answer_.payoutId;
  if (ok_.payments) view.pendingNanoCents = PendingUsdcNanoCents(answer_.payments);
  view.reads = ok_;
  view.ready = true;
  return true;
}

SolanaPanelView SolanaPanelFor(const LegacyCommitted& view) {
  return SolanaPanelFor(view.ready ? LegacyState::Ready : LegacyState::Loading, view.reads,
                        PayoutWalletFor(view.wallets, view.payoutWalletId),
                        view.pendingNanoCents);
}

bool NeedsPayoutSwitch(const std::string& newWalletId, const std::string& payoutWalletId) {
  return !newWalletId.empty() && newWalletId != payoutWalletId;
}

// ---- the connect sheet ------------------------------------------------------

bool ChooseProvider(ConnectMachine& m) {
  if (InFlight(m.state)) return false;
  m.state = ConnectState::OpeningBrowser;
  ClearFailure(m);
  return true;
}

bool PublicKey(ConnectMachine& m) {
  if (m.state != ConnectState::OpeningBrowser) return false;
  m.state = ConnectState::Linking;
  return true;
}

bool BridgeError(ConnectMachine& m, const std::string& detail) {
  if (m.state != ConnectState::OpeningBrowser) return false;
  Fail(m, detail);
  return true;
}

bool Timeout(ConnectMachine& m) {
  if (m.state != ConnectState::OpeningBrowser && m.state != ConnectState::Linking) return false;
  Fail(m, std::string());
  return true;
}

bool Typed(ConnectMachine& m) {
  // the entry is disabled while linking; nothing it says can matter then
  if (m.state == ConnectState::Linking || m.state == ConnectState::Linked) return false;
  m.address.clear();
  SettleCheck(m, AddressCheck::None);
  return true;
}

bool Debounced(ConnectMachine& m, const std::string& text, bool askServer) {
  if (m.state == ConnectState::Linking || m.state == ConnectState::Linked) return false;
  m.address = Trim(text);
  if (m.address.empty()) {
    SettleCheck(m, AddressCheck::None);
    return false;
  }
  // a new address is a new attempt: the last failure no longer applies
  if (m.state == ConnectState::Failed) {
    m.state = ConnectState::Idle;
    ClearFailure(m);
  }
  if (!LooksLikeSolanaAddress(m.address)) {
    SettleCheck(m, AddressCheck::Invalid);
    return false;
  }
  if (!askServer) {
    SettleCheck(m, AddressCheck::None);
    return false;
  }
  m.check = AddressCheck::Checking;
  // typing while the browser is out checks the address without ending that flow
  if (m.state != ConnectState::OpeningBrowser) m.state = ConnectState::Checking;
  return true;
}

bool Verdict(ConnectMachine& m, ServerVerdict verdict) {
  if (m.check != AddressCheck::Checking) return false;
  switch (verdict) {
    case ServerVerdict::Valid:
      m.check = AddressCheck::Valid;
      break;
    case ServerVerdict::Invalid:
      m.check = AddressCheck::Invalid;
      break;
    case ServerVerdict::Unavailable:
      m.check = AddressCheck::Unavailable;
      break;
  }
  if (m.state == ConnectState::Checking) {
    m.state = m.check == AddressCheck::Valid ? ConnectState::Ready : ConnectState::Idle;
  }
  return true;
}

bool Submit(ConnectMachine& m) {
  if (m.check != AddressCheck::Valid || InFlight(m.state)) return false;
  m.state = ConnectState::Linking;
  ClearFailure(m);
  return true;
}

bool CreateResult(ConnectMachine& m, bool ok, const std::string& walletId,
                  const std::string& detail) {
  if (m.state != ConnectState::Linking) return false;
  if (ok && !walletId.empty()) {
    m.state = ConnectState::Linked;
    ClearFailure(m);
    return true;
  }
  Fail(m, detail);
  return true;
}

bool ProvidersEnabled(const ConnectMachine& m) { return !InFlight(m.state); }

bool EntryEnabled(const ConnectMachine& m) {
  return m.state != ConnectState::Linking && m.state != ConnectState::Linked;
}

bool ConnectEnabled(const ConnectMachine& m) {
  return m.check == AddressCheck::Valid && !InFlight(m.state);
}

const char* StatusKey(const ConnectMachine& m) {
  switch (m.state) {
    case ConnectState::OpeningBrowser: return "opening_wallet_in_browser";
    case ConnectState::Linking: return "connecting_to_wallet";
    default: return "";
  }
}

const char* CheckKey(const ConnectMachine& m) {
  switch (m.check) {
    case AddressCheck::Checking: return "checking_wallet_address";
    case AddressCheck::Invalid: return "invalid_solana_address";
    case AddressCheck::Unavailable: return "something_went_wrong";
    default: return "";
  }
}

bool CheckIsError(const ConnectMachine& m) {
  return m.check == AddressCheck::Invalid || m.check == AddressCheck::Unavailable;
}

const char* FailureKey(const std::string& detail) {
  return detail.empty() ? "something_went_wrong" : "error_connecting_wallet_with_reason";
}

}  // namespace urnw::solana
