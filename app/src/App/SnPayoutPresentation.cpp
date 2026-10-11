// SPDX-License-Identifier: MPL-2.0
#include "SnPayoutPresentation.h"

#include <algorithm>
#include <format>
#include <limits>

namespace urnw::snpayout {

PayoutLineView PayoutLineFor(bool walletKnown, bool hasColdkey, int64_t totalClaimableRao,
                             const std::optional<EpochSchedule>& schedule, int64_t nowMillis,
                             const std::function<std::string(int64_t)>& formatTime) {
  PayoutLineView view;
  if (!walletKnown) return view;
  if (!hasColdkey) {
    view.kind = LineKind::SetColdkey;
    return view;
  }
  view.kind = LineKind::Schedule;
  if (schedule && nowMillis < schedule->endMillis) {
    view.showTimes = true;
    view.epochEnd = formatTime(schedule->endMillis);
    view.claimOpen = formatTime(schedule->claimOpenMillis);
    view.expiry = formatTime(schedule->expiryMillis);
  }
  view.showClaim = totalClaimableRao > 0;
  return view;
}

std::string FormatScheduleTime(int64_t millis, int32_t utcOffsetMinutes) {
  constexpr int64_t kDayMillis = 86'400'000;
  const int64_t local = millis + static_cast<int64_t>(utcOffsetMinutes) * 60'000;
  int64_t days = local / kDayMillis;
  int64_t rest = local % kDayMillis;
  if (rest < 0) {
    rest += kDayMillis;
    --days;
  }
  // civil-from-days (Howard Hinnant)
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const int64_t doe = days - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int64_t day = doy - (153 * mp + 2) / 5 + 1;
  const int64_t month = mp < 10 ? mp + 3 : mp - 9;
  const int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
  const int64_t minutes = rest / 60'000;
  // std::format sizes its own output: GCC cannot bound these fields from the
  // arithmetic above, so an snprintf into a fixed buffer fails the Linux
  // harness build with -Werror=format-truncation
  return std::format("{:04}-{:02}-{:02} {:02}:{:02}", year, month, day, minutes / 60,
                     minutes % 60);
}

LedgerSectionView LedgerSectionFor(bool hasColdkey, bool vault2Configured, LedgerFetch fetch,
                                   const std::vector<LedgerAccount>& accounts, bool canClaim) {
  LedgerSectionView view;
  if (!hasColdkey) return view;
  if (fetch == LedgerFetch::Ready || fetch == LedgerFetch::Loading) {
    for (const LedgerAccount& account : accounts) {
      LedgerAccountView block;
      block.noId = account.noId;
      block.claimableRao = account.claimableRao;
      block.paidRao = account.paidRao;
      block.earnedRao = account.finalizedRao;
      block.showPending = account.pending && account.pendingRao > account.finalizedRao;
      if (block.showPending) block.pendingRao = account.pendingRao - account.finalizedRao;
      view.accounts.push_back(block);
    }
  }
  if (!vault2Configured && view.accounts.empty()) return LedgerSectionView{};
  view.shown = true;
  view.loading = fetch == LedgerFetch::Loading;
  view.poolTitles = view.accounts.size() > 1;
  if (fetch == LedgerFetch::Failed) view.note = LedgerNote::Unavailable;
  if (fetch == LedgerFetch::NoDevice) view.note = LedgerNote::NeedsSession;
  if (fetch == LedgerFetch::Ready && view.accounts.empty()) view.note = LedgerNote::NoAccount;
  // any account with something to claim, never a sum of the ledger's amounts,
  // which an absurd answer could overflow
  const bool anyClaimable = std::any_of(
      view.accounts.begin(), view.accounts.end(),
      [](const LedgerAccountView& account) { return account.claimableRao > 0; });
  view.showClaim = vault2Configured;
  view.claimEnabled = vault2Configured && canClaim && anyClaimable;
  return view;
}

std::string ExplorerTxLink(const std::string& explorerTxUrl, const std::string& txHash) {
  if (explorerTxUrl.empty() || txHash.empty()) return {};
  std::string url = explorerTxUrl;
  const size_t at = url.find("%s");
  if (at != std::string::npos) return url.replace(at, 2, txHash);
  if (url.back() != '/') url += '/';
  return url + txHash;
}

std::string ClaimFailureCode(const std::string& message) {
  std::string code = message.substr(0, message.find(':'));
  while (!code.empty() && (code.back() == ' ' || code.back() == '\t')) code.pop_back();
  return code;
}

std::string ClaimFailureKey(const std::string& message) {
  const std::string code = ClaimFailureCode(message);
  if (code == "needs_gas") return "add_tao_for_gas";
  if (code == "chain_rpc_unreachable" || code == "chain_rpc_error") return "chain_rpc_unreachable";
  if (code == "connect_wallet_first") return "connect_wallet_first";
  if (message.empty() || message == "claim_failed") return "claim_failed";
  return {};
}

LedgerClaimProgress::LedgerClaimProgress(const std::vector<LedgerAccount>& accounts,
                                         std::optional<double> gasTao)
    // A known-empty gas key cannot pay for anything; an unknown balance (the
    // chain did not answer) does not block the attempt, and the SDK's
    // preflight says needs_gas if it comes to that.
    : needsGas_(gasTao.has_value() && *gasTao <= 0.0) {
  for (const LedgerAccount& account : accounts) {
    if (account.claimableRao <= 0) continue;
    LedgerClaimRow row;
    row.noId = account.noId;
    row.index = account.index;
    row.amountRao = account.claimableRao;
    rows_.push_back(row);
  }
}

int64_t LedgerClaimProgress::ClaimableRao() const {
  // every row's amount is at least 0: the constructor takes the claimable
  // accounts, Confirmed keeps a positive amount, and an unlisted row starts at 0
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  int64_t total = 0;
  for (const LedgerClaimRow& row : rows_) {
    if (row.state == LedgerClaimState::Confirmed) continue;
    total = row.amountRao > kMax - total ? kMax : total + row.amountRao;
  }
  return total;
}

bool LedgerClaimProgress::CanClaim(bool allowActions) const {
  return allowActions && !sending_ && !needsGas_ && ClaimableRao() > 0;
}

bool LedgerClaimProgress::Start() {
  if (sending_ || ClaimableRao() <= 0) return false;
  for (LedgerClaimRow& row : rows_) {
    if (row.state == LedgerClaimState::Confirmed) continue;
    row.state = LedgerClaimState::Sending;
    row.message.clear();
  }
  sending_ = true;
  failure_.clear();
  return true;
}

LedgerClaimRow& LedgerClaimProgress::RowFor(int64_t noId, int64_t index) {
  for (LedgerClaimRow& row : rows_) {
    if (row.noId == noId && row.index == index) return row;
  }
  LedgerClaimRow row;
  row.noId = noId;
  row.index = index;
  rows_.push_back(row);
  return rows_.back();
}

void LedgerClaimProgress::Sent(int64_t noId, int64_t index, const std::string& txHash) {
  LedgerClaimRow& row = RowFor(noId, index);
  row.state = LedgerClaimState::Sent;
  row.txHash = txHash;
}

void LedgerClaimProgress::Confirmed(int64_t noId, int64_t index, const std::string& txHash,
                                    int64_t amountRao) {
  LedgerClaimRow& row = RowFor(noId, index);
  row.state = LedgerClaimState::Confirmed;
  row.txHash = txHash;
  if (amountRao > 0) row.amountRao = amountRao;
}

void LedgerClaimProgress::Failed(int64_t noId, int64_t index, const std::string& message) {
  const std::string code = ClaimFailureCode(message);
  // a funding problem rather than an account's: the gas key's funding hint shows
  if (code == "needs_gas") needsGas_ = true;
  if (noId <= 0) {
    failure_ = message;
    return;
  }
  LedgerClaimRow& row = RowFor(noId, index);
  if (code == "already_claimed") {
    row.state = LedgerClaimState::Confirmed;
    return;
  }
  row.state = LedgerClaimState::Failed;
  row.message = message;
}

void LedgerClaimProgress::Done() {
  sending_ = false;
  for (LedgerClaimRow& row : rows_) {
    if (row.state == LedgerClaimState::Sending) row.state = LedgerClaimState::Idle;
  }
}

}  // namespace urnw::snpayout
