// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "ProtocolBoard.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <winrt/Microsoft.UI.Input.h>  // PointerPoint: the thumb's and the chart's pointer
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.Text.h>

#include "LeaderboardIndicator.h"
#include "Localization.h"
#include "Log.h"
#include "MainWindow.xaml.h"
#include "PageContext.h"
#include "Sdk.h"      // urnet::Api's snTop and snTopMe and their results
#include "SdkHost.h"  // Sdk().api()
#include "SettingsSheets.h"  // rows::MakeSheet, rows::Lookup
#include "SheetFit.h"        // sheets clamp to the window at open time
#include "StatsFormat.h"     // the user's dates and how long ago
#include "Strings.h"
#include "UrColors.h"
#include "UrComponents.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using urnw::pages::H;
using urnw::pages::Loc;
using urnw::pages::Sdk;

namespace urnw {

using winrt::Windows::Foundation::IInspectable;
namespace pb = protocolboard;

namespace {

// two lines beside the position: the name (with the caller's chips) over the tag
constexpr double kRowHeight = 52;
constexpr double kLineHeight = 34;
constexpr int kPlaceholderRows = 8;
constexpr double kChartHeight = 96;
constexpr double kChartPad = 8;  // the line and the rank stay off the chart's edges

constexpr wchar_t kUnmeasuredGlyph[] = L"\uE711";  // Cancel: a row the validator cannot score
constexpr wchar_t kUpGlyph[] = L"\uE70E";        // ChevronUp: moved up, and the "you" bar
constexpr wchar_t kDownGlyph[] = L"\uE70D";      // ChevronDown: moved down

// the position indicator (the points board's metrics)
constexpr double kIndicatorWidth = 32.0;
constexpr double kIndicatorPad = 8.0;
constexpr double kIndicatorTrackWidth = 6.0;

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

hstring Utf8(std::string const& text) { return hstring{Widen(text)}; }

TextBlock MakeText(hstring const& text, double fontSize, Brush const& brush = nullptr, bool wrap = false) {
  TextBlock block;
  block.Text(text);
  block.FontSize(fontSize);
  if (brush) block.Foreground(brush);
  if (wrap) block.TextWrapping(TextWrapping::Wrap);
  return block;
}

FontIcon MakeGlyph(wchar_t const* glyph, double size, Brush const& brush) {
  FontIcon icon;
  icon.FontFamily(FontFamily(kit::kIconFontFamily));
  icon.Glyph(glyph);
  icon.FontSize(size);
  icon.Foreground(brush);
  return icon;
}

ColumnDefinition Column(GridLength const& width) {
  ColumnDefinition column;
  column.Width(width);
  return column;
}

void SetRaw(UIElement const& element) {
  Automation::AutomationProperties::SetAccessibilityView(element, Automation::Peers::AccessibilityView::Raw);
}

Duration Millis(int64_t ms) {
  return Duration{std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(std::chrono::milliseconds(ms)),
                  DurationType::TimeSpan};
}

// ---- the locale's numbers --------------------------------------------------------

std::string LocaleInfo(LCTYPE type, std::string fallback) {
  const std::wstring language = Widen(PrimaryLanguage());
  wchar_t value[16]{};
  if (::GetLocaleInfoEx(language.c_str(), type, value, static_cast<int>(std::size(value))) != 0 &&
      value[0] != L'\0') {
    return Narrow(value);
  }
  return fallback;
}

// The decimal separator, the percent pattern (LOCALE_IPOSITIVEPERCENT: 0
// "# %", 1 "#%", 2 "%#", 3 "% #") and the percent sign of the strings'
// language, read once.
struct NumberStyle {
  std::string decimal = ".";
  int percentPattern = 1;
  std::string percentSign = "%";
};

NumberStyle const& LocaleNumbers() {
  static const NumberStyle style = [] {
    NumberStyle read;
    read.decimal = LocaleInfo(LOCALE_SDECIMAL, ".");
    read.percentSign = LocaleInfo(LOCALE_SPERCENT, "%");
    const std::wstring language = Widen(PrimaryLanguage());
    DWORD pattern = 0;
    if (::GetLocaleInfoEx(language.c_str(), LOCALE_IPOSITIVEPERCENT | LOCALE_RETURN_NUMBER,
                          reinterpret_cast<LPWSTR>(&pattern), sizeof(pattern) / sizeof(wchar_t)) != 0) {
      read.percentPattern = static_cast<int>(pattern);
    }
    return read;
  }();
  return style;
}

std::string Decimal(double value, int decimals) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
  std::string text = buffer;
  if (const auto dot = text.find('.'); dot != std::string::npos) text.replace(dot, 1, LocaleNumbers().decimal);
  return text;
}

std::string PercentText(double ratio, int decimals) {
  NumberStyle const& style = LocaleNumbers();
  const std::string number = Decimal(ratio * 100, decimals);
  constexpr std::string_view kNoBreakSpace = "\xC2\xA0";
  switch (style.percentPattern) {
    case 0: return number + std::string(kNoBreakSpace) + style.percentSign;
    case 2: return style.percentSign + number;
    case 3: return style.percentSign + std::string(kNoBreakSpace) + number;
    default: return number + style.percentSign;
  }
}

Brush ToneBrush(pb::Tone tone) {
  switch (tone) {
    case pb::Tone::Positive: return colors::MakeBrush(colors::kUrGreen);
    case pb::Tone::Warning: return colors::MakeBrush(colors::kUrAmber);
    case pb::Tone::Muted: return colors::MutedBrush();
    case pb::Tone::Neutral: break;
  }
  return colors::TextBrush();
}

// the faint accent tint the rows above the line wear
Brush InsideTint() { return colors::MakeBrush(colors::WithAlpha(colors::kAccent, 0x10)); }

}  // namespace

FrameworkElement MakeProtocolChip(std::string const& text, pb::Tone tone) {
  Border chip;
  chip.CornerRadius(CornerRadiusHelper::FromUniformRadius(10));
  chip.Padding(ThicknessHelper::FromLengths(8, 2, 8, 2));
  chip.BorderThickness(ThicknessHelper::FromUniformLength(1));
  chip.VerticalAlignment(VerticalAlignment::Center);
  chip.HorizontalAlignment(HorizontalAlignment::Left);
  const Brush brush = ToneBrush(tone);
  chip.BorderBrush(tone == pb::Tone::Neutral ? colors::BorderBrush() : brush);
  TextBlock words = MakeText(Utf8(text), 12, brush);
  words.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  chip.Child(words);
  return chip;
}

ProtocolBoard::ProtocolBoard(winrt::URnetwork::implementation::MainWindow& window, std::shared_ptr<bool> alive)
    : w_(window), alive_(std::move(alive)) {
  model_.Open();
}

ProtocolBoard::~ProtocolBoard() {
  try {
    if (labelFade_) labelFade_.Stop();
  } catch (...) {
    // the tree may already be gone at teardown
  }
}

bool ProtocolBoard::HasSession() const { return !w_.previewUi() && Sdk().apiReady() && Sdk().IsLoggedIn(); }

// The list is public (GET /sn/top, optional auth): it loads with an api and
// no preview, signed in or not.
bool ProtocolBoard::CanLoadList() const { return !w_.previewUi() && Sdk().apiReady(); }

pb::Text ProtocolBoard::BoardText() const {
  pb::Text text;
  text.localized = [](std::string_view key) { return Narrow(Localized(key)); };
  text.format = [](std::string_view key, std::string const& value) { return Narrow(Format(key, Widen(value))); };
  text.format2 = [](std::string_view key, std::string const& first, std::string const& second) {
    return Narrow(Format(key, Widen(first), Widen(second)));
  };
  text.count = [](int64_t count) { return leaderboard::GroupedRank(count); };
  text.score = [](double score) { return Decimal(score, 1); };
  text.percent = [](double ratio, int decimals) { return PercentText(ratio, decimals); };
  text.ago = [](int64_t unixMillis) { return RelativeTime(unixMillis, NowMillis()); };
  text.day = [](std::string_view day) {
    const auto number = pb::DayNumber(day);
    // noon UTC: the same calendar day in every time zone
    return number ? FormatLocalDate(*number * 86'400'000 + 43'200'000) : std::string(day);
  };
  return text;
}

// ---- setup ------------------------------------------------------------------------

