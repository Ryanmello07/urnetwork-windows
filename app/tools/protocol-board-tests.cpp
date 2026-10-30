// Executable spec for the Protocol leaderboard and the Earnings Top 200 tile
// (App/ProtocolBoardPresentation.h, owner decision 2026-10-29, LEADERTOP):
// the sdk's two answers read as plain values (a failed call, a server error,
// a page with no snapshot, rows out of range or repeated, fleets with no UID,
// every nullable number as an optional and as the sdk's value with its has_
// flag, computed_at, labels and reasons this build does not know), the
// banners, the progress chart and the point a pointer shows, the claim prompt
// with binding open and closed and close to the line, the own-status card in
// every state, the top-400 list with the Top 200 line, the tint above it, the
// dimmed rows, the badges and the caller's row, the "you" bar, the detail
// sheet, the tile, and the model's fences: the open, the latest list request
// and the latest card request, a failed reload over the rows, a page from
// another snapshot, the seek and the refresh. The WinUI half
// (App/ProtocolBoard.cpp, App/WalletPage.cpp) cannot be built off Windows;
// this is every decision it makes before it touches a XAML object, run on the
// same header the app compiles.
//
//   c++ -std=c++20 -I ../src/App protocol-board-tests.cpp -o /tmp/protocol-board-tests && /tmp/protocol-board-tests
//
// With URNW_PROTOCOL_BOARD_TESTS_SDK the readers also run on the generated
// header's own result types, and the board's two calls run through the
// wrapper's Api::snTop and Api::snTopMe over a fake of the C ABI functions
// they call, which records the JSON the wrapper sends and answers with the
// JSON the sdk's cgo layer writes. The header needs nlohmann/json; both are
// system includes because the generated code does not build with -Wextra
// -Werror:
//
//   c++ -std=c++20 -Wall -Wextra -Werror -DURNW_PROTOCOL_BOARD_TESTS_SDK -I ../src/App -isystem <dir of urnetwork_sdk.hpp> -isystem <dir of nlohmann/> protocol-board-tests.cpp -o ...
//
// SPDX-License-Identifier: MPL-2.0

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ProtocolBoardPresentation.h"

#if defined(URNW_PROTOCOL_BOARD_TESTS_SDK)
#include "urnetwork_sdk.hpp"
#endif

namespace pb = urnw::protocolboard;

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

void CheckText(const std::string& want, const std::string& got, const std::string& what) {
  Check(want == got, what + ": want \"" + want + "\", got \"" + got + "\"");
}

void CheckEq(int64_t want, int64_t got, const std::string& what) {
  Check(want == got, what + ": want " + std::to_string(want) + ", got " + std::to_string(got));
}

void CheckNear(double want, double got, const std::string& what) {
  Check(std::fabs(want - got) < 1e-9, what + ": want " + std::to_string(want) + ", got " + std::to_string(got));
}

// ---- the store's English ------------------------------------------------------
//
// The catalog's own English for every key the presentation reads, with its
// placeholders as the catalog lowers them ({} for one, {0} {1} for two).
// tests/protocol_board_test.go checks each entry against
// Strings/en/Resources.resw, so the cases below read the words the app shows.
const std::map<std::string, std::string, std::less<>> kEnglish = {
    {"anonymous", "Anonymous"},
    {"leaderboard_protocol_banner_closed", "Claiming isn't open yet. You can see where you'd rank now."},
    {"leaderboard_protocol_banner_stale", "Rankings last updated {}."},
    {"leaderboard_protocol_projected", "projected"},
    {"leaderboard_protocol_of_ranked", "of {} ranked networks"},
    {"leaderboard_protocol_above_line", "{} above the Top 200 line"},
    {"leaderboard_protocol_below_line", "{} below the Top 200 line"},
    {"leaderboard_protocol_trend_up", "up {} since the last update"},
    {"leaderboard_protocol_trend_down", "down {} since the last update"},
    {"leaderboard_protocol_why", "Distinct IP prefixes: {0}. Shared with other networks: {1}."},
    {"leaderboard_protocol_solo", "If you claimed alone today: {}."},
    {"leaderboard_protocol_you", "You"},
    {"leaderboard_protocol_label_estimate", "Estimate"},
    {"leaderboard_protocol_label_likely", "Likely"},
    {"leaderboard_protocol_label_borderline", "Borderline"},
    {"leaderboard_protocol_label_unlikely", "Unlikely"},
    {"leaderboard_protocol_label_not_measurable", "Not measurable"},
    {"leaderboard_protocol_chart_title", "Projected rank, last 90 days"},
    {"leaderboard_protocol_chart_line", "Top 200 line"},
    {"leaderboard_protocol_chart_a11y", "Projected rank over 90 days, now {}"},
    {"leaderboard_protocol_claim_inside", "You're projected in the top 200."},
    {"leaderboard_protocol_claim_button", "Claim on Earnings"},
    {"leaderboard_protocol_claim_soon", "Claiming opens soon on the Earnings page."},
    {"leaderboard_protocol_claim_borderline", "You're close to the line, and rankings change every tempo."},
    {"leaderboard_protocol_claim_warning", "A claimed fleet that ranks 201 or lower earns nothing."},
    {"leaderboard_protocol_not_measurable_cohort", "Your devices aren't measured by the validator yet."},
    {"leaderboard_protocol_not_measurable_shared", "Your devices share an IP with another network."},
    {"leaderboard_protocol_not_measurable_providing", "Turn on providing to be ranked."},
    {"leaderboard_protocol_line", "Top 200 line \xC2\xB7 score {}"},
    {"leaderboard_protocol_line_a11y", "Top 200 line, score {}"},
    {"leaderboard_protocol_uid", "UID {}"},
    {"leaderboard_protocol_detail_claimed", "claimed"},
    {"leaderboard_protocol_detail_projected_rank", "Projected rank"},
    {"leaderboard_protocol_detail_validator_rank", "Validator rank"},
    {"leaderboard_protocol_detail_validator_rank_value", "#{0} of {1} claimed"},
    {"leaderboard_protocol_detail_validator_score", "Validator score"},
    {"leaderboard_protocol_detail_replica_score", "Board replica score"},
    {"leaderboard_protocol_detail_incentive", "On-chain incentive"},
    {"leaderboard_protocol_detail_stale", "stale"},
    {"leaderboard_protocol_detail_generation", "Generation"},
    {"leaderboard_protocol_detail_valid_until", "Claim valid until"},
    {"leaderboard_protocol_detail_epoch", "Epoch {}"},
    {"leaderboard_protocol_detail_selected", "Selected"},
    {"leaderboard_protocol_detail_not_selected", "Not selected"},
    {"leaderboard_protocol_detail_incentive_note", "On-chain incentive lags the validator's ranking."},
    {"earnings_top200_projected", "projected \xC2\xB7 {}"},
    {"earnings_top200_body",
     "Ranked like the SN25 validator's Top 200, by the distinct IP prefixes your devices provide from. Head "
     "miners earn SN25\xCE\xB1 every tempo."},
    {"earnings_top200_claiming_soon", "Claiming opens soon"},
    {"earnings_top200_claiming_soon_body",
     "We'll let you know when you can claim your spot. A claimed fleet that ranks 201 or lower earns nothing."},
    {"top200_bound_detail", "Emission is paid to your coldkey directly. Bindings renew per epoch."},
};

std::string English(std::string_view key) {
  const auto found = kEnglish.find(key);
  if (found == kEnglish.end()) {
    Check(false, "the spec has no English for " + std::string(key));
    return std::string(key);
  }
  return found->second;
}

std::string Replace(std::string text, std::string_view placeholder, const std::string& value) {
  const auto at = text.find(placeholder);
  if (at != std::string::npos) text.replace(at, placeholder.size(), value);
  return text;
}

// English numbers: "41,230", one decimal, a whole or one-decimal percent
std::string Grouped(int64_t count) {
  std::string digits = std::to_string(count < 0 ? -count : count);
  std::string out;
  const size_t lead = digits.size() % 3;
  for (size_t i = 0; i < digits.size(); ++i) {
    if (i != 0 && (i + 3 - lead) % 3 == 0) out += ',';
    out += digits[i];
  }
  return count < 0 ? "-" + out : out;
}

std::string OneDecimal(double value) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.1f", value);
  return buffer;
}

std::string Percent(double ratio, int decimals) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.*f%%", decimals, ratio * 100);
  return buffer;
}

pb::Text EnglishText() {
  pb::Text text;
  text.localized = [](std::string_view key) { return English(key); };
  text.format = [](std::string_view key, const std::string& value) {
    return Replace(English(key), "{}", value);
  };
  text.format2 = [](std::string_view key, const std::string& first, const std::string& second) {
    return Replace(Replace(English(key), "{0}", first), "{1}", second);
  };
  text.count = Grouped;
  text.score = OneDecimal;
  text.percent = Percent;
  text.ago = [](int64_t millis) { return "<ago " + std::to_string(millis) + ">"; };
  text.day = [](std::string_view day) { return "<day " + std::string(day) + ">"; };
  return text;
}

// ---- fakes of the generated header's result types ---------------------------------
//
// Two shapes, because the sdk may carry a nullable number either way: as an
// optional member, or as the value with its has_ flag (CONTRACT.md's
// Has<Field>). The readers must read both the same.