void ProtocolBoard::Initialize() {
  auto weak = w_.get_weak();
  auto alive = alive_;
  w_.ProtocolScroll().ViewChanged([weak, alive](IInspectable const&, ScrollViewerViewChangedEventArgs const&) {
    if (!*alive) return;
    if (auto self = weak.get()) self->wallet().protocol().OnScroll();
  });
  w_.ProtocolRetryButton().Click([weak, alive](IInspectable const&, RoutedEventArgs const&) {
    if (!*alive) return;
    auto self = weak.get();
    if (!self) return;
    auto& board = self->wallet().protocol();
    // a failed next page asks for it again; anything else reloads
    if (board.model_.state().pageFailed) {
      board.LoadNextPage(pb::kPageSize);
    } else {
      board.Reload();
    }
    board.Render();
  });

  // the network pane's block: the strip, the banners, the card, the explainer
  auto host = w_.ProtocolNetworkHost();
  host.Children().Clear();
  auto group = kit::MakePaneGroupHeader(Loc("leaderboard_tab_protocol"));
  groupTitle_ = group.title;
  host.Children().Append(group.root);
  bannersHost_ = StackPanel{};
  bannersHost_.Spacing(8);
  bannersHost_.Padding(ThicknessHelper::FromLengths(12, 0, 12, 0));
  host.Children().Append(bannersHost_);
  cardHost_ = StackPanel{};
  host.Children().Append(cardHost_);
  Border explainerRow;
  explainerRow.BorderBrush(colors::BorderBrush());
  explainerRow.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));
  explainerRow.Padding(ThicknessHelper::FromLengths(12, 8, 12, 10));
  explainer_ = MakeText(Loc("leaderboard_protocol_explainer"), 12, colors::MutedBrush(), true);
  explainerRow.Child(explainer_);
  host.Children().Append(explainerRow);

  BuildIndicator();
  initialized_ = true;
  // the window applies its strings before it initializes its pages
  ApplyStrings();
}

void ProtocolBoard::ApplyStrings() {
  if (!initialized_) return;
  w_.ProtocolRetryButton().Content(winrt::box_value(Loc("retry")));
  groupTitle_.Text(Loc("leaderboard_tab_protocol"));
  explainer_.Text(Loc("leaderboard_protocol_explainer"));
  Automation::AutomationProperties::SetName(slider_, Loc("leaderboard_position_indicator"));
  renderedItems_.clear();  // every row's words again
  renderedBody_ = pb::ListBody::Placeholders;
  Render();
}

// ---- showing and loading -----------------------------------------------------------

void ProtocolBoard::SetShowing(bool showing) {
  showing_ = showing;
  if (showing && !preview_) {
    const pb::State& state = model_.state();
    if (CanLoadList() &&
        ((!state.listLoaded && !state.listLoading) || model_.ShouldRefresh(NowMillis()))) {
      Reload();
    }
    if (HasSession() && !state.meLoaded && !state.meLoading) LoadMe();
  }
  Render();
  UpdateIndicator();
}

void ProtocolBoard::Reload() {
  if (!CanLoadList()) return;
  const uint64_t generation = model_.Current();
  const pb::Model::PageRequest request = model_.BeginReload();
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  auto alive = alive_;
  try {
    urnet::SnTopArgs args;
    args.limit = request.limit;
    // The sdk answers on its own thread. The answer is read there, and only
    // the plain value crosses to the UI thread.
    Sdk().api().snTop(args, [queue, weak, alive, generation, request](std::optional<urnet::SnTopResult> result,
                                                                        std::optional<std::string> err) {
      pb::TopOutcome outcome = pb::TopOutcomeFrom(result, err);
      queue.TryEnqueue([weak, alive, generation, request, outcome = std::move(outcome)]() mutable {
        if (!*alive) return;
        if (auto self = weak.get()) self->wallet().protocol().ApplyPage(generation, request, std::move(outcome));
      });
    });
  } catch (std::exception const& e) {
    LogError("protocol board: could not ask for the board: {}", e.what());
    pb::TopOutcome failed;
    failed.error = e.what();
    ApplyPage(generation, request, std::move(failed));
  }
}

void ProtocolBoard::LoadNextPage(int64_t limit) {
  if (!CanLoadList()) return;
  const uint64_t generation = model_.Current();
  const std::optional<pb::Model::PageRequest> request = model_.BeginNextPage(limit);
  if (!request) return;
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  auto alive = alive_;
  const pb::Model::PageRequest sent = *request;
  try {
    urnet::SnTopArgs args;
    args.limit = sent.limit;
    args.cursor = sent.cursor;  // pinned to the first page's snapshot
    Sdk().api().snTop(args, [queue, weak, alive, generation, sent](std::optional<urnet::SnTopResult> result,
                                                                     std::optional<std::string> err) {
      pb::TopOutcome outcome = pb::TopOutcomeFrom(result, err);
      queue.TryEnqueue([weak, alive, generation, sent, outcome = std::move(outcome)]() mutable {
        if (!*alive) return;
        if (auto self = weak.get()) self->wallet().protocol().ApplyPage(generation, sent, std::move(outcome));
      });
    });
  } catch (std::exception const& e) {
    LogError("protocol board: could not ask for the next page: {}", e.what());
    pb::TopOutcome failed;
    failed.error = e.what();
    ApplyPage(generation, sent, std::move(failed));
  }
  RenderFooter(pb::ListFor(model_.state(), BoardText()));
}

void ProtocolBoard::ApplyPage(uint64_t generation, pb::Model::PageRequest request, pb::TopOutcome outcome) {
  // for a board this one has reset: a sign-out
  if (!model_.Admits(generation)) return;
  // the board shows its own words, never this message
  if (!outcome.ok) LogWarn("protocol board: a page failed: {}", outcome.error);
  switch (model_.ApplyPage(request, std::move(outcome), NowMillis())) {
    case pb::Model::Applied::Dropped:
      return;
    case pb::Model::Applied::Restart:
      // the board moved on to a new snapshot while the list paged, or the
      // page's snapshot expired (410): from the top of the newest one
      LogInfo("protocol board: the list's snapshot is gone; reloading from the top");
      Reload();
      break;
    case pb::Model::Applied::Applied:
      break;
  }
  Render();
  ContinueSeek();
  // a page that does not fill the pane can never be scrolled to its end, so
  // the next one is asked for once layout has run
  auto weak = w_.get_weak();
  auto alive = alive_;
  w_.DispatcherQueue().TryEnqueue(winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [weak, alive] {
    if (!*alive) return;
    auto self = weak.get();
    if (!self) return;
    auto& board = self->wallet().protocol();
    if (board.showing_ && self->ProtocolScroll().ScrollableHeight() <= 0) board.LoadNextPage(pb::kPageSize);
  });
}

void ProtocolBoard::LoadMe() {
  if (!HasSession()) {
    Render();
    return;
  }
  const uint64_t generation = model_.Current();
  const uint64_t load = model_.BeginMe();
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  auto alive = alive_;
  try {
    Sdk().api().snTopMe([queue, weak, alive, generation, load](std::optional<urnet::SnTopMeResult> result,
                                                               std::optional<std::string> err) {
      pb::MeOutcome outcome = pb::MeOutcomeFrom(result, err);
      queue.TryEnqueue([weak, alive, generation, load, outcome = std::move(outcome)]() mutable {
        if (!*alive) return;
        if (auto self = weak.get()) self->wallet().protocol().ApplyMe(generation, load, std::move(outcome));
      });
    });
  } catch (std::exception const& e) {
    LogError("protocol board: could not ask for the card: {}", e.what());
    pb::MeOutcome failed;
    failed.error = e.what();
    ApplyMe(generation, load, std::move(failed));
    return;
  }
  Render();
}

void ProtocolBoard::ApplyMe(uint64_t generation, uint64_t load, pb::MeOutcome outcome) {
  if (!model_.Admits(generation)) return;
  if (!outcome.ok) LogWarn("protocol board: the card failed: {}", outcome.error);
  if (!model_.ApplyMe(load, std::move(outcome))) return;
  Render();
}

void ProtocolBoard::ContinueSeek() {
  if (model_.WantsSeekPage()) {
    LoadNextPage(pb::kSeekPageSize);
    return;
  }
  if (const auto landed = model_.TakeLandedSeek()) ScrollToPosition(*landed);
}

void ProtocolBoard::ApplyNamePublic(bool isPublic, bool writing) {
  const bool changed = namePublic_ != isPublic || nameWriting_ != writing;
  namePublic_ = isPublic;
  nameWriting_ = writing;
  // the card's header follows the switch: the name, or Anonymous
  if (initialized_ && changed) RenderCard(model_.state());
}

void ProtocolBoard::ReloadAfterNameChange() {
  LoadMe();
  if (model_.state().listLoaded) Reload();
}

void ProtocolBoard::ResetForSignOut() {
  model_.Close();
  model_.Open();
  preview_ = false;
  namePublic_ = false;
  nameWriting_ = false;
  refreshNoticeShown_ = false;
  renderedItems_.clear();
  renderedBody_ = pb::ListBody::Placeholders;
  lineAfter_ = -1;
  firstVisible_ = 1;
  total_ = 0;
  renderedYou_.reset();
  w_.ProtocolYouHost().Child(nullptr);
  w_.ProtocolNotice().IsOpen(false);
  Render();
  UpdateIndicator();
}