namespace optional_shape {
struct ApiError {
  std::string message{};
};
struct SnTopFleet {
  int64_t uid{};
  int64_t generation{};
  int64_t valid_to_epoch{};
  double replica_score{};
  std::optional<double> validator_score;
  std::optional<int64_t> validator_rank;
  std::optional<int64_t> validator_rank_of;
  std::optional<bool> selected;
  std::optional<double> incentive;
  bool incentive_stale{};
};
using SnTopFleetList = std::vector<SnTopFleet>;
struct SnTopRow {
  int64_t position{};
  std::string name{};
  std::string tag{};
  double projected_score{};
  double raw_last_tempo{};
  double margin{};
  int64_t trend{};
  int64_t prefixes{};
  int64_t shared_prefixes{};
  double presence{};
  std::string label{};
  bool measurable{};
  bool verify_eligible{};
  bool claim_prompt{};
  bool is_me{};
  std::optional<SnTopFleetList> fleets;
};
using SnTopRowList = std::vector<SnTopRow>;
struct SnTopSnapshot {
  std::string id{};
  int64_t tempo_index{};
  int64_t block{};
  std::optional<std::string> computed_at;
  int64_t settlement_epoch{};
  int64_t total_ranked{};
  int64_t head_seats{};
  int64_t claimed_fleets{};
  int64_t open_seats{};
  double projected_cutoff{};
  std::optional<double> claimed_cutoff;
  bool binding_open{};
  std::string binding_note{};
  int64_t measured_providers{};
  int64_t cohort_capacity{};
  std::optional<double> precision_at_200;
  bool stale{};
};
struct SnTopResult {
  std::optional<SnTopSnapshot> snapshot;
  std::optional<SnTopRowList> rows;
  std::optional<SnTopRow> me;
  std::optional<std::string> next;
  std::optional<ApiError> error;
};
struct SnTopHistoryPoint {
  std::string day{};
  int64_t rank{};
  double score{};
};
using SnTopHistoryPointList = std::vector<SnTopHistoryPoint>;
struct SnTopMe {
  std::optional<SnTopRow> row;
  double solo_score{};
  int64_t distinct_prefixes{};
  int64_t shared_prefixes{};
  int64_t shared_with_units{};
  std::string not_measurable_reason{};
  std::optional<SnTopHistoryPointList> history;
};
struct SnTopMeResult {
  std::optional<SnTopSnapshot> snapshot;
  std::optional<SnTopMe> me;
  std::optional<ApiError> error;
};
}  // namespace optional_shape

namespace flag_shape {
struct ApiError {
  std::string message{};
};
struct SnTopFleet {
  int64_t uid{};
  int64_t generation{};
  int64_t valid_to_epoch{};
  double replica_score{};
  double validator_score{};
  bool has_validator_score{};
  int64_t validator_rank{};
  bool has_validator_rank{};
  int64_t validator_rank_of{};
  bool has_validator_rank_of{};
  bool selected{};
  bool has_selected{};
  double incentive{};
  bool has_incentive{};
  bool incentive_stale{};
};
using SnTopFleetList = std::vector<SnTopFleet>;
struct SnTopRow {
  int64_t position{};
  std::optional<std::string> name;
  std::optional<std::string> tag;
  double projected_score{};
  double raw_last_tempo{};
  double margin{};
  int64_t trend{};
  int64_t prefixes{};
  int64_t shared_prefixes{};
  double presence{};
  std::string label{};
  bool measurable{};
  bool verify_eligible{};
  bool claim_prompt{};
  bool is_me{};
  std::optional<SnTopFleetList> fleets;
};
using SnTopRowList = std::vector<SnTopRow>;
struct SnTopSnapshot {
  std::string id{};
  int64_t tempo_index{};
  int64_t block{};
  std::string computed_at{};
  int64_t settlement_epoch{};
  int64_t total_ranked{};
  int64_t head_seats{};
  int64_t claimed_fleets{};
  int64_t open_seats{};
  double projected_cutoff{};
  double claimed_cutoff{};
  bool has_claimed_cutoff{};
  bool binding_open{};
  std::string binding_note{};
  int64_t measured_providers{};
  int64_t cohort_capacity{};
  double precision_at_200{};
  bool has_precision_at_200{};
  bool stale{};
};
struct SnTopResult {
  std::optional<SnTopSnapshot> snapshot;
  std::optional<SnTopRowList> rows;
  std::optional<SnTopRow> me;
  std::string next{};
  std::optional<ApiError> error;
};
struct SnTopHistoryPoint {
  std::string day{};
  int64_t rank{};
  double score{};
};
using SnTopHistoryPointList = std::vector<SnTopHistoryPoint>;
struct SnTopMe {
  std::optional<SnTopRow> row;
  double solo_score{};
  int64_t distinct_prefixes{};
  int64_t shared_prefixes{};
  int64_t shared_with_units{};
  std::optional<std::string> not_measurable_reason;
  std::optional<SnTopHistoryPointList> history;
};
struct SnTopMeResult {
  std::optional<SnTopSnapshot> snapshot;
  std::optional<SnTopMe> me;
  std::optional<ApiError> error;
};
}  // namespace flag_shape

struct OptionalTypes {
  using ApiError = optional_shape::ApiError;
  using Fleet = optional_shape::SnTopFleet;
  using Row = optional_shape::SnTopRow;
  using Snapshot = optional_shape::SnTopSnapshot;
  using TopResult = optional_shape::SnTopResult;
  using HistoryPoint = optional_shape::SnTopHistoryPoint;
  using Me = optional_shape::SnTopMe;
  using MeResult = optional_shape::SnTopMeResult;
};

struct FlagTypes {
  using ApiError = flag_shape::ApiError;
  using Fleet = flag_shape::SnTopFleet;
  using Row = flag_shape::SnTopRow;
  using Snapshot = flag_shape::SnTopSnapshot;
  using TopResult = flag_shape::SnTopResult;
  using HistoryPoint = flag_shape::SnTopHistoryPoint;
  using Me = flag_shape::SnTopMe;
  using MeResult = flag_shape::SnTopMeResult;
};

#if defined(URNW_PROTOCOL_BOARD_TESTS_SDK)
struct SdkTypes {
  using ApiError = urnet::ApiError;
  using Fleet = urnet::SnTopFleet;
  using Row = urnet::SnTopRow;
  using Snapshot = urnet::SnTopSnapshot;
  using TopResult = urnet::SnTopResult;
  using HistoryPoint = urnet::SnTopHistoryPoint;
  using Me = urnet::SnTopMe;
  using MeResult = urnet::SnTopMeResult;
};
#endif