void ProtocolBoard::ShowPreview(bool sample) {
  preview_ = true;
  model_.Close();
  model_.Open();
  renderedItems_.clear();
  renderedBody_ = pb::ListBody::Placeholders;
  renderedYou_.reset();
  // no request: an empty board, or obviously synthetic rows (never a real
  // network's) for a screenshot
  pb::TopOutcome page;
  page.ok = true;
  page.snapshot = pb::Snapshot{};
  page.snapshot->id = "preview";
  page.snapshot->totalRanked = sample ? 260 : 0;
  page.snapshot->projectedCutoff = 41.2;
  page.snapshot->computedAtMillis = NowMillis();
  if (sample) {
    for (int64_t position = 1; position <= 260; ++position) {
      pb::Row row;
      row.position = position;
      row.name = position % 3 == 0 ? std::string() : "sample-network-" + std::to_string(position);
      row.projectedScore = 400.0 - static_cast<double>(position);
      row.margin = (row.projectedScore - 41.2) / 41.2;
      row.trend = position % 5 == 0 ? 2 : (position % 7 == 0 ? -1 : 0);
      row.measurable = position % 11 != 0;
      row.label = row.measurable ? pb::Label::Estimate : pb::Label::NotMeasurable;
      row.claimPrompt = row.measurable && position <= 200;
      if (position % 9 == 0) {
        pb::Fleet fleet;
        fleet.uid = 100 + position;
        fleet.generation = 1;
        fleet.replicaScore = row.projectedScore;
        row.fleets.push_back(fleet);
      }
      page.rows.push_back(std::move(row));
    }
  }
  model_.ApplyPage(model_.BeginReload(), std::move(page), NowMillis());
  Render();
}

// ---- rendering ---------------------------------------------------------------------

void ProtocolBoard::Render() {
  if (!initialized_) return;
  const pb::State& state = model_.state();
  RenderBanners(state);
  RenderCard(state);
  RenderList();
  RenderYouBar();
  RenderTile();
}

void ProtocolBoard::RenderBanners(pb::State const& state) {
  bannersHost_.Children().Clear();
  const auto banners = pb::BannersFor(pb::BoardSnapshot(state), BoardText());
  bannersHost_.Padding(ThicknessHelper::FromLengths(12, banners.empty() ? 0 : 10, 12, banners.empty() ? 0 : 2));
  for (auto const& banner : banners) {
    // neutral, never the error style: claiming closed is not a fault
    InfoBar bar;
    bar.IsOpen(true);
    bar.IsClosable(false);
    bar.IsIconVisible(true);
    bar.Severity(InfoBarSeverity::Informational);
    bar.Message(Utf8(banner.text));
    bannersHost_.Children().Append(bar);
  }
}

void ProtocolBoard::RenderCard(pb::State const& state) {
  cardHost_.Children().Clear();
  nameToggle_ = nullptr;
  chartLabel_ = nullptr;
  const pb::Text text = BoardText();
  std::string ownName;
  if (auto jwt = Sdk().ParsedJwt(); jwt && !jwt->NetworkName.empty()) {
    ownName = kit::SanitizeExternalDisplayText(jwt->NetworkName);
  }
  const pb::Card card = pb::CardFor(state, HasSession(), namePublic_, ownName, std::string(), text);

  Border row;
  row.BorderBrush(colors::BorderBrush());
  row.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));
  row.Padding(ThicknessHelper::FromLengths(12, 12, 12, 12));
  StackPanel body;
  body.Spacing(10);
  row.Child(body);
  cardHost_.Children().Append(row);

  switch (card.body) {
    case pb::CardBody::SignIn:
      body.Children().Append(MakeText(Loc("please_login_to_urnetwork"), 13, colors::MutedBrush(), true));
      return;
    case pb::CardBody::Loading:
      // the card's own sizes: the header, the position, the chip line, the chart
      body.Children().Append(kit::MakeSkeletonText(L"kayak-hollow-network", 16));
      body.Children().Append(kit::MakeSkeletonText(L"#173", 44));
      body.Children().Append(kit::MakeSkeletonText(L"Estimate 12% above the Top 200 line", 13));
      {
        Border chart;
        chart.Height(kChartHeight);
        chart.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
        chart.Background(colors::CardBrush());
        body.Children().Append(chart);
      }
      return;
    case pb::CardBody::Failed: {
      body.Children().Append(MakeText(Loc("something_went_wrong"), 13, colors::MutedBrush(), true));
      Button retry;
      retry.Style(rows::Lookup(L"UrSecondaryButtonStyle"));
      retry.Content(winrt::box_value(Loc("retry")));
      auto weak = w_.get_weak();
      auto alive = alive_;
      retry.Click([weak, alive](IInspectable const&, RoutedEventArgs const&) {
        if (!*alive) return;
        if (auto self = weak.get()) self->wallet().protocol().LoadMe();
      });
      body.Children().Append(retry);
      return;
    }
    case pb::CardBody::Unranked:
    case pb::CardBody::Ranked:
      break;
  }

  // the header: the tag, the name or Anonymous, the shared name switch
  {
    Grid header;
    header.ColumnSpacing(8);
    header.ColumnDefinitions().Append(Column(GridLengthHelper::Auto()));
    header.ColumnDefinitions().Append(Column(GridLengthHelper::FromValueAndType(1, GridUnitType::Star)));
    TextBlock tag = MakeText(Utf8(card.tag), 18, colors::TextBrush());
    tag.VerticalAlignment(VerticalAlignment::Center);
    kit::SetTextOrCollapse(tag, Utf8(card.tag));
    header.Children().Append(tag);
    TextBlock name = MakeText(Utf8(card.name), 16, card.anonymous ? colors::MutedBrush() : colors::TextBrush());
    name.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    name.TextTrimming(TextTrimming::CharacterEllipsis);
    name.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(name, 1);
    header.Children().Append(name);
    body.Children().Append(header);

    Grid toggleRow;
    toggleRow.ColumnSpacing(10);
    toggleRow.ColumnDefinitions().Append(Column(GridLengthHelper::FromValueAndType(1, GridUnitType::Star)));
    toggleRow.ColumnDefinitions().Append(Column(GridLengthHelper::Auto()));
    TextBlock label = MakeText(Loc("show_on_points_leaderboard"), 13, colors::MutedBrush(), true);
    label.VerticalAlignment(VerticalAlignment::Center);
    toggleRow.Children().Append(label);
    nameToggle_ = ToggleSwitch{};
    nameToggle_.Style(rows::Lookup(L"UrSwitchToggleStyle"));
    nameToggle_.Width(44);
    Automation::AutomationProperties::SetLabeledBy(nameToggle_, label);
    applyingNameToggle_ = true;
    nameToggle_.IsOn(namePublic_);
    applyingNameToggle_ = false;
    nameToggle_.IsEnabled(!nameWriting_ && HasSession());
    auto weak = w_.get_weak();
    auto alive = alive_;
    nameToggle_.Toggled([weak, alive](IInspectable const&, RoutedEventArgs const&) {
      if (!*alive) return;
      if (auto self = weak.get()) self->wallet().protocol().OnNamePublicToggled();
    });
    Grid::SetColumn(nameToggle_, 1);
    toggleRow.Children().Append(nameToggle_);
    body.Children().Append(toggleRow);
  }

  if (card.body == pb::CardBody::Ranked) {
    // the position, projected, of how many
    StackPanel position;
    position.Orientation(Orientation::Horizontal);
    position.Spacing(8);
    TextBlock figure = MakeText(Utf8(card.position), 44, colors::TextBrush());
    figure.FontFamily(FontFamily(L"ms-appx:///Assets/Fonts/abcgravity_extra_condensed.otf#ABC Gravity Extra Condensed"));
    position.Children().Append(figure);
    StackPanel caption;
    caption.VerticalAlignment(VerticalAlignment::Bottom);
    caption.Margin(ThicknessHelper::FromLengths(0, 0, 0, 6));
    caption.Children().Append(MakeText(Utf8(card.projected), 14, colors::MutedBrush()));
    TextBlock of = MakeText(Utf8(card.ofRanked), 12, colors::MutedBrush());
    kit::SetTextOrCollapse(of, Utf8(card.ofRanked));
    caption.Children().Append(of);
    position.Children().Append(caption);
    body.Children().Append(position);

    // the chip, the margin, the trend
    StackPanel standing;
    standing.Spacing(4);
    StackPanel chips;
    chips.Orientation(Orientation::Horizontal);
    chips.Spacing(8);
    if (!card.labelText.empty()) chips.Children().Append(MakeProtocolChip(card.labelText, card.tone));
    for (auto const& uid : card.uids) chips.Children().Append(MakeProtocolChip(uid, pb::Tone::Neutral));
    if (chips.Children().Size() > 0) standing.Children().Append(chips);
    standing.Children().Append(MakeText(Utf8(card.margin), 13, colors::TextBrush(), true));
    if (!card.trend.empty()) {
      standing.Children().Append(MakeText(
          Utf8(card.trend), 13,
          card.trendDirection > 0 ? colors::MakeBrush(colors::kUrGreen) : colors::MakeBrush(colors::kUrAmber), true));
    }
    body.Children().Append(standing);

    if (card.chart.shown) body.Children().Append(MakeChart(card.chart));
    body.Children().Append(MakeText(Utf8(card.why), 13, colors::MutedBrush(), true));
    body.Children().Append(MakeText(Utf8(card.solo), 13, colors::MutedBrush(), true));
  }
  if (!card.notMeasurable.empty()) {
    if (card.body == pb::CardBody::Unranked && !card.labelText.empty()) {
      body.Children().Append(MakeProtocolChip(card.labelText, card.tone));
    }
    body.Children().Append(MakeText(Utf8(card.notMeasurable), 13, colors::MakeBrush(colors::kUrAmber), true));
  }
  if (card.claim.shown) body.Children().Append(MakeClaimPrompt(card.claim));
}

// The daily projected rank over 90 days: a line on an inverted axis (up is
// better) with the Top 200 line across it. A pointer over it shows the
// nearest day's rank. A screen reader reads the summary off the title.
FrameworkElement ProtocolBoard::MakeChart(pb::Chart const& chart) {
  StackPanel panel;
  panel.Spacing(4);
  TextBlock title = MakeText(Utf8(chart.title), 12, colors::MutedBrush());
  Automation::AutomationProperties::SetName(title, Utf8(chart.spoken));
  panel.Children().Append(title);

  Grid area;
  area.Height(kChartHeight);
  area.Background(colors::MakeBrush(colors::WithAlpha(colors::kBackground, 0)));  // hit-testable
  Canvas canvas;
  SetRaw(canvas);
  area.Children().Append(canvas);
  chartLabel_ = MakeText(hstring{}, 11, colors::TextBrush());
  chartLabel_.HorizontalAlignment(HorizontalAlignment::Left);
  chartLabel_.VerticalAlignment(VerticalAlignment::Top);
  chartLabel_.Visibility(Visibility::Collapsed);
  chartLabel_.IsHitTestVisible(false);
  SetRaw(chartLabel_);
  area.Children().Append(chartLabel_);
  panel.Children().Append(area);

  const pb::Chart drawn = chart;
  // the canvas is the area's child: holding it here makes no cycle back to the area
  auto plot = [canvas, drawn](double width) {
    canvas.Children().Clear();
    if (width <= 0) return;
    const double height = kChartHeight - 2 * kChartPad;
    auto yOf = [height](double y) { return kChartPad + y * height; };
    // the line, dashed, in the accent
    Shapes::Line line;
    line.X1(0);
    line.X2(width);
    line.Y1(yOf(drawn.lineY));
    line.Y2(yOf(drawn.lineY));
    line.Stroke(colors::AccentBrush());
    line.StrokeThickness(1);
    line.Opacity(0.7);
    DoubleCollection dashes;
    dashes.Append(4);
    dashes.Append(4);
    line.StrokeDashArray(dashes);
    canvas.Children().Append(line);
    TextBlock lineLabel = MakeText(Utf8(drawn.lineLabel), 10, colors::MutedBrush());
    lineLabel.Measure(winrt::Windows::Foundation::Size{std::numeric_limits<float>::infinity(),
                                                       std::numeric_limits<float>::infinity()});
    Canvas::SetLeft(lineLabel, std::max(0.0, width - lineLabel.DesiredSize().Width));
    Canvas::SetTop(lineLabel, std::max(0.0, yOf(drawn.lineY) - 14));
    canvas.Children().Append(lineLabel);
    // the rank
    Shapes::Polyline series;
    series.Stroke(colors::TextBrush());
    series.StrokeThickness(2);
    series.StrokeLineJoin(PenLineJoin::Round);
    for (auto const& point : drawn.points) {
      series.Points().Append(winrt::Windows::Foundation::Point{static_cast<float>(point.x * width),
                                                               static_cast<float>(yOf(point.y))});
    }
    canvas.Children().Append(series);
    // today
    auto const& last = drawn.points.back();
    Shapes::Ellipse dot;
    dot.Width(7);
    dot.Height(7);
    dot.Fill(colors::AccentBrush());
    Canvas::SetLeft(dot, last.x * width - 3.5);
    Canvas::SetTop(dot, yOf(last.y) - 3.5);
    canvas.Children().Append(dot);
  };
  area.SizeChanged([plot](IInspectable const&, SizeChangedEventArgs const& e) { plot(e.NewSize().Width); });

  auto weak = w_.get_weak();
  auto alive = alive_;
  // the area comes in as the event's sender: a handler that held it would keep
  // every chart a re-render replaces alive
  auto show = [weak, alive, drawn](Grid const& area, double x) {
    if (!*alive) return;
    auto self = weak.get();
    if (!self) return;
    auto label = self->wallet().protocol().chartLabel_;
    const double width = area.ActualWidth();
    if (!label || width <= 0) return;
    const auto index = pb::NearestPoint(drawn, x / width);
    if (!index) return;
    auto const& point = drawn.points[*index];
    label.Text(hstring{L"#" + Widen(leaderboard::GroupedRank(point.rank)) + L"  " + Widen(point.day)});
    label.Visibility(Visibility::Visible);
    label.Measure(winrt::Windows::Foundation::Size{std::numeric_limits<float>::infinity(),
                                                   std::numeric_limits<float>::infinity()});
    const double left = std::clamp(point.x * width - label.DesiredSize().Width / 2, 0.0,
                                   std::max(0.0, width - label.DesiredSize().Width));
    label.Margin(ThicknessHelper::FromLengths(left, 0, 0, 0));
  };
  area.PointerMoved([show](IInspectable const& sender, Input::PointerRoutedEventArgs const& e) {
    if (auto area = sender.try_as<Grid>()) show(area, e.GetCurrentPoint(area).Position().X);
  });
  area.PointerPressed([show](IInspectable const& sender, Input::PointerRoutedEventArgs const& e) {
    if (auto area = sender.try_as<Grid>()) show(area, e.GetCurrentPoint(area).Position().X);
  });
  area.PointerExited([weak, alive](IInspectable const&, Input::PointerRoutedEventArgs const&) {
    if (!*alive) return;
    if (auto self = weak.get()) {
      if (auto label = self->wallet().protocol().chartLabel_) label.Visibility(Visibility::Collapsed);
    }
  });
  return panel;
}

// Q5, Q13, Q16: binding open, the screen's one primary button; closed, a note
// in its place. Every prompt carries the warning.
FrameworkElement ProtocolBoard::MakeClaimPrompt(pb::ClaimPrompt const& prompt) {
  Border box;
  box.Background(colors::BackgroundBrush());
  box.BorderBrush(colors::BorderBrush());
  box.BorderThickness(ThicknessHelper::FromUniformLength(1));
  box.CornerRadius(CornerRadiusHelper::FromUniformRadius(6));
  box.Padding(ThicknessHelper::FromUniformLength(12));
  StackPanel lines;
  lines.Spacing(8);
  TextBlock inside = MakeText(Utf8(prompt.inside), 14, colors::TextBrush(), true);
  inside.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  lines.Children().Append(inside);
  if (prompt.button) {
    Button claim;
    claim.Style(rows::Lookup(L"UrPaneActionPrimaryStyle"));
    claim.Margin(ThicknessHelper::FromUniformLength(0));
    claim.HorizontalAlignment(HorizontalAlignment::Stretch);
    claim.Content(winrt::box_value(Utf8(prompt.action)));
    auto weak = w_.get_weak();
    auto alive = alive_;
    claim.Click([weak, alive](IInspectable const&, RoutedEventArgs const&) {
      if (!*alive) return;
      if (auto self = weak.get()) self->wallet().protocol().ClaimOnEarnings();
    });
    lines.Children().Append(claim);
  } else {
    TextBlock soon = MakeText(Utf8(prompt.action), 14, colors::TextBrush(), true);
    lines.Children().Append(soon);
  }
  if (!prompt.borderline.empty()) {
    lines.Children().Append(MakeText(Utf8(prompt.borderline), 12, colors::MakeBrush(colors::kUrAmber), true));
  }
  lines.Children().Append(MakeText(Utf8(prompt.warning), 12, colors::MutedBrush(), true));
  box.Child(lines);
  return box;
}

// "Claim on Earnings": the Top 200 tile in the wallet pane, scrolled to and
// focused. Below the two-pane width that pane is folded, and the tile's own
// claim runs instead (the fold-doors rule: a door the fold hides is duplicated,
// never lost).
void ProtocolBoard::ClaimOnEarnings() {
  if (w_.WalletPaneA().Visibility() == Visibility::Visible &&
      w_.Top200Panel().Visibility() == Visibility::Visible) {
    w_.Top200Panel().StartBringIntoView();
    if (w_.Top200Button().Visibility() == Visibility::Visible) {
      w_.Top200Button().Focus(FocusState::Programmatic);
    } else if (w_.Top200SignButton().Visibility() == Visibility::Visible) {
      w_.Top200SignButton().Focus(FocusState::Programmatic);
    }
    return;
  }
  w_.wallet().OnClaimTop200(nullptr, nullptr);
}