// A nullable number set the way the shape carries it.
#define SET_NULLABLE(object, field, value)            \
  do {                                                \
    if constexpr (requires { (object).has_##field; }) { \
      if (value) {                                    \
        (object).field = *(value);                    \
        (object).has_##field = true;                  \
      }                                               \
    } else {                                          \
      if (value) (object).field = *(value);           \
    }                                                 \
  } while (false)

template <typename T>
typename T::Fleet ResultFleet(int64_t uid, std::optional<int64_t> validatorRank, std::optional<int64_t> rankOf,
                              std::optional<bool> selected, std::optional<double> incentive, bool stale) {
  typename T::Fleet fleet{};
  fleet.uid = uid;
  fleet.generation = 2;
  fleet.valid_to_epoch = 16;
  fleet.replica_score = 312.4;
  const std::optional<double> score = validatorRank ? std::optional<double>(312.5) : std::nullopt;
  SET_NULLABLE(fleet, validator_score, score);
  SET_NULLABLE(fleet, validator_rank, validatorRank);
  SET_NULLABLE(fleet, validator_rank_of, rankOf);
  SET_NULLABLE(fleet, selected, selected);
  SET_NULLABLE(fleet, incentive, incentive);
  fleet.incentive_stale = stale;
  return fleet;
}

template <typename T>
typename T::Row ResultRow(int64_t position, std::string name, std::string label, bool measurable,
                          std::vector<typename T::Fleet> fleets = {}) {
  typename T::Row row{};
  row.position = position;
  row.name = std::move(name);
  row.tag = std::string("\xF0\x9F\xA6\x8A");  // a fox
  row.projected_score = 500.0 - static_cast<double>(position);
  row.margin = 0.12;
  row.trend = 3;
  row.prefixes = 18;
  row.shared_prefixes = 3;
  row.label = std::move(label);
  row.measurable = measurable;
  row.claim_prompt = measurable && position <= 200;
  if (!fleets.empty()) row.fleets = std::move(fleets);
  return row;
}

template <typename T>
typename T::Snapshot ResultSnapshot(std::string id, bool bindingOpen, std::optional<double> claimedCutoff,
                                    std::optional<double> precision) {
  typename T::Snapshot snapshot{};
  snapshot.id = std::move(id);
  snapshot.tempo_index = 7;
  snapshot.computed_at = std::string("2026-10-29T15:04:05Z");
  snapshot.total_ranked = 41230;
  snapshot.head_seats = 200;
  snapshot.claimed_fleets = 142;
  snapshot.open_seats = 58;
  snapshot.projected_cutoff = 41.2;
  SET_NULLABLE(snapshot, claimed_cutoff, claimedCutoff);
  snapshot.binding_open = bindingOpen;
  snapshot.binding_note = "Top 200 binding isn't open on mainnet yet";
  SET_NULLABLE(snapshot, precision_at_200, precision);
  return snapshot;
}

// ---- fixtures in the presentation's own values -------------------------------------

constexpr int64_t kComputedAtMillis = 1'793'286'245'000;  // 2026-10-29T15:04:05Z

pb::Snapshot MakeSnapshot(bool bindingOpen = false) {
  pb::Snapshot snapshot;
  snapshot.id = "snap-7";
  snapshot.tempoIndex = 7;
  snapshot.computedAtMillis = kComputedAtMillis;
  snapshot.totalRanked = 41230;
  snapshot.headSeats = 200;
  snapshot.projectedCutoff = 41.2;
  snapshot.bindingOpen = bindingOpen;
  return snapshot;
}

pb::Row MakeRow(int64_t position, std::string name = "", double margin = 0.12, bool measurable = true) {
  pb::Row row;
  row.position = position;
  row.name = std::move(name);
  row.projectedScore = 500.0 - static_cast<double>(position);
  row.margin = margin;
  row.trend = 0;
  row.label = pb::Label::Estimate;
  row.measurable = measurable;
  row.claimPrompt = measurable && position <= 200;
  return row;
}

pb::Fleet MakeFleet(int64_t uid) {
  pb::Fleet fleet;
  fleet.uid = uid;
  fleet.generation = 2;
  fleet.validToEpoch = 16;
  fleet.replicaScore = 312.4;
  return fleet;
}

pb::TopOutcome Page(std::vector<pb::Row> rows, std::string next, std::string snapshotId = "snap-7",
                    bool bindingOpen = false) {
  pb::TopOutcome outcome;
  outcome.ok = true;
  outcome.snapshot = MakeSnapshot(bindingOpen);
  outcome.snapshot->id = std::move(snapshotId);
  outcome.rows = std::move(rows);
  outcome.next = std::move(next);
  return outcome;
}

std::vector<pb::Row> Rows(int64_t from, int64_t to) {
  std::vector<pb::Row> rows;
  for (int64_t position = from; position <= to; ++position) rows.push_back(MakeRow(position));
  return rows;
}

pb::MeOutcome MeAt(int64_t position, double margin = 0.12, bool bindingOpen = false) {
  pb::MeOutcome outcome;
  outcome.ok = true;
  outcome.snapshot = MakeSnapshot(bindingOpen);
  pb::Me me;
  me.row = MakeRow(position, "", margin);
  me.row->isMe = true;
  me.row->trend = 9;
  me.soloScore = 21.44;
  me.distinctPrefixes = 18;
  me.sharedPrefixes = 3;
  me.history = {{"2026-10-27", 190, 40.0}, {"2026-10-28", 180, 41.0}, {"2026-10-29", position, 42.0}};
  outcome.me = std::move(me);
  return outcome;
}

// ---- the cases --------------------------------------------------------------------

void TestLabelsAndReasons() {
  Check(pb::LabelFrom("estimate") == pb::Label::Estimate && pb::LabelFrom("likely") == pb::Label::Likely &&
            pb::LabelFrom("borderline") == pb::Label::Borderline &&
            pb::LabelFrom("unlikely") == pb::Label::Unlikely &&
            pb::LabelFrom("not_measurable") == pb::Label::NotMeasurable,
        "every label the server sends reads");
  Check(pb::LabelFrom("") == pb::Label::None && pb::LabelFrom("certain") == pb::Label::None,
        "a label this build does not know shows no chip");
  CheckText("", pb::LabelKey(pb::Label::None), "no chip has no words");
  Check(pb::ToneOf(pb::Label::Borderline) == pb::Tone::Warning && pb::ToneOf(pb::Label::Likely) == pb::Tone::Positive &&
            pb::ToneOf(pb::Label::Estimate) == pb::Tone::Neutral &&
            pb::ToneOf(pb::Label::NotMeasurable) == pb::Tone::Muted,
        "each chip's colour family");
  Check(pb::ReasonFrom("not_in_cohort") == pb::Reason::NotInCohort &&
            pb::ReasonFrom("shared_egress") == pb::Reason::SharedEgress &&
            pb::ReasonFrom("not_providing") == pb::Reason::NotProviding && pb::ReasonFrom("") == pb::Reason::None &&
            pb::ReasonFrom("asleep") == pb::Reason::None,
        "every reason the server sends reads, and nothing else");
}

template <typename T>
void TestReaders(const std::string& which) {
  using TopResult = typename T::TopResult;
  using MeResult = typename T::MeResult;

  // ---- snTop
  {
    const pb::TopOutcome failed =
        pb::TopOutcomeFrom(std::optional<TopResult>{}, std::optional<std::string>{"timeout"});
    Check(!failed.ok && failed.error == "timeout", which + ": a failed call fails the page");
    const pb::TopOutcome none = pb::TopOutcomeFrom(std::optional<TopResult>{}, std::optional<std::string>{});
    Check(!none.ok && !none.error.empty(), which + ": no result and no error is a failure");
    TopResult refused{};
    refused.error = typename T::ApiError{};
    refused.error->message = "rate limited";
    const pb::TopOutcome serverError = pb::TopOutcomeFrom(std::optional<TopResult>{refused}, std::nullopt);
    Check(!serverError.ok && serverError.error == "rate limited", which + ": the server's error fails the page");
    const pb::TopOutcome noSnapshot =
        pb::TopOutcomeFrom(std::optional<TopResult>{TopResult{}}, std::nullopt);
    Check(!noSnapshot.ok, which + ": a page with no snapshot cannot say which board it is from");

    TopResult page{};
    page.snapshot = ResultSnapshot<T>("snap-7", false, std::nullopt, std::nullopt);
    std::vector<typename T::Row> rows;
    rows.push_back(ResultRow<T>(2, "stellar-mesh", "estimate", true));
    rows.push_back(ResultRow<T>(1, "", "likely", true,
                                {ResultFleet<T>(81, 1, 142, true, 0.018, true), ResultFleet<T>(0, 3, 142, false,
                                                                                               std::nullopt, false)}));
    rows.push_back(ResultRow<T>(2, "a repeat of 2", "estimate", true));
    rows.push_back(ResultRow<T>(0, "position 0", "estimate", true));
    rows.push_back(ResultRow<T>(401, "past 400", "estimate", true));
    rows.push_back(ResultRow<T>(3, "", "not_measurable", false));
    page.rows = std::move(rows);
    page.me = ResultRow<T>(9000, "", "unlikely", true);
    page.next = std::string("cursor-2");
    const pb::TopOutcome read = pb::TopOutcomeFrom(std::optional<TopResult>{page}, std::nullopt);
    Check(read.ok, which + ": a page reads");
    Check(read.snapshot && read.snapshot->id == "snap-7" && read.snapshot->totalRanked == 41230 &&
              read.snapshot->headSeats == 200 && read.snapshot->claimedFleets == 142 &&
              read.snapshot->openSeats == 58,
          which + ": the snapshot's counts");
    Check(read.snapshot && read.snapshot->computedAtMillis == kComputedAtMillis,
          which + ": computed_at in milliseconds");
    Check(read.snapshot && !read.snapshot->claimedCutoff && !read.snapshot->precisionAt200,
          which + ": a null cutoff and precision stay unknown, not 0");
    Check(read.snapshot && !read.snapshot->bindingOpen, which + ": binding is closed");
    CheckEq(3, static_cast<int64_t>(read.rows.size()),
            which + ": rows out of range and a repeated position are left out");
    if (read.rows.size() == 3) {
      CheckEq(1, read.rows[0].position, which + ": by position");
      CheckText("stellar-mesh", read.rows[1].name, which + ": the first row at a position is kept");
      Check(read.rows[0].name.empty() && read.rows[0].label == pb::Label::Likely, which + ": an anonymous row");
      CheckEq(1, static_cast<int64_t>(read.rows[0].fleets.size()), which + ": a fleet with no UID is left out");
      if (!read.rows[0].fleets.empty()) {
        const pb::Fleet& fleet = read.rows[0].fleets[0];
        Check(fleet.uid == 81 && fleet.generation == 2 && fleet.validToEpoch == 16 &&
                  fleet.validatorRank == std::optional<int64_t>(1) &&
                  fleet.validatorRankOf == std::optional<int64_t>(142) &&
                  fleet.selected == std::optional<bool>(true) && fleet.incentive &&
                  std::fabs(*fleet.incentive - 0.018) < 1e-12 && fleet.incentiveStale && fleet.validatorScore,
              which + ": every fleet field");
      }
      Check(!read.rows[2].measurable && read.rows[2].label == pb::Label::NotMeasurable,
            which + ": an unmeasurable row");
      CheckText("\xF0\x9F\xA6\x8A", read.rows[1].tag, which + ": the emoji tag");
    }
    Check(read.me && read.me->position == 9000 && read.me->isMe, which + ": the caller's row wherever it ranks");
    CheckText("cursor-2", read.next, which + ": the next cursor");

    TopResult open{};
    open.snapshot = ResultSnapshot<T>("snap-8", true, 38.5, 0.93);
    open.snapshot->head_seats = 0;
    const pb::TopOutcome opened = pb::TopOutcomeFrom(std::optional<TopResult>{open}, std::nullopt);
    Check(opened.ok && opened.rows.empty() && opened.next.empty() && !opened.me, which + ": an empty page");
    Check(opened.snapshot && opened.snapshot->bindingOpen &&
              opened.snapshot->claimedCutoff == std::optional<double>(38.5) &&
              opened.snapshot->precisionAt200 == std::optional<double>(0.93),
          which + ": a set cutoff and precision read");
    CheckEq(200, opened.snapshot ? opened.snapshot->headSeats : 0, which + ": no seats named is 200");
  }

  // ---- snTopMe
  {
    const pb::MeOutcome failed = pb::MeOutcomeFrom(std::optional<MeResult>{}, std::optional<std::string>{"EOF"});
    Check(!failed.ok && failed.error == "EOF", which + ": a failed call fails the card");
    MeResult refused{};
    refused.error = typename T::ApiError{};
    refused.error->message = "unauthorized";
    Check(!pb::MeOutcomeFrom(std::optional<MeResult>{refused}, std::nullopt).ok,
          which + ": the server's error fails the card");
    const pb::MeOutcome bare = pb::MeOutcomeFrom(std::optional<MeResult>{MeResult{}}, std::nullopt);
    Check(bare.ok && !bare.me, which + ": an answer with no me is a network with nothing to show");

    MeResult answer{};
    answer.snapshot = ResultSnapshot<T>("snap-7", false, std::nullopt, std::nullopt);
    typename T::Me me{};
    me.row = ResultRow<T>(173, "kayak-hollow", "estimate", true);
    me.solo_score = 21.44;
    me.distinct_prefixes = 18;
    me.shared_prefixes = 3;
    me.shared_with_units = 2;
    me.not_measurable_reason = std::string("");
    std::vector<typename T::HistoryPoint> history;
    typename T::HistoryPoint day{};
    day.day = "2026-10-28";
    day.rank = 180;
    day.score = 41.0;
    history.push_back(day);
    day.day = "2026-10-29";
    day.rank = 173;
    history.push_back(day);
    me.history = std::move(history);
    answer.me = me;
    const pb::MeOutcome read = pb::MeOutcomeFrom(std::optional<MeResult>{answer}, std::nullopt);
    Check(read.ok && read.me && read.me->Ranked() && read.me->row->position == 173 && read.me->row->isMe,
          which + ": the caller's row");
    Check(read.me && read.me->soloScore == 21.44 && read.me->distinctPrefixes == 18 &&
              read.me->sharedPrefixes == 3 && read.me->sharedWithUnits == 2 && read.me->reason == pb::Reason::None,
          which + ": why this score");
    Check(read.me && read.me->history.size() == 2 && read.me->history[1].rank == 173 &&
              read.me->history[1].day == "2026-10-29",
          which + ": the history");
    Check(read.snapshot && read.snapshot->id == "snap-7", which + ": the card's snapshot");

    MeResult unranked{};
    typename T::Me away{};
    away.not_measurable_reason = std::string("not_providing");
    unranked.me = away;
    const pb::MeOutcome readAway = pb::MeOutcomeFrom(std::optional<MeResult>{unranked}, std::nullopt);
    Check(readAway.ok && readAway.me && !readAway.me->Ranked() && readAway.me->reason == pb::Reason::NotProviding,
          which + ": a network that is not ranked, and why");

    // the server's own shape for it: me is never null; an unranked network
    // has a row at position 0, labelled not_measurable, and a reason
    MeResult serverUnranked{};
    serverUnranked.snapshot = ResultSnapshot<T>("snap-7", false, std::nullopt, std::nullopt);
    typename T::Me zero{};
    zero.row = ResultRow<T>(0, "", "not_measurable", false);
    zero.not_measurable_reason = std::string("not_providing");
    serverUnranked.me = zero;
    const pb::MeOutcome readZero = pb::MeOutcomeFrom(std::optional<MeResult>{serverUnranked}, std::nullopt);
    Check(readZero.ok && readZero.me && readZero.me->row && readZero.me->row->position == 0 &&
              !readZero.me->Ranked() && readZero.me->reason == pb::Reason::NotProviding,
          which + ": a row at position 0 is the server's unranked network");

    // before the first snapshot: the stand-in with no id, nothing ranked
    TopResult standIn{};
    standIn.snapshot = ResultSnapshot<T>("", false, std::nullopt, std::nullopt);
    standIn.snapshot->total_ranked = 0;
    standIn.rows = std::vector<typename T::Row>{};
    const pb::TopOutcome readStandIn = pb::TopOutcomeFrom(std::optional<TopResult>{standIn}, std::nullopt);
    Check(readStandIn.ok && readStandIn.rows.empty() && readStandIn.next.empty(),
          which + ": the stand-in snapshot reads as a board with nothing ranked");
  }
}

void TestWords() {
  const pb::Text text = EnglishText();
  CheckText("12% above the Top 200 line", pb::MarginText(0.12, text), "a margin above the line");
  CheckText("8% below the Top 200 line", pb::MarginText(-0.08, text), "a margin below it");
  CheckText("0% above the Top 200 line", pb::MarginText(0.0, text), "the 200th row is on the line");
  CheckText("+12%", pb::SignedMarginText(0.12, text), "the tile's margin above the line");
  CheckText("\xE2\x88\x92" "8%", pb::SignedMarginText(-0.08, text), "and below it, with U+2212");
  CheckText("+0%", pb::SignedMarginText(0.0, text), "on the line reads as above");
  CheckText("0.4% above the Top 200 line", pb::MarginText(0.004, text), "under 1%, one decimal, never 0%");
  CheckText("\xE2\x88\x92" "0.4%", pb::SignedMarginText(-0.004, text), "on the tile too");
  CheckText("1% above the Top 200 line", pb::MarginText(0.0096, text), "0.95% and up is a whole percent");
  CheckText("0% above the Top 200 line", pb::MarginText(0.00001, text), "a tenth that rounds to nothing reads 0%");
  CheckText("up 9 since the last update", pb::TrendText(9, text), "a row that moved up");
  CheckText("down 1,200 since the last update", pb::TrendText(-1200, text), "a row that moved down, grouped");
  CheckText("", pb::TrendText(0, text), "a row that did not move says nothing");
  CheckText("#1,240", pb::PositionText(1240, text), "a position, grouped");
  pb::Row named = MakeRow(5, "ridgeline-net");
  CheckText("ridgeline-net", pb::RowName(named, text), "a named row");
  pb::Row anonymous = MakeRow(6);
  CheckText("Anonymous", pb::RowName(anonymous, text), "an anonymous row");
  anonymous.fleets = {MakeFleet(77), MakeFleet(78)};
  CheckText("UID 77, UID 78", pb::RowName(anonymous, text), "an anonymous claimed row shows only its UIDs (Q12)");
  pb::Row spaces = MakeRow(7, "  \t ");
  CheckText("Anonymous", pb::RowName(spaces, text), "a name of spaces is no name");
  named.fleets = {MakeFleet(81)};
  CheckText("ridgeline-net", pb::RowName(named, text), "a named claimed row keeps its name");
}

void TestBanners() {
  const pb::Text text = EnglishText();
  Check(pb::BannersFor(std::nullopt, text).empty(), "no snapshot, no banner");
  pb::Snapshot closed = MakeSnapshot(false);
  auto banners = pb::BannersFor(closed, text);
  Check(banners.size() == 1 && !banners[0].stale &&
            banners[0].text == "Claiming isn't open yet. You can see where you'd rank now.",
        "binding closed: the neutral banner");
  pb::Snapshot open = MakeSnapshot(true);
  Check(pb::BannersFor(open, text).empty(), "binding open: the banner is removed");
  open.stale = true;
  banners = pb::BannersFor(open, text);
  Check(banners.size() == 1 && banners[0].stale &&
            banners[0].text == "Rankings last updated <ago " + std::to_string(kComputedAtMillis) + ">.",
        "a stale snapshot says when it was computed");
  closed.stale = true;
  CheckEq(2, static_cast<int64_t>(pb::BannersFor(closed, text).size()), "closed and stale: both lines");
  closed.computedAtMillis = 0;
  CheckEq(1, static_cast<int64_t>(pb::BannersFor(closed, text).size()),
          "a stale snapshot with no time cannot say when, and says nothing");
}

void TestChart() {
  const pb::Text text = EnglishText();
  Check(!pb::ChartFor({}, 200, text).shown, "no history, no chart");
  Check(!pb::ChartFor({{"yesterday", 10, 1}, {"2026-10-29", 0, 1}}, 200, text).shown,
        "days that do not parse and ranks of 0 draw nothing");
  CheckEq(0, pb::DayNumber("1970-01-01").value_or(-1), "day 0");
  CheckEq(20755, pb::DayNumber("2026-10-29").value_or(-1), "a day's number");
  Check(!pb::DayNumber("2026-13-01") && !pb::DayNumber("2026-10-29T00:00:00Z"), "only a YYYY-MM-DD day");

  const std::vector<pb::HistoryPoint> history = {
      {"2026-10-29", 173, 42}, {"2026-08-01", 412, 20}, {"2026-07-31", 999, 10},  // the 91st day: left out
      {"2026-10-28", 190, 40}, {"2026-10-28", 185, 41},                            // a repeat: the last kept
  };
  const pb::Chart chart = pb::ChartFor(history, 200, text);
  Check(chart.shown, "a history draws");
  CheckEq(3, static_cast<int64_t>(chart.points.size()), "90 days, one point a day");
  if (chart.points.size() == 3) {
    CheckEq(412, chart.points[0].rank, "oldest first");
    CheckNear(0.0, chart.points[0].x, "the window's first day at the left");
    CheckNear(1.0, chart.points[2].x, "the last day at the right");
    CheckEq(185, chart.points[1].rank, "the day's last answer");
    CheckNear(0.0, chart.points[2].y, "the best rank on the chart at the top");
    CheckNear(1.0, chart.points[0].y, "the worst at the bottom");
    CheckText("<day 2026-10-29>", chart.points[2].day, "each point's day in the user's dates");
  }
  CheckNear((200.0 - 173.0) / (412.0 - 173.0), chart.lineY, "the line between them, up is better");
  CheckText("Projected rank over 90 days, now 173", chart.spoken, "the accessible summary");
  CheckText("Projected rank, last 90 days", chart.title, "the title");
  CheckText("Top 200 line", chart.lineLabel, "the line's label");

  const pb::Chart far = pb::ChartFor({{"2026-10-28", 3000, 1}, {"2026-10-29", 2000, 1}}, 200, text);
  CheckNear(0.0, far.lineY, "a network far below still sees the line, at the top");
  const pb::Chart single = pb::ChartFor({{"2026-10-29", 200, 1}}, 200, text);
  Check(single.shown && single.points.size() == 1, "one day is a point");

  Check(!pb::NearestPoint(pb::Chart{}, 0.5), "no points, nothing under the pointer");
  Check(pb::NearestPoint(chart, 0.999) == std::optional<size_t>(2) &&
            pb::NearestPoint(chart, 0.0) == std::optional<size_t>(0),
        "the pointer shows the nearest day");
}

void TestClaimPrompt() {
  const pb::Text text = EnglishText();
  const pb::Snapshot closed = MakeSnapshot(false);
  const pb::Snapshot open = MakeSnapshot(true);

  pb::Row inside = MakeRow(173, "", 0.35);
  pb::ClaimPrompt prompt = pb::ClaimPromptFor(inside, closed, text);
  Check(prompt.shown && !prompt.button, "binding closed: a note, no button");
  CheckText("You're projected in the top 200.", prompt.inside, "the note's first line");
  CheckText("Claiming opens soon on the Earnings page.", prompt.action, "nothing invites registering early");
  CheckText("A claimed fleet that ranks 201 or lower earns nothing.", prompt.warning, "every prompt warns");
  CheckText("", prompt.borderline, "well inside: no closeness line");

  prompt = pb::ClaimPromptFor(inside, open, text);
  Check(prompt.shown && prompt.button, "binding open: the primary button");
  CheckText("Claim on Earnings", prompt.action, "the button's words");
  CheckText("A claimed fleet that ranks 201 or lower earns nothing.", prompt.warning, "and the warning");

  pb::Row close = MakeRow(196, "", 0.03);
  close.label = pb::Label::Estimate;
  prompt = pb::ClaimPromptFor(close, open, text);
  CheckText("You're close to the line, and rankings change every tempo.", prompt.borderline,
            "within 20% of the line, even while labels are estimates (Q17)");
  close.margin = 0.19999;
  Check(!pb::ClaimPromptFor(close, open, text).borderline.empty(), "just inside M");
  close.margin = 0.20;
  Check(pb::ClaimPromptFor(close, open, text).borderline.empty(), "M itself is not close");
  close.label = pb::Label::Borderline;
  Check(!pb::ClaimPromptFor(close, open, text).borderline.empty(), "the borderline label is close, whatever the margin");
  close.label = pb::Label::Likely;
  close.margin = 0.05;
  Check(pb::ClaimPromptFor(close, open, text).borderline.empty(), "once labels are measured, likely is not close");
  close.label = pb::Label::None;
  Check(!pb::ClaimPromptFor(close, open, text).borderline.empty(), "no label reads by the margin, as an estimate");

  pb::Row claimed = MakeRow(40, "", 0.5);
  claimed.fleets = {MakeFleet(81)};
  claimed.claimPrompt = false;  // as the server sends a claimed network
  Check(!pb::ClaimPromptFor(claimed, open, text).shown, "a network that has claimed is not asked to claim");

  // the server's claim_prompt decides (CONTRACT.md, amended 2026-10-29): it is
  // off past 200, for a network it cannot score and for one that claimed
  pb::Row outside = MakeRow(201, "", -0.01);
  Check(!outside.claimPrompt && !pb::ClaimPromptFor(outside, open, text).shown, "201 or lower: no prompt");
  pb::Row unmeasured = MakeRow(12, "", 0.5, false);
  Check(!unmeasured.claimPrompt && !pb::ClaimPromptFor(unmeasured, open, text).shown, "not measurable: no prompt");
  pb::Row unflagged = MakeRow(12);
  unflagged.claimPrompt = false;
  Check(!pb::ClaimPromptFor(unflagged, open, text).shown, "no claim_prompt from the server: no prompt");
  pb::Row line = MakeRow(200, "", 0.0);
  Check(pb::ClaimPromptFor(line, open, text).shown, "the 200th row is inside");
  // ...and the app never second-guesses the flag
  pb::Row flagged = MakeRow(201, "", -0.01, false);
  flagged.claimPrompt = true;
  Check(pb::ClaimPromptFor(flagged, open, text).shown, "the flag decides, the app does not compute it");
}

void TestCard() {
  const pb::Text text = EnglishText();
  pb::Model model;
  model.Open();
  Check(pb::CardFor(model.state(), false, false, "", "", text).body == pb::CardBody::SignIn,
        "signed out: a sign-in prompt in the card's place");
  Check(pb::CardFor(model.state(), true, false, "", "", text).body == pb::CardBody::Loading,
        "before the card's answer: placeholders");
  const uint64_t first = model.BeginMe();
  model.ApplyMe(first, pb::MeOutcome{});
  Check(pb::CardFor(model.state(), true, false, "", "", text).body == pb::CardBody::Failed,
        "a failed first read: the retry row");
  model.BeginMe();
  Check(pb::CardFor(model.state(), true, false, "", "", text).body == pb::CardBody::Loading,
        "a retry shows placeholders again");

  // unranked: the header and why
  pb::Model away;
  away.Open();
  pb::MeOutcome unranked;
  unranked.ok = true;
  unranked.me = pb::Me{};
  unranked.me->reason = pb::Reason::NotProviding;
  away.ApplyMe(away.BeginMe(), unranked);
  pb::Card card = pb::CardFor(away.state(), true, false, "kayak-hollow", "K", text);
  Check(card.body == pb::CardBody::Unranked, "a network that is not ranked");
  CheckText("Turn on providing to be ranked.", card.notMeasurable, "says what helps");
  CheckText("Anonymous", card.name, "with the switch off, the header is Anonymous");
  Check(card.anonymous, "and reads as anonymous");
  CheckText("K", card.tag, "the network's own tag");
  Check(!card.claim.shown, "no prompt");

  // the server's unranked network: a row at position 0, labelled
  // not_measurable, with its reason. The card is Unranked and says why; the
  // tile stays hidden.
  {
    pb::Model zero;
    zero.Open();
    pb::MeOutcome serverUnranked;
    serverUnranked.ok = true;
    serverUnranked.snapshot = MakeSnapshot(true);
    serverUnranked.me = pb::Me{};
    serverUnranked.me->row = MakeRow(0, "", 0.0, false);
    serverUnranked.me->row->label = pb::Label::NotMeasurable;
    serverUnranked.me->row->claimPrompt = false;
    serverUnranked.me->reason = pb::Reason::NotProviding;
    zero.ApplyMe(zero.BeginMe(), serverUnranked);
    const pb::Card unrankedCard = pb::CardFor(zero.state(), true, false, "n", "", text);
    Check(unrankedCard.body == pb::CardBody::Unranked, "a row at position 0 is an unranked card");
    CheckText("Turn on providing to be ranked.", unrankedCard.notMeasurable, "with its reason");
    Check(!unrankedCard.claim.shown && unrankedCard.position.empty(), "and no position or prompt");
    Check(pb::TileFor(zero.state().me, zero.state().meSnapshot, text).body == pb::TileBody::Hidden,
          "the tile stays hidden");
    Check(!pb::YouBarFor(zero.state(), true, text).shown, "and no \"you\" bar points at position 0");
  }

  // ranked, binding closed, inside
  pb::Model board;
  board.Open();
  board.ApplyMe(board.BeginMe(), MeAt(173, 0.12, false));
  card = pb::CardFor(board.state(), true, true, "kayak-hollow", "K", text);
  Check(card.body == pb::CardBody::Ranked, "a ranked network");
  CheckText("kayak-hollow", card.name, "the switch on: the network's name");
  Check(!card.anonymous, "not anonymous");
  CheckText("#173", card.position, "the position");
  CheckText("projected", card.projected, "projected");
  CheckText("of 41,230 ranked networks", card.ofRanked, "of how many");
  CheckText("12% above the Top 200 line", card.margin, "the margin");
  Check(card.above, "above");
  CheckText("up 9 since the last update", card.trend, "the trend");
  CheckEq(1, card.trendDirection, "up");
  CheckText("Estimate", card.labelText, "the chip");
  Check(card.chart.shown && card.chart.points.size() == 3, "the progress chart");
  CheckText("Distinct IP prefixes: 18. Shared with other networks: 3.", card.why, "why this score, counts only");
  CheckText("If you claimed alone today: 21.4.", card.solo, "the solo score (Q4)");
  Check(card.claim.shown && !card.claim.button, "the closed prompt");
  Check(card.uids.empty(), "unclaimed: no UIDs");

  // the row's own name wins over the jwt's while it is shown
  pb::MeOutcome named = MeAt(173);
  named.me->row->name = "public-name";
  board.ApplyMe(board.BeginMe(), named);
  CheckText("public-name", pb::CardFor(board.state(), true, true, "jwt-name", "", text).name,
            "the row's name while the switch is on");
  CheckText("Anonymous", pb::CardFor(board.state(), true, false, "jwt-name", "", text).name,
            "and Anonymous while it is off");
  named.me->row->tag = "\xF0\x9F\xA6\x8A";
  board.ApplyMe(board.BeginMe(), named);
  CheckText("\xF0\x9F\xA6\x8A", pb::CardFor(board.state(), true, true, "", "K", text).tag, "the row's tag first");

  // below the line
  board.ApplyMe(board.BeginMe(), MeAt(260, -0.08));
  card = pb::CardFor(board.state(), true, true, "n", "", text);
  CheckText("8% below the Top 200 line", card.margin, "below");
  Check(!card.above && !card.claim.shown, "below the line: no prompt");

  // not measurable: the reason, the chip, no prompt
  pb::MeOutcome unmeasured = MeAt(150);
  unmeasured.me->row->measurable = false;
  unmeasured.me->row->claimPrompt = false;
  unmeasured.me->row->label = pb::Label::None;
  unmeasured.me->reason = pb::Reason::SharedEgress;
  board.ApplyMe(board.BeginMe(), unmeasured);
  card = pb::CardFor(board.state(), true, true, "n", "", text);
  CheckText("Your devices share an IP with another network.", card.notMeasurable, "why it cannot be scored");
  CheckText("Not measurable", card.labelText, "the chip says so when the server's label does not");
  Check(!card.claim.shown, "and no prompt, whatever the position");
  unmeasured.me->reason = pb::Reason::NotInCohort;
  unmeasured.me->row->label = pb::Label::NotMeasurable;
  board.ApplyMe(board.BeginMe(), unmeasured);
  CheckText("Your devices aren't measured by the validator yet.",
            pb::CardFor(board.state(), true, true, "n", "", text).notMeasurable, "not in the cohort");

  // a measurable row drops a stray reason
  pb::MeOutcome stray = MeAt(150);
  stray.me->reason = pb::Reason::NotInCohort;
  board.ApplyMe(board.BeginMe(), stray);
  CheckText("", pb::CardFor(board.state(), true, true, "n", "", text).notMeasurable,
            "a measurable network shows no reason");

  // claimed
  pb::MeOutcome claimed = MeAt(40, 0.5, true);
  claimed.me->row->fleets = {MakeFleet(81)};
  claimed.me->row->claimPrompt = false;
  board.ApplyMe(board.BeginMe(), claimed);
  card = pb::CardFor(board.state(), true, true, "n", "", text);
  Check(card.uids == std::vector<std::string>{"UID 81"}, "a claimed network's UIDs");
  Check(!card.claim.shown, "a claimed network is not asked to claim again");

  // a failed reread keeps the card
  board.ApplyMe(board.BeginMe(), pb::MeOutcome{});
  Check(pb::CardFor(board.state(), true, true, "n", "", text).body == pb::CardBody::Ranked,
        "a failed reread keeps the card on screen");
}

void TestList() {
  const pb::Text text = EnglishText();
  pb::Model model;
  model.Open();
  Check(pb::ListFor(model.state(), text).body == pb::ListBody::Placeholders, "before the first page: placeholders");
  auto request = model.BeginReload();
  model.ApplyPage(request, pb::TopOutcome{}, 1000);
  Check(pb::ListFor(model.state(), text).body == pb::ListBody::Failed, "a failed first page: the retry row");
  request = model.BeginReload();
  Check(pb::ListFor(model.state(), text).body == pb::ListBody::Placeholders, "a retry: placeholders again");
  model.ApplyPage(request, Page({}, ""), 1000);
  Check(pb::ListFor(model.state(), text).body == pb::ListBody::Empty, "an empty snapshot");
  {
    // no rankings yet: the server's stand-in snapshot has no id
    pb::Model none;
    none.Open();
    pb::TopOutcome standIn = Page({}, "", "");
    standIn.snapshot->totalRanked = 0;
    none.ApplyPage(none.BeginReload(), standIn, 1);
    Check(pb::ListFor(none.state(), text).body == pb::ListBody::Empty, "an empty snapshot id is the empty state");
    Check(!none.BeginNextPage(), "with nothing to page");
  }

  // rows 195-205 around the line
  std::vector<pb::Row> rows = Rows(195, 205);
  rows[1].fleets = {MakeFleet(212)};                // 196, named and claimed
  rows[1].name = "quiet-harbor";
  rows[2].measurable = false;                       // 197, dimmed
  rows[2].label = pb::Label::NotMeasurable;
  rows[7].fleets = {MakeFleet(77)};                 // 202, anonymous and claimed
  rows[8].isMe = true;                              // 203, the caller
  rows[8].label = pb::Label::Unlikely;
  rows[8].trend = -2;
  rows[4].trend = 4;
  request = model.BeginReload();
  pb::TopOutcome around = Page(rows, "cursor-2");
  model.ApplyPage(request, around, 2000);
  pb::ListView view = pb::ListFor(model.state(), text);
  Check(view.body == pb::ListBody::Rows, "rows");
  CheckEq(12, static_cast<int64_t>(view.items.size()), "eleven rows and the line");
  if (view.items.size() == 12) {
    Check(!view.items[5].line && view.items[5].row.position == 200 && view.items[6].line &&
              view.items[7].row.position == 201,
          "the line between 200 and 201");
    CheckText("Top 200 line \xC2\xB7 score 41.2", view.items[6].lineText, "the line names the cutoff's score");
    CheckText("Top 200 line, score 41.2", view.items[6].lineSpoken, "and reads it out");
    Check(view.items[0].row.insideLine && view.items[5].row.insideLine && !view.items[7].row.insideLine,
          "rows at or above the line get the tint");
    const pb::ListRow& claimed = view.items[1].row;
    Check(claimed.claimed && claimed.uids == std::vector<std::string>{"UID 212"}, "the claimed badge");
    CheckText("quiet-harbor", claimed.name, "a named claimed row keeps its name");
    const pb::ListRow& dim = view.items[2].row;
    Check(dim.dimmed && dim.dimmedSpoken == "Not measurable", "an unmeasurable row is dimmed, and says so");
    Check(dim.label == pb::Label::None && dim.labelText.empty(), "the chip is the caller's row's alone");
    const pb::ListRow& anon = view.items[8].row;
    CheckText("UID 77", anon.name, "an anonymous claimed row by its UID");
    Check(!anon.anonymous, "which does not step back like Anonymous");
    Check(anon.uids.empty(), "and carries no badge repeating it");
    const pb::ListRow& plain = view.items[0].row;
    CheckText("Anonymous", plain.name, "an anonymous row");
    Check(plain.anonymous, "steps back");
    const pb::ListRow& me = view.items[9].row;
    Check(me.isMe && me.you == "You", "the caller's row is marked");
    CheckText("Unlikely", me.labelText, "with its chip");
    CheckEq(-1, me.trendDirection, "its trend");
    CheckText("2", me.trend, "the arrow's count");
    CheckText("You, #203, Anonymous, 297.0, down 2 since the last update, Unlikely", me.spoken,
              "the whole row, for a screen reader, the caller first");
    CheckText("#196, quiet-harbor, 304.0, UID 212", claimed.spoken, "a claimed row reads its UIDs after its score");
    CheckText("#197, Anonymous, 303.0, Not measurable", dim.spoken, "a dimmed row says it cannot be scored");
    const pb::ListRow& up = view.items[4].row;
    CheckText("up 4 since the last update", up.trendSpoken, "a row that moved up reads so");
    CheckText("297.0", me.score, "the score");
    CheckText("199", up.positionText, "the position");
  }
  CheckEq(400, view.total, "the indicator spans 1 to 400");
  Check(!view.paging && !view.pageFailed && !view.refreshFailed, "nothing out, nothing failed");

  // the caller's row named by the list's me
  pb::TopOutcome byMe = Page(Rows(1, 3), "");
  byMe.me = MakeRow(2);
  request = model.BeginReload();
  model.ApplyPage(request, byMe, 3000);
  view = pb::ListFor(model.state(), text);
  Check(view.items.size() == 3 && view.items[1].row.isMe && !view.items[0].row.isMe,
        "the caller's row found by its position");
  CheckEq(3, view.total, "a board of three spans three");
  Check(std::none_of(view.items.begin(), view.items.end(), [](const pb::ListItem& item) { return item.line; }),
        "a board that does not reach the line draws none");

  // a failed reload keeps the rows, with the notice
  request = model.BeginReload();
  model.ApplyPage(request, pb::TopOutcome{}, 4000);
  view = pb::ListFor(model.state(), text);
  Check(view.body == pb::ListBody::Rows && view.refreshFailed && view.items.size() == 3,
        "a failed refresh keeps the rows and shows a notice");
}

void TestYouBar() {
  const pb::Text text = EnglishText();
  pb::Model model;
  model.Open();
  Check(!pb::YouBarFor(model.state(), true, text).shown, "nothing known: no bar");
  model.ApplyMe(model.BeginMe(), MeAt(173));
  pb::YouBar bar = pb::YouBarFor(model.state(), true, text);
  Check(bar.shown && bar.inList && bar.position == 173, "the caller's place");
  CheckText("You", bar.you, "named");
  CheckText("#173", bar.positionText, "its position");
  CheckText("12% above the Top 200 line", bar.margin, "and margin");
  Check(!pb::YouBarFor(model.state(), false, text).shown, "signed out: no bar");
  model.ApplyMe(model.BeginMe(), MeAt(5120, -0.6));
  bar = pb::YouBarFor(model.state(), true, text);
  Check(bar.shown && !bar.inList, "past 400 the list cannot reach it");
  CheckText("#5,120", bar.positionText, "grouped");
}

void TestDetail() {
  const pb::Text text = EnglishText();
  pb::Row row = MakeRow(1);
  pb::Fleet fleet = MakeFleet(81);
  fleet.validatorRank = 1;
  fleet.validatorRankOf = 142;
  fleet.validatorScore = 312.44;
  fleet.selected = true;
  fleet.incentive = 0.018;
  fleet.incentiveStale = true;
  pb::Fleet second = MakeFleet(119);
  second.selected = false;
  row.fleets = {fleet, second};
  const pb::Detail detail = pb::DetailFor(row, text);
  CheckText("UID 81, UID 119", detail.title, "an anonymous claimed row's sheet is named by its UIDs");
  CheckText("Anonymous \xC2\xB7 claimed", detail.subtitle, "anonymous, claimed");
  Check(detail.facts.size() == 1 && detail.facts[0].label == "Projected rank" && detail.facts[0].value == "#1",
        "the projected rank");
  CheckEq(2, static_cast<int64_t>(detail.fleets.size()), "a block per fleet");
  if (detail.fleets.size() == 2) {
    const pb::FleetDetail& one = detail.fleets[0];
    CheckText("UID 81", one.title, "the UID");
    Check(one.isSelected && one.selected == "Selected", "selected this tempo");
    const std::vector<pb::Fact> want = {
        {"Validator rank", "#1 of 142 claimed"}, {"Validator score", "312.4"},
        {"Board replica score", "312.4"},       {"On-chain incentive", "1.8% \xC2\xB7 stale"},
        {"Generation", "2"},                    {"Claim valid until", "Epoch 16"},
    };
    Check(one.facts == want, "every fact, the incentive flagged stale");
    const pb::FleetDetail& two = detail.fleets[1];
    Check(!two.isSelected && two.selected == "Not selected", "not selected");
    Check(two.facts.size() == 3 && two.facts[0].label == "Board replica score",
          "before the validator says, only the board's own facts");
  }
  CheckText("On-chain incentive lags the validator's ranking.", detail.note, "the incentive's lag");
  pb::Row named = MakeRow(3, "harbor-lights");
  named.fleets = {MakeFleet(5)};
  const pb::Detail plain = pb::DetailFor(named, text);
  CheckText("harbor-lights", plain.title, "a named row's sheet is its name");
  CheckText("harbor-lights \xC2\xB7 claimed", plain.subtitle, "named, claimed");
  CheckText("", plain.note, "no incentive, no note");
  Check(plain.fleets.size() == 1 && plain.fleets[0].selected.empty(), "no selection before the validator says");
}

void TestTile() {
  const pb::Text text = EnglishText();
  pb::MeOutcome closed = MeAt(173, 0.12, false);
  Check(pb::TileFor(std::nullopt, closed.snapshot, text).body == pb::TileBody::Hidden, "no answer: hidden");
  Check(pb::TileFor(closed.me, std::nullopt, text).body == pb::TileBody::Hidden, "no snapshot: hidden");
  pb::Tile tile = pb::TileFor(closed.me, closed.snapshot, text);
  Check(tile.body == pb::TileBody::Claimable, "projected inside: the tile shows");
  CheckText("#173", tile.position, "the same position as the Protocol tab");
  CheckText("projected \xC2\xB7 +12%", tile.projected, "and margin, signed");
  CheckText("Estimate", tile.labelText, "and label");
  Check(!tile.claimButton && !tile.signButton, "binding closed: no claim, no sign");
  CheckText("Claiming opens soon", tile.soonTitle, "the note instead");
  CheckText("We'll let you know when you can claim your spot. A claimed fleet that ranks 201 or lower earns nothing.",
            tile.soonBody, "and what it means");
  Check(tile.about.find("SN25") != std::string::npos, "what the rank is");

  pb::MeOutcome open = MeAt(173, 0.12, true);
  tile = pb::TileFor(open.me, open.snapshot, text);
  Check(tile.claimButton && tile.signButton, "binding open: the existing claim flow");
  CheckText("A claimed fleet that ranks 201 or lower earns nothing.", tile.warning, "with the warning");
  Check(tile.soonTitle.empty(), "and no note");

  pb::MeOutcome outside = MeAt(230, -0.1, true);
  Check(pb::TileFor(outside.me, outside.snapshot, text).body == pb::TileBody::Hidden,
        "outside the top 200 and unclaimed: hidden");
  pb::MeOutcome unmeasured = MeAt(100, 0.6, true);
  unmeasured.me->row->measurable = false;
  unmeasured.me->row->claimPrompt = false;
  Check(pb::TileFor(unmeasured.me, unmeasured.snapshot, text).body == pb::TileBody::Hidden,
        "not measurable and unclaimed: hidden");

  // GET /sn/top/me failing (a 404 until the server ships the route, or any
  // error) hides the tile, as the old head read's failure did
  pb::Model failing;
  failing.Open();
  pb::MeOutcome notFound;
  notFound.error = "404 Not Found";
  failing.ApplyMe(failing.BeginMe(), notFound);
  Check(pb::TileFor(failing.state().me, failing.state().meSnapshot, text).body == pb::TileBody::Hidden,
        "a failed card read hides the tile");
  failing.ApplyMe(failing.BeginMe(), MeAt(173, 0.12, true));
  failing.ApplyMe(failing.BeginMe(), notFound);
  Check(pb::TileFor(failing.state().me, failing.state().meSnapshot, text).body == pb::TileBody::Claimable,
        "a failed reread keeps the tile it had");

  pb::MeOutcome claimed = MeAt(230, -0.1, true);
  pb::Fleet fleet = MakeFleet(81);
  fleet.validatorRank = 150;
  fleet.validatorRankOf = 142;
  fleet.selected = false;
  claimed.me->row->fleets = {fleet};
  tile = pb::TileFor(claimed.me, claimed.snapshot, text);
  Check(tile.body == pb::TileBody::Claimed, "a claimed network's tile, wherever it ranks");
  CheckText("projected \xC2\xB7 \xE2\x88\x92" "10%", tile.projected, "below the line, with a minus sign");
  Check(tile.fleets.size() == 1 && tile.fleets[0].uid == "UID 81" &&
            tile.fleets[0].validatorRank == "#150 of 142 claimed" && tile.fleets[0].selected == "Not selected" &&
            !tile.fleets[0].isSelected,
        "its UID, validator rank and selection, as the detail sheet");
  Check(!tile.claimButton && tile.signButton, "binding open: sign, not claim");
  Check(tile.nearLine, "below the line: the demotion warning");
  CheckText("Emission is paid to your coldkey directly. Bindings renew per epoch.", tile.boundDetail,
            "how a claimed fleet is paid");
  claimed.snapshot->bindingOpen = false;
  tile = pb::TileFor(claimed.me, claimed.snapshot, text);
  Check(!tile.signButton && tile.soonTitle.empty(), "binding closed: nothing to sign, nothing to claim");
  claimed.me->row->margin = 0.4;
  claimed.me->row->position = 50;
  Check(!pb::TileFor(claimed.me, claimed.snapshot, text).nearLine, "well inside: no warning");
}

void TestGeometry() {
  // rows 52 tall from 100, the 34 line after the row at index 199 (position 200)
  CheckNear(100.0, pb::RowTop(0, 100, 52, 34, 199), "the first row at the top of the rows");
  CheckNear(100.0 + 199 * 52, pb::RowTop(199, 100, 52, 34, 199), "the 200th row above the line");
  CheckNear(100.0 + 200 * 52 + 34, pb::RowTop(200, 100, 52, 34, 199), "the 201st row below it");
  CheckNear(100.0 + 5 * 52, pb::RowTop(5, 100, 52, 34, -1), "no line, no gap");
  CheckEq(0, pb::FirstVisibleIndex(0, 100, 52, 34, 199, 400), "a view above the rows reads as the first");
  CheckEq(0, pb::FirstVisibleIndex(151, 100, 52, 34, 199, 400), "inside the first row");
  CheckEq(1, pb::FirstVisibleIndex(152, 100, 52, 34, 199, 400), "the second row's top");
  CheckEq(199, pb::FirstVisibleIndex(100 + 199 * 52 + 51, 100, 52, 34, 199, 400), "the 200th row");
  CheckEq(200, pb::FirstVisibleIndex(100 + 200 * 52, 100, 52, 34, 199, 400), "the line reads as the row after it");
  CheckEq(200, pb::FirstVisibleIndex(100 + 200 * 52 + 34, 100, 52, 34, 199, 400), "the 201st row's top");
  CheckEq(200, pb::FirstVisibleIndex(100 + 200 * 52 + 34 + 51, 100, 52, 34, 199, 400), "inside the 201st");
  CheckEq(201, pb::FirstVisibleIndex(100 + 201 * 52 + 34, 100, 52, 34, 199, 400), "the 202nd past the line");
  CheckEq(399, pb::FirstVisibleIndex(1e9, 100, 52, 34, 199, 400), "past the end reads as the last");
  CheckEq(0, pb::FirstVisibleIndex(500, 100, 52, 34, 199, 0), "no rows");
  for (int64_t index : {0, 1, 150, 199, 200, 201, 399}) {
    CheckEq(index, pb::FirstVisibleIndex(pb::RowTop(index, 100, 52, 34, 199), 100, 52, 34, 199, 400),
            "a row's top reads as that row (" + std::to_string(index) + ")");
  }
}

void TestModel() {
  pb::Model model;
  Check(!model.Admits(0) && !model.Admits(1), "nothing open admits nothing");
  const uint64_t first = model.Open();
  Check(model.Admits(first), "the open board admits its own answers");
  auto request = model.BeginReload();
  Check(model.state().listLoading, "a reload is out");
  const uint64_t second = model.Open();
  Check(!model.Admits(first) && model.Admits(second), "a reopen drops the earlier open's answers");
  Check(model.ApplyPage(request, Page(Rows(1, 50), "c2"), 1) == pb::Model::Applied::Dropped,
        "a page from before the reopen is not the latest");
  Check(!model.state().listLoaded, "and changes nothing");

  // the latest reload wins
  const auto older = model.BeginReload();
  const auto newer = model.BeginReload();
  Check(model.ApplyPage(newer, Page(Rows(1, 50), "c2", "snap-8"), 10) == pb::Model::Applied::Applied,
        "the newest reload applies");
  Check(model.ApplyPage(older, Page(Rows(1, 50), "c2", "snap-7"), 11) == pb::Model::Applied::Dropped,
        "an older one, answering after it, is dropped");
  CheckText("snap-8", model.state().snapshot ? model.state().snapshot->id : "", "the newest snapshot stays");
  CheckEq(10, model.state().loadedAtMillis, "loaded when the newest landed");

  // paging
  auto next = model.BeginNextPage();
  Check(next && next->cursor == "c2" && next->limit == pb::kPageSize && !next->first, "the next page by cursor");
  Check(!model.BeginNextPage(), "one page at a time");
  Check(model.ApplyPage(*next, Page(Rows(45, 100), "c3", "snap-8"), 12) == pb::Model::Applied::Applied,
        "the next page applies");
  CheckEq(100, static_cast<int64_t>(model.state().rows.size()), "an overlapping page adds only past the rows");
  CheckEq(100, model.state().rows.back().position, "in order");
  next = model.BeginNextPage();
  Check(model.ApplyPage(*next, pb::TopOutcome{}, 13) == pb::Model::Applied::Applied, "a failed page");
  Check(model.state().pageFailed && model.state().rows.size() == 100, "keeps the rows, and its retry");
  next = model.BeginNextPage();
  Check(next.has_value() && !model.state().pageFailed, "the retry is the next page again");
  Check(model.ApplyPage(*next, Page(Rows(101, 150), "c4", "snap-9"), 14) == pb::Model::Applied::Restart,
        "a page from another snapshot restarts the list");
  CheckEq(100, static_cast<int64_t>(model.state().rows.size()), "without mixing its rows in");
  // a reload while a page is out drops the page
  model.BeginNextPage();
  const auto reload = model.BeginReload();
  Check(!model.state().pageLoading, "a reload drops a page still out");
  model.ApplyPage(reload, Page(Rows(1, 50), "c2", "snap-9"), 20);

  // A next page whose snapshot expired: the server keeps snapshots 7 days and
  // refuses the cursor with HTTP 410, which the sdk delivers as "<status
  // line>: <body>". Re-sending that cursor fails forever, so the board
  // reloads from the first page of the newest snapshot instead.
  {
    pb::Model expiring;
    expiring.Open();
    expiring.ApplyPage(expiring.BeginReload(), Page(Rows(1, 50), "c2", "snap-old"), 1);
    auto stale = expiring.BeginNextPage();
    pb::TopOutcome gone;
    gone.error = "410 Gone: The snapshot expired. Reload from the top.";
    Check(stale && expiring.ApplyPage(*stale, gone, 2) == pb::Model::Applied::Restart,
          "a next page whose snapshot expired reloads from the top");
    Check(!expiring.state().pageFailed && !expiring.state().pageLoading,
          "and offers no retry that would send the dead cursor again");
    CheckEq(50, static_cast<int64_t>(expiring.state().rows.size()), "the rows stay until the reload lands");
    const auto reload = expiring.BeginReload();
    Check(reload.first && reload.cursor.empty(), "the reload is the first page, with no cursor");
    expiring.ApplyPage(reload, Page(Rows(1, 50), "c2-new", "snap-new"), 3);
    CheckText("snap-new", expiring.state().snapshot ? expiring.state().snapshot->id : "",
              "the newest snapshot replaces the expired one");
    auto fresh = expiring.BeginNextPage();
    Check(fresh && fresh->cursor == "c2-new", "and pages on with its own cursor");

    // other refusals of a next page keep the retry
    for (const char* error : {"404 Not Found: Unknown snapshot.", "timeout",
                              "Get \"https://api.example/sn/top?cursor=410\": EOF"}) {
      pb::Model other;
      other.Open();
      other.ApplyPage(other.BeginReload(), Page(Rows(1, 50), "c2"), 1);
      auto page = other.BeginNextPage();
      pb::TopOutcome failed;
      failed.error = error;
      Check(page && other.ApplyPage(*page, failed, 2) == pb::Model::Applied::Applied && other.state().pageFailed,
            std::string("a next page refused otherwise keeps its retry: ") + error);
    }
    // a first page has no cursor to expire: a 410 there is a failure, never a loop
    pb::Model first;
    first.Open();
    pb::TopOutcome gone2;
    gone2.error = "410 Gone: The snapshot expired. Reload from the top.";
    Check(first.ApplyPage(first.BeginReload(), gone2, 1) == pb::Model::Applied::Applied && first.state().listFailed,
          "a first page answered 410 fails rather than reloading forever");
  }

  // the end and 400
  pb::Model full;
  full.Open();
  full.ApplyPage(full.BeginReload(), Page(Rows(1, 400), "c9"), 1);
  Check(!full.BeginNextPage(), "the list stops at 400");
  pb::Model ended;
  ended.Open();
  ended.ApplyPage(ended.BeginReload(), Page(Rows(1, 30), ""), 1);
  Check(!ended.BeginNextPage(), "and at the end");
  pb::Model unloaded;
  unloaded.Open();
  Check(!unloaded.BeginNextPage(), "and before the first page");

  // the seek
  pb::Model seek;
  seek.Open();
  seek.ApplyPage(seek.BeginReload(), Page(Rows(1, 50), "c2"), 1);
  Check(!seek.Seek(30), "a loaded position needs no pages");
  Check(!seek.TakeLandedSeek(), "and leaves nothing to land");
  Check(seek.Seek(350) && seek.WantsSeekPage(), "a position past the rows loads through");
  auto page = seek.BeginNextPage(pb::kSeekPageSize);
  Check(page && page->limit == pb::kSeekPageSize, "with the API's largest pages");
  Check(!seek.WantsSeekPage(), "one page at a time");
  seek.ApplyPage(*page, Page(Rows(51, 250), "c3"), 2);
  Check(!seek.TakeLandedSeek() && seek.WantsSeekPage(), "short of it: another page");
  page = seek.BeginNextPage(pb::kSeekPageSize);
  seek.ApplyPage(*page, Page(Rows(251, 400), ""), 3);
  Check(seek.TakeLandedSeek() == std::optional<int64_t>(350), "then it lands, once");
  Check(!seek.TakeLandedSeek(), "and only once");
  Check(!seek.Seek(9999), "past 400 is 400, which is loaded");
  pb::Model shortBoard;
  shortBoard.Open();
  shortBoard.ApplyPage(shortBoard.BeginReload(), Page(Rows(1, 50), "c2"), 1);
  shortBoard.Seek(300);
  page = shortBoard.BeginNextPage(pb::kSeekPageSize);
  shortBoard.ApplyPage(*page, Page(Rows(51, 120), ""), 2);
  Check(!shortBoard.TakeLandedSeek() && !shortBoard.WantsSeekPage(), "a board that ends short gives the seek up");
  pb::Model failing;
  failing.Open();
  failing.ApplyPage(failing.BeginReload(), Page(Rows(1, 50), "c2"), 1);
  failing.Seek(300);
  page = failing.BeginNextPage(pb::kSeekPageSize);
  failing.ApplyPage(*page, pb::TopOutcome{}, 2);
  Check(!failing.WantsSeekPage() && !failing.TakeLandedSeek(), "a failed page gives the seek up");

  // the refresh
  pb::Model refresh;
  refresh.Open();
  Check(!refresh.ShouldRefresh(1'000'000), "nothing loaded: nothing to refresh");
  refresh.ApplyPage(refresh.BeginReload(), Page(Rows(1, 5), ""), 1'000'000);
  Check(!refresh.ShouldRefresh(1'000'000 + pb::kRefreshAfterMillis - 1), "a fresh board stays");
  Check(refresh.ShouldRefresh(1'000'000 + pb::kRefreshAfterMillis), "an old one reloads when shown");
  refresh.BeginReload();
  Check(!refresh.ShouldRefresh(9'000'000), "not while a reload is out");

  // the card's read
  pb::Model card;
  card.Open();
  const uint64_t meOlder = card.BeginMe();
  const uint64_t meNewer = card.BeginMe();
  Check(card.ApplyMe(meNewer, MeAt(10)), "the newest card read applies");
  Check(!card.ApplyMe(meOlder, MeAt(99)), "an older one is dropped");
  CheckEq(10, card.state().me->row->position, "the newest stays");
  Check(card.ApplyMe(card.BeginMe(), pb::MeOutcome{}) && card.state().meFailed && card.state().me,
        "a failed reread keeps the card");
  card.Close();
  Check(!card.state().me && !card.Admits(card.Current()), "a close forgets the account");
}

}  // namespace