void ProtocolBoard::OnNamePublicToggled() {
  // the handler cannot tell a flip from the write that renders WalletPage's
  // answer, so the write sets a flag and this returns
  if (applyingNameToggle_ || !nameToggle_) return;
  const bool requested = nameToggle_.IsOn();
  if (requested == namePublic_) return;
  w_.wallet().RequestNamePublic(requested);
}

void ProtocolBoard::AppendPlaceholderRows(Panel const& host, int count) {
  for (int i = 0; i < count; ++i) {
    Border row;
    row.Height(kRowHeight);
    row.BorderBrush(colors::BorderBrush());
    row.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));
    row.Padding(ThicknessHelper::FromLengths(12, 0, 12, 0));
    Grid grid;
    grid.ColumnSpacing(12);
    grid.ColumnDefinitions().Append(Column(GridLengthHelper::FromPixels(40)));
    grid.ColumnDefinitions().Append(Column(GridLengthHelper::FromValueAndType(1, GridUnitType::Star)));
    grid.ColumnDefinitions().Append(Column(GridLengthHelper::FromPixels(56)));
    auto position = kit::MakeSkeletonText(L"000", 14);
    position.VerticalAlignment(VerticalAlignment::Center);
    grid.Children().Append(position);
    auto name = kit::MakeSkeletonText(L"sample-network-name", 14);
    name.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(name, 1);
    grid.Children().Append(name);
    auto score = kit::MakeSkeletonText(L"000.0", 14);
    score.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(score, 2);
    grid.Children().Append(score);
    row.Child(grid);
    host.Children().Append(row);
  }
}

void ProtocolBoard::RenderList() {
  const pb::Text text = BoardText();
  const pb::ListView view = pb::ListFor(model_.state(), text);
  auto rows = w_.ProtocolRows();
  total_ = view.total;

  if (view.body != pb::ListBody::Rows) {
    if (view.body != renderedBody_ || !renderedItems_.empty() || rows.Children().Size() == 0) {
      rows.Children().Clear();
      renderedItems_.clear();
      lineAfter_ = -1;
      switch (view.body) {
        case pb::ListBody::Placeholders:
          AppendPlaceholderRows(rows, kPlaceholderRows);
          break;
        case pb::ListBody::Failed:
          rows.Children().Append(kit::MakePaneEmptyLine(Loc("something_went_wrong")));
          break;
        case pb::ListBody::Empty:
          rows.Children().Append(kit::MakePaneEmptyLine(Loc("leaderboard_protocol_empty")));
          break;
        case pb::ListBody::Rows:
          break;
      }
    }
    renderedBody_ = view.body;
    RenderFooter(view);
    UpdateIndicator();
    return;
  }

  // the common case is the next page landing below the rows already drawn:
  // only the new items are appended; anything else redraws from the top
  const bool extends = renderedBody_ == pb::ListBody::Rows && renderedItems_.size() <= view.items.size() &&
                       std::equal(renderedItems_.begin(), renderedItems_.end(), view.items.begin());
  size_t from = renderedItems_.size();
  if (!extends) {
    rows.Children().Clear();
    lineAfter_ = -1;
    from = 0;
  }
  int64_t rowIndex = 0;
  for (size_t i = 0; i < view.items.size(); ++i) {
    auto const& item = view.items[i];
    if (item.line) {
      lineAfter_ = rowIndex - 1;
      if (from <= i) rows.Children().Append(MakeLine(item));
      continue;
    }
    if (from <= i) rows.Children().Append(MakeRow(item.row));
    ++rowIndex;
  }
  renderedItems_ = view.items;
  renderedBody_ = view.body;

  // a failed refresh keeps the rows and says so, once
  if (view.refreshFailed && !refreshNoticeShown_) {
    refreshNoticeShown_ = true;
    w_.ProtocolNotice().Message(Loc("something_went_wrong"));
    w_.ProtocolNotice().IsOpen(true);
  } else if (!view.refreshFailed) {
    refreshNoticeShown_ = false;
    w_.ProtocolNotice().IsOpen(false);
  }
  RenderFooter(view);
  RefreshPosition();
}

void ProtocolBoard::RenderFooter(pb::ListView const& view) {
  w_.ProtocolFooterRing().IsActive(view.paging);
  w_.ProtocolFooterRing().Visibility(view.paging ? Visibility::Visible : Visibility::Collapsed);
  const bool retry = view.pageFailed || view.body == pb::ListBody::Failed;
  kit::SetTextOrCollapse(w_.ProtocolFooterText(), view.pageFailed ? Loc("something_went_wrong") : hstring{});
  w_.ProtocolRetryButton().Visibility(retry ? Visibility::Visible : Visibility::Collapsed);
}

// One ranked network: the position, the name over the tag (with the caller's
// "You" and chip), the claimed badges, the trend, the score. A claimed row is
// a button that opens its detail sheet; the others are plain rows.
UIElement ProtocolBoard::MakeRow(pb::ListRow const& row) {
  Grid grid;
  grid.ColumnSpacing(10);
  grid.ColumnDefinitions().Append(Column(GridLengthHelper::FromPixels(44)));
  grid.ColumnDefinitions().Append(Column(GridLengthHelper::FromValueAndType(1, GridUnitType::Star)));
  grid.ColumnDefinitions().Append(Column(GridLengthHelper::Auto()));
  grid.ColumnDefinitions().Append(Column(GridLengthHelper::FromPixels(48)));
  grid.ColumnDefinitions().Append(Column(GridLengthHelper::FromPixels(56)));
  const Brush text = row.dimmed ? colors::FaintBrush() : colors::TextBrush();

  TextBlock position = MakeText(Utf8(row.positionText), 14, colors::MutedBrush());
  position.VerticalAlignment(VerticalAlignment::Center);
  grid.Children().Append(position);

  StackPanel identity;
  identity.VerticalAlignment(VerticalAlignment::Center);
  StackPanel top;
  top.Orientation(Orientation::Horizontal);
  top.Spacing(6);
  TextBlock name = MakeText(Utf8(row.name), 14, row.anonymous || row.dimmed ? colors::MutedBrush() : text);
  name.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  name.TextTrimming(TextTrimming::CharacterEllipsis);
  top.Children().Append(name);
  if (row.dimmed) {
    auto mark = MakeGlyph(kUnmeasuredGlyph, 12, colors::FaintBrush());
    mark.VerticalAlignment(VerticalAlignment::Center);
    SetRaw(mark);  // the row's name says it
    top.Children().Append(mark);
  }
  if (row.isMe) {
    TextBlock you = MakeText(Utf8(row.you), 12, colors::MakeBrush(colors::kUrGreen));
    you.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    you.VerticalAlignment(VerticalAlignment::Center);
    top.Children().Append(you);
    if (!row.labelText.empty()) top.Children().Append(MakeProtocolChip(row.labelText, row.tone));
  }
  identity.Children().Append(top);
  TextBlock tag = MakeText(hstring{}, 12, colors::MutedBrush());
  kit::SetTextOrCollapse(tag, Utf8(row.tag));
  identity.Children().Append(tag);
  Grid::SetColumn(identity, 1);
  grid.Children().Append(identity);

  if (!row.uids.empty()) {
    StackPanel badges;
    badges.Orientation(Orientation::Horizontal);
    badges.Spacing(4);
    badges.VerticalAlignment(VerticalAlignment::Center);
    for (auto const& uid : row.uids) {
      Border badge;
      badge.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
      badge.Padding(ThicknessHelper::FromLengths(6, 1, 6, 1));
      badge.Background(colors::CardBrush());
      TextBlock words = MakeText(Utf8(uid), 11, colors::MakeBrush(colors::kToggleAccent));
      words.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
      badge.Child(words);
      badges.Children().Append(badge);
    }
    Grid::SetColumn(badges, 2);
    grid.Children().Append(badges);
  }

  if (row.trendDirection != 0) {
    StackPanel trend;
    trend.Orientation(Orientation::Horizontal);
    trend.Spacing(2);
    trend.HorizontalAlignment(HorizontalAlignment::Right);
    trend.VerticalAlignment(VerticalAlignment::Center);
    const Brush brush = row.trendDirection > 0 ? colors::MakeBrush(colors::kUrGreen)
                                               : colors::MakeBrush(colors::kUrAmber);
    trend.Children().Append(MakeGlyph(row.trendDirection > 0 ? kUpGlyph : kDownGlyph, 10, brush));
    trend.Children().Append(MakeText(Utf8(row.trend), 12, brush));
    SetRaw(trend);  // the row's name reads it out in words
    Grid::SetColumn(trend, 3);
    grid.Children().Append(trend);
  }

  TextBlock score = MakeText(Utf8(row.score), 14, text);
  score.HorizontalAlignment(HorizontalAlignment::Right);
  score.VerticalAlignment(VerticalAlignment::Center);
  Grid::SetColumn(score, 4);
  grid.Children().Append(score);

  // the caller's row: a fill step and the "You" mark, never colour alone;
  // the rows at or above the line wear the faint accent tint
  Brush background = row.isMe ? colors::CardBrush() : (row.insideLine ? InsideTint() : nullptr);
  if (row.claimed) {
    Button button;
    button.Style(rows::Lookup(L"UrPaneRowButtonStyle"));
    button.Height(kRowHeight);
    button.HorizontalAlignment(HorizontalAlignment::Stretch);
    button.HorizontalContentAlignment(HorizontalAlignment::Stretch);
    if (background) button.Background(background);
    button.Content(grid);
    Automation::AutomationProperties::SetName(button, Utf8(row.spoken));
    auto weak = w_.get_weak();
    auto alive = alive_;
    const int64_t at = row.position;
    button.Click([weak, alive, at](IInspectable const&, RoutedEventArgs const&) {
      if (!*alive) return;
      if (auto self = weak.get()) self->wallet().protocol().ShowDetail(at);
    });
    return button;
  }
  Border border;
  border.Height(kRowHeight);
  border.BorderBrush(colors::BorderBrush());
  border.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));
  border.Padding(ThicknessHelper::FromLengths(12, 0, 12, 0));
  if (background) border.Background(background);
  border.Child(grid);
  // a screen reader hears the row as one line, not its cells one by one
  Automation::AutomationProperties::SetName(position, Utf8(row.spoken));
  for (UIElement const& cell : {UIElement(name), UIElement(tag), UIElement(score)}) SetRaw(cell);
  return border;
}

UIElement ProtocolBoard::MakeLine(pb::ListItem const& item) {
  Grid line;
  line.Height(kLineHeight);
  line.Padding(ThicknessHelper::FromLengths(12, 0, 12, 0));
  line.ColumnSpacing(8);
  line.ColumnDefinitions().Append(Column(GridLengthHelper::FromValueAndType(1, GridUnitType::Star)));
  line.ColumnDefinitions().Append(Column(GridLengthHelper::Auto()));
  line.ColumnDefinitions().Append(Column(GridLengthHelper::FromPixels(12)));
  // a dashed rule that fills its own column (a 0-1 line stretched to it)
  auto rule = [](int column) {
    Shapes::Line dash;
    dash.X1(0);
    dash.X2(1);
    dash.Y1(0);
    dash.Y2(0);
    dash.Stretch(Stretch::Fill);
    dash.Stroke(colors::AccentBrush());
    dash.StrokeThickness(2);
    DoubleCollection dashes;
    dashes.Append(3);
    dashes.Append(3);
    dash.StrokeDashArray(dashes);
    dash.HorizontalAlignment(HorizontalAlignment::Stretch);
    dash.VerticalAlignment(VerticalAlignment::Center);
    SetRaw(dash);
    Grid::SetColumn(dash, column);
    return dash;
  };
  line.Children().Append(rule(0));
  TextBlock words = MakeText(Utf8(item.lineText), 12, colors::AccentBrush());
  words.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
  words.VerticalAlignment(VerticalAlignment::Center);
  // a screen reader element of its own: "Top 200 line, score 41.2"
  Automation::AutomationProperties::SetName(words, Utf8(item.lineSpoken));
  Grid::SetColumn(words, 1);
  line.Children().Append(words);
  line.Children().Append(rule(2));
  kit::ClipToBounds(line);
  return line;
}

// The caller's place under the list while its row is out of view: a press
// scrolls to it, or, past 400, brings the card into view.
void ProtocolBoard::RenderYouBar() {
  auto host = w_.ProtocolYouHost();
  const pb::YouBar bar = pb::YouBarFor(model_.state(), HasSession(), BoardText());
  bool inView = false;
  if (bar.shown && bar.inList && !renderedItems_.empty()) {
    const auto& rows = model_.state().rows;
    for (size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].position != bar.position) continue;
      auto const scroll = w_.ProtocolScroll();
      const double top = pb::RowTop(static_cast<int64_t>(i), RowsTop(), kRowHeight, kLineHeight, lineAfter_);
      inView = scroll.VerticalOffset() <= top && top + kRowHeight <= scroll.VerticalOffset() + scroll.ViewportHeight();
      break;
    }
  }
  const bool show = bar.shown && showing_ && !inView && model_.state().listLoaded;
  host.Visibility(show ? Visibility::Visible : Visibility::Collapsed);
  if (!show) return;
  // a scroll only moves the arrow; the bar is rebuilt when what it says changes
  const bool up = bar.position < firstVisible_;
  if (renderedYou_ && *renderedYou_ == bar && renderedYouUp_ == up) return;
  renderedYou_ = bar;
  renderedYouUp_ = up;

  Button button;
  button.Style(rows::Lookup(L"UrPaneRowButtonStyle"));
  button.Height(44);
  button.HorizontalAlignment(HorizontalAlignment::Stretch);
  button.HorizontalContentAlignment(HorizontalAlignment::Stretch);
  button.Background(colors::CardBrush());
  Grid grid;
  grid.ColumnSpacing(10);
  grid.ColumnDefinitions().Append(Column(GridLengthHelper::Auto()));
  grid.ColumnDefinitions().Append(Column(GridLengthHelper::Auto()));
  grid.ColumnDefinitions().Append(Column(GridLengthHelper::FromValueAndType(1, GridUnitType::Star)));
  grid.ColumnDefinitions().Append(Column(GridLengthHelper::Auto()));
  TextBlock you = MakeText(Utf8(bar.you), 13, colors::MakeBrush(colors::kUrGreen));
  you.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
  you.VerticalAlignment(VerticalAlignment::Center);
  grid.Children().Append(you);
  TextBlock position = MakeText(Utf8(bar.positionText), 14, colors::TextBrush());
  position.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
  position.VerticalAlignment(VerticalAlignment::Center);
  Grid::SetColumn(position, 1);
  grid.Children().Append(position);
  TextBlock margin = MakeText(Utf8(bar.margin), 13, colors::MutedBrush());
  margin.TextTrimming(TextTrimming::CharacterEllipsis);
  margin.VerticalAlignment(VerticalAlignment::Center);
  Grid::SetColumn(margin, 2);
  grid.Children().Append(margin);
  auto arrow = MakeGlyph(up ? kUpGlyph : kDownGlyph, 12, colors::MutedBrush());
  arrow.VerticalAlignment(VerticalAlignment::Center);
  SetRaw(arrow);
  Grid::SetColumn(arrow, 3);
  grid.Children().Append(arrow);
  button.Content(grid);
  Automation::AutomationProperties::SetName(
      button, Utf8(bar.you + ", " + bar.positionText + ", " + bar.margin));
  auto weak = w_.get_weak();
  auto alive = alive_;
  const int64_t at = bar.position;
  const bool inList = bar.inList;
  button.Click([weak, alive, at, inList](IInspectable const&, RoutedEventArgs const&) {
    if (!*alive) return;
    auto self = weak.get();
    if (!self) return;
    auto& board = self->wallet().protocol();
    if (inList) {
      board.Seek(at);
    } else {
      // past 400 only the card says where it ranks
      self->ProtocolNetworkHost().StartBringIntoView();
    }
  });
  host.Child(button);
}

void ProtocolBoard::RenderTile() {
  const pb::State& state = model_.state();
  const std::optional<pb::Snapshot>& snapshot = state.meSnapshot ? state.meSnapshot : state.snapshot;
  w_.wallet().ApplyTop200(HasSession() ? pb::TileFor(state.me, snapshot, BoardText()) : pb::Tile{});
}

// ---- the claimed row's detail sheet ----------------------------------------------------