#if defined(URNW_PROTOCOL_BOARD_TESTS_SDK)
// ---- the binding path, over a fake of the C ABI the wrapper calls ----------------

namespace abi {
constexpr uint64_t kApi = 0x51;
struct Answer {
  const char* resultJson = nullptr;
  const char* err = nullptr;
};
Answer gAnswer;
int gCalls = 0;
uint64_t gSentTo = 0;
std::string gSentArgs;
}  // namespace abi

extern "C" {

void urnet_free_string(char* s) { std::free(s); }

bool urnet_release(uint64_t) { return true; }

void urnet_api_sn_top(uint64_t self, const char* args_json, urnet_sn_top_cb callback_result,
                      void* callback_user_data) {
  ++abi::gCalls;
  abi::gSentTo = self;
  abi::gSentArgs = args_json ? args_json : "";
  if (callback_result) callback_result(callback_user_data, abi::gAnswer.resultJson, abi::gAnswer.err);
}

void urnet_api_sn_top_me(uint64_t self, urnet_sn_top_me_cb callback_result, void* callback_user_data) {
  ++abi::gCalls;
  abi::gSentTo = self;
  abi::gSentArgs.clear();
  if (callback_result) callback_result(callback_user_data, abi::gAnswer.resultJson, abi::gAnswer.err);
}

}  // extern "C"