winrt::fire_and_forget ProtocolBoard::ShowDetail(int64_t position) {
  const std::optional<pb::Row> row = pb::RowAt(model_.state(), position);
  if (!row || !row->Claimed()) co_return;
  if (w_.sheetOpen()) {
    LogInfo("protocol board: another sheet is open; the detail was not opened");
    co_return;
  }
  const pb::Detail detail = pb::DetailFor(*row, BoardText());
  auto window = w_.get_strong();
  auto const root = window->Content().XamlRoot();
  ContentDialog dialog = rows::MakeSheet(root, Utf8(detail.title));
  dialog.CloseButtonText(Loc("close"));
  dialog.DefaultButton(ContentDialogButton::Close);
  StackPanel content;
  content.Spacing(12);
  content.MinWidth(sheetfit::Width(root, 380));
  content.Children().Append(MakeText(Utf8(detail.subtitle), 13, colors::MutedBrush(), true));
  auto facts = [](Panel const& host, std::vector<pb::Fact> const& list) {
    for (auto const& fact : list) {
      auto row = kit::MakePaneKeyValueRow(Utf8(fact.label), Utf8(fact.value), 36);
      host.Children().Append(row.root);
    }
  };
  facts(content, detail.facts);
  for (auto const& fleet : detail.fleets) {
    Grid head;
    head.ColumnSpacing(8);
    head.ColumnDefinitions().Append(Column(GridLengthHelper::FromValueAndType(1, GridUnitType::Star)));
    head.ColumnDefinitions().Append(Column(GridLengthHelper::Auto()));
    TextBlock title = MakeText(Utf8(fleet.title), 16, colors::TextBrush());
    title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    head.Children().Append(title);
    if (!fleet.selected.empty()) {
      auto chip = MakeProtocolChip(fleet.selected, fleet.isSelected ? pb::Tone::Positive : pb::Tone::Muted);
      Grid::SetColumn(chip, 1);
      head.Children().Append(chip);
    }
    content.Children().Append(head);
    StackPanel list;
    facts(list, fleet.facts);
    content.Children().Append(list);
  }
  if (!detail.note.empty()) content.Children().Append(MakeText(Utf8(detail.note), 12, colors::MutedBrush(), true));
  ScrollViewer scroll;
  scroll.MaxHeight(sheetfit::Height(root, 520));
  scroll.Content(content);
  dialog.Content(scroll);

  window->SetSheetOpen(true);
  try {
    co_await dialog.ShowAsync();
  } catch (winrt::hresult_error const& e) {
    LogError("protocol board: the detail sheet failed to open: {}", Narrow(std::wstring{e.message()}));
  } catch (...) {
    LogError("protocol board: the detail sheet failed to open");
  }
  window->SetSheetOpen(false);
}

// ---- the position indicator ------------------------------------------------------------
// A slider over positions 1..N (N at most 400) at the scroller's right edge
// while the list is longer than the pane (mmm/DESIGNSTYLE.md "Long ranked
// lists"): the thumb at the first row in view, as long as the loaded rows over
// N; dragging shows the position and tier beside it, and a release seeks there,
// loading the pages up to it first. A hidden Slider under the thumb is its
// keyboard and UI Automation face. The math is LeaderboardIndicator.h's.

void ProtocolBoard::BuildIndicator() {
  auto weak = w_.get_weak();
  auto alive = alive_;
  indicator_ = Canvas{};
  indicator_.Width(kIndicatorWidth);
  indicator_.HorizontalAlignment(HorizontalAlignment::Right);
  indicator_.VerticalAlignment(VerticalAlignment::Stretch);
  indicator_.Visibility(Visibility::Collapsed);
  w_.ProtocolHost().Children().Append(indicator_);
  indicator_.SizeChanged([weak, alive](IInspectable const&, SizeChangedEventArgs const&) {
    if (!*alive) return;
    if (auto self = weak.get()) self->wallet().protocol().UpdateIndicator();
  });

  track_ = Shapes::Rectangle{};
  track_.Width(kIndicatorTrackWidth);
  track_.RadiusX(kIndicatorTrackWidth / 2);
  track_.RadiusY(kIndicatorTrackWidth / 2);
  track_.Fill(colors::FaintBrush());
  Canvas::SetLeft(track_, (kIndicatorWidth - kIndicatorTrackWidth) / 2);
  Canvas::SetTop(track_, kIndicatorPad);
  indicator_.Children().Append(track_);

  slider_ = Slider{};
  slider_.Orientation(Orientation::Vertical);
  slider_.IsDirectionReversed(true);  // position 1 at the top
  slider_.Minimum(1);
  slider_.Maximum(1);
  slider_.StepFrequency(1);
  slider_.Opacity(0);
  slider_.IsHitTestVisible(false);
  slider_.Width(kIndicatorWidth);
  Automation::AutomationProperties::SetName(slider_, Loc("leaderboard_position_indicator"));
  Canvas::SetLeft(slider_, 0);
  Canvas::SetTop(slider_, kIndicatorPad);
  slider_.ValueChanged([weak, alive](IInspectable const&,
                                     winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e) {
    if (!*alive) return;
    auto self = weak.get();
    if (!self) return;
    auto& board = self->wallet().protocol();
    if (board.applyingSlider_) return;  // the board set it, not the keyboard
    const int64_t position = static_cast<int64_t>(std::llround(e.NewValue()));
    board.ShowDragLabel(position);
    board.HideDragLabel(/*fade=*/true);
    board.Seek(position);
  });
  indicator_.Children().Append(slider_);

  thumb_ = Shapes::Rectangle{};
  thumb_.Width(leaderboard::kThumbWidth);
  thumb_.Height(leaderboard::kThumbMinHeight);
  thumb_.RadiusX(6);
  thumb_.RadiusY(6);
  thumb_.Fill(colors::AccentBrush());
  thumb_.Opacity(0.85);
  SetRaw(thumb_);  // the slider speaks for it
  Canvas::SetLeft(thumb_, (kIndicatorWidth - leaderboard::kThumbWidth) / 2);
  Canvas::SetTop(thumb_, kIndicatorPad);
  thumb_.PointerPressed([weak, alive](IInspectable const&, Input::PointerRoutedEventArgs const& e) {
    if (!*alive) return;
    auto self = weak.get();
    if (!self) return;
    auto& board = self->wallet().protocol();
    board.OnThumbPressed(e.GetCurrentPoint(board.indicator_).Position().Y, e);
  });
  thumb_.PointerMoved([weak, alive](IInspectable const&, Input::PointerRoutedEventArgs const& e) {
    if (!*alive) return;
    auto self = weak.get();
    if (!self) return;
    auto& board = self->wallet().protocol();
    if (!board.dragging_) return;
    board.OnThumbMoved(e.GetCurrentPoint(board.indicator_).Position().Y);
    e.Handled(true);
  });
  auto released = [weak, alive](IInspectable const&, Input::PointerRoutedEventArgs const& e) {
    if (!*alive) return;
    auto self = weak.get();
    if (!self) return;
    auto& board = self->wallet().protocol();
    if (!board.dragging_) return;
    board.thumb_.ReleasePointerCapture(e.Pointer());
    board.OnThumbReleased();
    e.Handled(true);
  };
  thumb_.PointerReleased(released);
  thumb_.PointerCanceled(released);
  thumb_.PointerCaptureLost(released);
  track_.PointerPressed([weak, alive](IInspectable const&, Input::PointerRoutedEventArgs const& e) {
    if (!*alive) return;
    auto self = weak.get();
    if (!self) return;
    auto& board = self->wallet().protocol();
    board.OnThumbPressed(e.GetCurrentPoint(board.indicator_).Position().Y, e);
  });
  indicator_.Children().Append(thumb_);

  dragLabel_ = Border{};
  dragLabel_.Background(colors::CardBrush());
  dragLabel_.BorderBrush(colors::BorderBrush());
  dragLabel_.BorderThickness(ThicknessHelper::FromUniformLength(1));
  dragLabel_.CornerRadius(CornerRadiusHelper::FromUniformRadius(8));
  dragLabel_.Padding(ThicknessHelper::FromLengths(10, 6, 10, 6));
  dragLabel_.HorizontalAlignment(HorizontalAlignment::Right);
  dragLabel_.VerticalAlignment(VerticalAlignment::Top);
  dragLabel_.IsHitTestVisible(false);
  dragLabel_.Visibility(Visibility::Collapsed);
  SetRaw(dragLabel_);
  StackPanel lines;
  dragRank_ = MakeText(hstring{}, 14, colors::TextBrush());
  dragRank_.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  dragRank_.HorizontalAlignment(HorizontalAlignment::Right);
  dragTier_ = MakeText(hstring{}, 11, colors::MutedBrush());
  dragTier_.HorizontalAlignment(HorizontalAlignment::Right);
  lines.Children().Append(dragRank_);
  lines.Children().Append(dragTier_);
  dragLabel_.Child(lines);
  w_.ProtocolHost().Children().Append(dragLabel_);
}

// Where the rows start in the scrolled content: under the card when it is
// folded into the list's scroll, and under the refresh notice.
double ProtocolBoard::RowsTop() const {
  try {
    const auto point = w_.ProtocolRows().TransformToVisual(w_.ProtocolScrollContent()).TransformPoint(
        winrt::Windows::Foundation::Point{0, 0});
    return point.Y;
  } catch (...) {
    return 0;  // not laid out yet
  }
}

void ProtocolBoard::RefreshPosition() {
  const auto& rows = model_.state().rows;
  if (rows.empty() || renderedBody_ != pb::ListBody::Rows) {
    firstVisible_ = 1;
  } else {
    const int64_t index = pb::FirstVisibleIndex(w_.ProtocolScroll().VerticalOffset(), RowsTop(), kRowHeight,
                                                kLineHeight, lineAfter_, static_cast<int64_t>(rows.size()));
    firstVisible_ = rows[static_cast<size_t>(index)].position;
  }
  UpdateIndicator();
}