namespace {

// The board's two calls, made as ProtocolBoard.cpp makes them, through the
// generated wrapper and over the fake above.
void TestSdkCalls() {
  const urnet::Api api(abi::kApi);

  urnet::SnTopArgs args;
  args.limit = pb::kPageSize;
  args.cursor = "c2";
  abi::gAnswer = {
      R"({"snapshot":{"id":"snap-7","tempo_index":7,"computed_at":"2026-10-29T15:04:05Z","total_ranked":41230,)"
      R"("head_seats":200,"claimed_fleets":142,"open_seats":58,"projected_cutoff":41.2,"binding_open":false,)"
      R"("binding_note":"Top 200 binding isn't open on mainnet yet","stale":false},)"
      R"("rows":[{"position":1,"name":"","tag":"","projected_score":312.4,"margin":6.5,"trend":0,"prefixes":40,)"
      R"("shared_prefixes":2,"label":"estimate","measurable":true,"claim_prompt":true,"is_me":false,)"
      R"("fleets":[{"uid":81,"generation":2,"valid_to_epoch":16,"replica_score":312.4,"incentive_stale":false}]},)"
      R"({"position":2,"name":"stellar-mesh","tag":"","projected_score":288.1,"margin":6.0,"trend":1,"prefixes":30,)"
      R"("shared_prefixes":0,"label":"estimate","measurable":true,"claim_prompt":true,"is_me":false}],)"
      R"("next":"c3"})",
      nullptr};
  std::optional<pb::TopOutcome> top;
  api.snTop(args, [&top](std::optional<urnet::SnTopResult> result, std::optional<std::string> err) {
    top = pb::TopOutcomeFrom(result, err);
  });
  Check(abi::gSentTo == abi::kApi, "snTop calls on the board's Api");
  Check(abi::gSentArgs.find("\"cursor\":\"c2\"") != std::string::npos &&
            abi::gSentArgs.find("\"limit\":50") != std::string::npos,
        "snTop sends the cursor and the page size: " + abi::gSentArgs);
  Check(top && top->ok && top->rows.size() == 2 && top->next == "c3", "the sdk's page reads");
  if (top && top->rows.size() == 2) {
    Check(top->rows[0].fleets.size() == 1 && top->rows[0].fleets[0].uid == 81, "with its fleets");
    CheckText("stellar-mesh", top->rows[1].name, "and names");
  }
  abi::gAnswer = {nullptr, "Get \"https://api.example/sn/top\": EOF"};
  top.reset();
  api.snTop(args, [&top](std::optional<urnet::SnTopResult> result, std::optional<std::string> err) {
    top = pb::TopOutcomeFrom(result, err);
  });
  Check(top && !top->ok, "a failed request, delivered by the sdk");

  // the server's 410 for a cursor whose snapshot expired, as the sdk delivers
  // it (connect's HttpStatusError: "<status line>: <body>"), restarts the list
  abi::gAnswer = {nullptr, "410 Gone: The snapshot expired. Reload from the top."};
  top.reset();
  api.snTop(args, [&top](std::optional<urnet::SnTopResult> result, std::optional<std::string> err) {
    top = pb::TopOutcomeFrom(result, err);
  });
  pb::Model expiring;
  expiring.Open();
  expiring.ApplyPage(expiring.BeginReload(), Page(Rows(1, 50), "c2"), 1);
  auto next = expiring.BeginNextPage();
  Check(top && !top->ok && next && expiring.ApplyPage(*next, std::move(*top), 2) == pb::Model::Applied::Restart,
        "the sdk's 410 for an expired cursor reloads the board from the top");

  abi::gAnswer = {
      R"({"snapshot":{"id":"snap-7","total_ranked":41230,"head_seats":200,"projected_cutoff":41.2,"binding_open":true},)"
      R"("me":{"row":{"position":173,"name":"","tag":"","projected_score":42.6,"margin":0.12,"trend":9,)"
      R"("label":"estimate","measurable":true,"claim_prompt":true,"is_me":true},"solo_score":21.4,)"
      R"("distinct_prefixes":18,"shared_prefixes":3,"shared_with_units":2,"not_measurable_reason":"",)"
      R"("history":[{"day":"2026-10-28","rank":180,"score":41.0},{"day":"2026-10-29","rank":173,"score":42.6}]}})",
      nullptr};
  std::optional<pb::MeOutcome> me;
  api.snTopMe([&me](std::optional<urnet::SnTopMeResult> result, std::optional<std::string> err) {
    me = pb::MeOutcomeFrom(result, err);
  });
  Check(me && me->ok && me->me && me->me->Ranked() && me->me->row->position == 173 && me->me->history.size() == 2,
        "the sdk's card reads");
  Check(me && me->snapshot && me->snapshot->bindingOpen, "with its snapshot");

  // the server's unranked network, as Go writes it: me never null, a row at
  // position 0 labelled not_measurable, and the reason
  abi::gAnswer = {
      R"({"snapshot":{"id":"","head_seats":200,"open_seats":200,"binding_open":false},)"
      R"("me":{"row":{"position":0,"name":"","tag":"","projected_score":0,"margin":0,"trend":0,)"
      R"("label":"not_measurable","measurable":false,"claim_prompt":false,"is_me":true},"solo_score":0,)"
      R"("distinct_prefixes":0,"shared_prefixes":0,"shared_with_units":0,"not_measurable_reason":"not_providing",)"
      R"("history":[]}})",
      nullptr};
  me.reset();
  api.snTopMe([&me](std::optional<urnet::SnTopMeResult> result, std::optional<std::string> err) {
    me = pb::MeOutcomeFrom(result, err);
  });
  Check(me && me->ok && me->me && !me->me->Ranked() && me->me->reason == pb::Reason::NotProviding,
        "the sdk's unranked network reads as unranked, with its reason");
  if (me) {
    pb::Model zero;
    zero.Open();
    zero.ApplyMe(zero.BeginMe(), std::move(*me));
    const pb::Text text = EnglishText();
    Check(pb::CardFor(zero.state(), true, false, "n", "", text).body == pb::CardBody::Unranked &&
              pb::TileFor(zero.state().me, zero.state().meSnapshot, text).body == pb::TileBody::Hidden,
          "its card is unranked and its tile hidden");
  }
}

}  // namespace
#endif

int main() {
  TestLabelsAndReasons();
  TestReaders<OptionalTypes>("optional members");
  TestReaders<FlagTypes>("has_ flags");
#if defined(URNW_PROTOCOL_BOARD_TESTS_SDK)
  TestReaders<SdkTypes>("sdk header");
  TestSdkCalls();
#endif
  TestWords();
  TestBanners();
  TestChart();
  TestClaimPrompt();
  TestCard();
  TestList();
  TestYouBar();
  TestDetail();
  TestTile();
  TestGeometry();
  TestModel();
  std::cout << "protocol board: " << gCases << " cases, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