void ProtocolBoard::UpdateIndicator() {
  if (!indicator_) return;
  auto const scroll = w_.ProtocolScroll();
  const auto& rows = model_.state().rows;
  const bool listShowing = showing_ && w_.LeaderboardHost().Visibility() == Visibility::Visible &&
                           renderedBody_ == pb::ListBody::Rows && !rows.empty();
  const double trackHeight = indicator_.ActualHeight() - 2 * kIndicatorPad;
  const auto thumb = listShowing ? leaderboard::ThumbFor(firstVisible_, static_cast<int64_t>(rows.size()), total_,
                                                         trackHeight, scroll.ExtentHeight(), scroll.ViewportHeight())
                                 : leaderboard::Thumb{};
  // the canvas stays in the tree (collapsed) so its height is known when the list grows
  const bool show = listShowing && (thumb.visible || indicator_.ActualHeight() <= 0);
  indicator_.Visibility(show ? Visibility::Visible : Visibility::Collapsed);
  scroll.VerticalScrollBarVisibility(thumb.visible ? ScrollBarVisibility::Hidden : ScrollBarVisibility::Auto);
  w_.ProtocolRows().Margin(ThicknessHelper::FromLengths(0, 0, thumb.visible ? kIndicatorWidth : 0, 0));
  if (!thumb.visible) {
    track_.Visibility(Visibility::Collapsed);
    thumb_.Visibility(Visibility::Collapsed);
    slider_.Visibility(Visibility::Collapsed);
    if (dragging_) {
      dragging_ = false;
      HideDragLabel(/*fade=*/false);
    }
    return;
  }
  track_.Visibility(Visibility::Visible);
  thumb_.Visibility(Visibility::Visible);
  slider_.Visibility(Visibility::Visible);
  track_.Height(trackHeight);
  slider_.Height(trackHeight);
  thumb_.Height(thumb.height);
  thumb_.Opacity(dragging_ ? 1.0 : 0.85);
  Canvas::SetTop(thumb_, kIndicatorPad + (dragging_ ? dragTop_ : thumb.top));
  // the slider mirrors the position in view: an arrow steps a page, a page key a tenth
  const int64_t position = dragging_ ? dragPosition_ : firstVisible_;
  applyingSlider_ = true;
  slider_.Maximum(static_cast<double>(std::max<int64_t>(1, total_)));
  slider_.SmallChange(static_cast<double>(pb::kPageSize));
  slider_.LargeChange(static_cast<double>(std::max<int64_t>(pb::kPageSize, total_ / 10)));
  slider_.Value(static_cast<double>(position));
  applyingSlider_ = false;
}

void ProtocolBoard::OnThumbPressed(double y, Input::PointerRoutedEventArgs const& e) {
  const double trackHeight = indicator_.ActualHeight() - 2 * kIndicatorPad;
  const double thumbHeight = thumb_.Height();
  if (trackHeight <= 0 || total_ <= 0) return;
  const double thumbTop = Canvas::GetTop(thumb_) - kIndicatorPad;
  const double pressed = y - kIndicatorPad;
  // a press on the thumb picks it up where it is; on the track, under the pointer
  const bool onThumb = pressed >= thumbTop && pressed <= thumbTop + thumbHeight;
  dragTop_ = onThumb ? thumbTop : std::clamp(pressed - thumbHeight / 2, 0.0, std::max(0.0, trackHeight - thumbHeight));
  dragGrab_ = pressed - dragTop_;
  dragging_ = true;
  thumb_.CapturePointer(e.Pointer());
  slider_.Focus(FocusState::Programmatic);
  e.Handled(true);
  OnThumbMoved(y);
}

void ProtocolBoard::OnThumbMoved(double y) {
  if (!dragging_) return;
  const double trackHeight = indicator_.ActualHeight() - 2 * kIndicatorPad;
  const double thumbHeight = thumb_.Height();
  dragTop_ = std::clamp(y - kIndicatorPad - dragGrab_, 0.0, std::max(0.0, trackHeight - thumbHeight));
  dragPosition_ = leaderboard::RankForThumbTop(dragTop_, thumbHeight, trackHeight, total_);
  ShowDragLabel(dragPosition_);
  UpdateIndicator();
}

void ProtocolBoard::OnThumbReleased() {
  if (!dragging_) return;
  dragging_ = false;
  HideDragLabel(/*fade=*/true);
  Seek(dragPosition_);
  UpdateIndicator();
}

void ProtocolBoard::Seek(int64_t position) {
  if (model_.state().rows.empty()) return;
  if (model_.Seek(position)) {
    ContinueSeek();
    return;
  }
  ScrollToPosition(std::clamp<int64_t>(position, 1, model_.state().rows.back().position));
}

void ProtocolBoard::ScrollToPosition(int64_t position) {
  const auto& rows = model_.state().rows;
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].position < position) continue;
    w_.ProtocolRows().UpdateLayout();
    const double top = pb::RowTop(static_cast<int64_t>(i), RowsTop(), kRowHeight, kLineHeight, lineAfter_);
    w_.ProtocolScroll().ChangeView(nullptr, winrt::Windows::Foundation::IReference<double>{top}, nullptr, true);
    return;
  }
}

void ProtocolBoard::ShowDragLabel(int64_t position) {
  if (!dragLabel_) return;
  if (labelFade_) {
    labelFade_.Stop();
    labelFade_ = nullptr;
  }
  std::wstring rankText = L"#" + Widen(leaderboard::GroupedRank(position));
  hstring tier;
  const int64_t ranked = pb::BoardSnapshot(model_.state()) ? pb::BoardSnapshot(model_.state())->totalRanked : 0;
  if (auto parts = urnet::pointsLeaderboardScrollLabel(position, std::max(ranked, total_))) {
    if (!parts->rank_text.empty()) rankText = Widen(parts->rank_text);
    const std::string tierKey = leaderboard::TierLabelKey(parts->tier);
    if (tierKey == "leaderboard_tier_top") {
      tier = hstring{Format("leaderboard_tier_top", parts->tier_percent)};
    } else if (tierKey == "leaderboard_tier_rest") {
      tier = Loc("leaderboard_tier_rest");
    }
  }
  dragRank_.Text(hstring{rankText});
  kit::SetTextOrCollapse(dragTier_, tier);
  dragLabel_.Opacity(1.0);
  dragLabel_.Visibility(Visibility::Visible);
  dragLabel_.Measure(winrt::Windows::Foundation::Size{std::numeric_limits<float>::infinity(),
                                                      std::numeric_limits<float>::infinity()});
  const double labelHeight = dragLabel_.DesiredSize().Height;
  const double middle = Canvas::GetTop(thumb_) + thumb_.Height() / 2;
  dragLabel_.Margin(ThicknessHelper::FromLengths(0, std::max(0.0, middle - labelHeight / 2), kIndicatorWidth + 8, 0));
}

void ProtocolBoard::HideDragLabel(bool fade) {
  if (!dragLabel_) return;
  if (labelFade_) {
    labelFade_.Stop();
    labelFade_ = nullptr;
  }
  if (!fade) {
    dragLabel_.Visibility(Visibility::Collapsed);
    return;
  }
  Animation::DoubleAnimation animation;
  animation.From(1.0);
  animation.To(0.0);
  animation.BeginTime(
      std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(std::chrono::milliseconds(600)));
  animation.Duration(Millis(250));
  Animation::Storyboard::SetTarget(animation, dragLabel_);
  Animation::Storyboard::SetTargetProperty(animation, L"Opacity");
  Animation::Storyboard storyboard;
  storyboard.Children().Append(animation);
  auto weak = w_.get_weak();
  auto alive = alive_;
  storyboard.Completed([weak, alive](IInspectable const&, IInspectable const&) {
    if (!*alive) return;
    auto self = weak.get();
    if (!self) return;
    auto& board = self->wallet().protocol();
    if (!board.labelFade_) return;  // a later show overtook this fade
    board.dragLabel_.Visibility(Visibility::Collapsed);
    board.dragLabel_.Opacity(1.0);
    board.labelFade_ = nullptr;
  });
  labelFade_ = storyboard;
  storyboard.Begin();
}

void ProtocolBoard::ScrollToTop() {
  w_.ProtocolScroll().ChangeView(nullptr, winrt::Windows::Foundation::IReference<double>{0.0}, nullptr, true);
  RefreshPosition();
}

// Asks for the next page when the last row in view nears the end, and keeps
// the "you" bar and the thumb on the rows in view.
void ProtocolBoard::OnScroll() {
  RefreshPosition();
  RenderYouBar();
  const auto& state = model_.state();
  if (!showing_ || state.rows.empty() || state.next.empty() || state.pageLoading || state.pageFailed) return;
  auto const scroll = w_.ProtocolScroll();
  if (scroll.ScrollableHeight() - scroll.VerticalOffset() < 3 * kRowHeight * 4) LoadNextPage(pb::kPageSize);
}

}  // namespace urnw
