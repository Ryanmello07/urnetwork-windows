// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "SlowInternetPill.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <utility>

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Documents.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include "PageContext.h"
#include "TunnelIndicator.h"
#include "UrColors.h"
#include "UrComponents.h"  // kit::kIconFontFamily

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace urnw::pages;

namespace urnw {

namespace {

namespace anim = winrt::Microsoft::UI::Xaml::Media::Animation;
namespace documents = winrt::Microsoft::UI::Xaml::Documents;
namespace media = winrt::Microsoft::UI::Xaml::Media;
namespace shapes = winrt::Microsoft::UI::Xaml::Shapes;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::Foundation::TimeSpan;
using AccessibilityView = winrt::Microsoft::UI::Xaml::Automation::Peers::AccessibilityView;

// The pill (apple/DELAY.md §5.2): 30 tall, its parts 8 apart, 6 from its
// leading edge and 10 from its trailing one, the 1 px outline included.
constexpr double kPillHeight = 30;
constexpr double kOutline = 1;
constexpr double kPaddingLeading = 5;
constexpr double kPaddingTrailing = 9;
constexpr double kSpacing = 8;
// between an arrow and its number
constexpr double kArrowSpacing = 2;
// the space before the pill, after the header's title
constexpr double kPillMargin = 10;
// black, as the compact island draws it, in its 35% amber outline
constexpr winrt::Windows::UI::Color kPillFill{255, 0, 0, 0};
constexpr uint8_t kOutlineAlpha = 0x59;
// an arrow's ink: white at 55%, or amber at 75% on the worse direction
constexpr uint8_t kArrowAlpha = 0x8C;
constexpr uint8_t kWorseArrowAlpha = 0xBF;
// the copy's crossfade to and from "Internet back to normal", as long as
// Apple's content change
constexpr int64_t kFaceMillis = 350;

Duration Millis(int64_t ms) {
  return Duration{std::chrono::duration_cast<TimeSpan>(std::chrono::milliseconds(ms)),
                  DurationType::TimeSpan};
}

// §3's fades are ease-out, in and out.
anim::DoubleAnimation EaseOut(double from, double to, int64_t ms, int64_t beginMs) {
  anim::DoubleAnimation animation;
  animation.From(from);
  animation.To(to);
  animation.Duration(Millis(ms));
  if (0 < beginMs) {
    animation.BeginTime(std::chrono::duration_cast<TimeSpan>(std::chrono::milliseconds(beginMs)));
  }
  anim::CubicEase ease;
  ease.EasingMode(anim::EasingMode::EaseOut);
  animation.EasingFunction(ease);
  return animation;
}

void Add(anim::Storyboard const& storyboard, anim::Timeline const& animation,
         DependencyObject const& target, wchar_t const* property) {
  anim::Storyboard::SetTarget(animation, target);
  anim::Storyboard::SetTargetProperty(animation, property);
  storyboard.Children().Append(animation);
}

// The brand body face, as the pane headers draw it (App.xaml).
media::FontFamily BodyFont() {
  if (auto app = Application::Current()) {
    auto key = winrt::box_value(winrt::hstring{L"UrBodyFontFamily"});
    if (app.Resources().HasKey(key)) {
      if (auto family = app.Resources().Lookup(key).try_as<media::FontFamily>()) return family;
    }
  }
  return media::FontFamily(L"Segoe UI Variable Text");
}

TextBlock MakeText(double size, winrt::Windows::UI::Color color) {
  TextBlock text;
  text.FontFamily(BodyFont());
  text.FontSize(size);
  text.Foreground(colors::MakeBrush(color));
  return text;
}

}  // namespace

// ---- the tunnel -------------------------------------------------------------

TunnelShapes::TunnelShapes(double size) {
  canvas_.Width(size);
  canvas_.Height(size);
  canvas_.IsHitTestVisible(false);
  const auto ink = colors::MakeBrush(colors::kUrAmber);
  const double vanishing = tunnel::kVanishingPoint * size;
  for (int index = 0; index < tunnel::kMaxDepth; ++index) {
    const tunnel::Square geometry = tunnel::SquareAt(size, index);
    // the outline's centre line, stroked on both sides of it: the outer square's
    // stroke ends exactly at the box
    media::RectangleGeometry outline;
    outline.Rect({static_cast<float>(geometry.x), static_cast<float>(geometry.y),
                  static_cast<float>(geometry.side), static_cast<float>(geometry.side)});
    shapes::Path path;
    path.Data(outline);
    path.Stroke(ink);
    path.StrokeThickness(geometry.stroke);
    // square corners: rounded ones soften the tunnel (§2.1)
    path.StrokeLineJoin(media::PenLineJoin::Miter);
    path.IsHitTestVisible(false);
    path.Opacity(0);
    // a square settles about the vanishing point, so it seems to recede
    media::ScaleTransform scale;
    scale.CenterX(vanishing);
    scale.CenterY(vanishing);
    path.RenderTransform(scale);
    canvas_.Children().Append(path);
    squares_.push_back(Square{path, scale, geometry.opacity});
  }
}

TunnelShapes::~TunnelShapes() {
  // a stopped storyboard raises no Completed into a destroyed tunnel
  if (storyboard_) storyboard_.Stop();
}

void TunnelShapes::Settle(int depth) {
  for (int index = 0; index < static_cast<int>(squares_.size()); ++index) {
    Square& square = squares_[index];
    square.path.Opacity(index < depth ? square.opacity : 0.0);
    square.scale.ScaleX(1.0);
    square.scale.ScaleY(1.0);
  }
}

void TunnelShapes::SetDepth(int depth, bool animate, bool scale) {
  depth = std::clamp(depth, 0, tunnel::kMaxDepth);
  const int from = depth_;
  // The change in flight lands on its target: a storyboard that stops shows
  // the base values, and those are set to where it was going.
  if (storyboard_) {
    Settle(from);
    storyboard_.Stop();
    storyboard_ = nullptr;
  }
  depth_ = depth;
  const uint64_t change = ++change_;
  if (from == depth || !animate) {
    Settle(depth);
    return;
  }
  // The base values stay at `from` until the change ends, so a square shows
  // its old state through its stagger delay; each fade holds its end value.
  anim::Storyboard storyboard;
  for (const tunnel::Fade& fade : tunnel::FadesFor(from, depth)) {
    Square& square = squares_[fade.index];
    Add(storyboard,
        EaseOut(fade.in ? 0.0 : square.opacity, fade.in ? square.opacity : 0.0,
                fade.durationMillis, fade.beginMillis),
        square.path, L"Opacity");
    if (scale) {
      const double fromScale = fade.in ? tunnel::kSettleScale : 1.0;
      const double toScale = fade.in ? 1.0 : tunnel::kSettleScale;
      Add(storyboard, EaseOut(fromScale, toScale, fade.durationMillis, fade.beginMillis),
          square.scale, L"ScaleX");
      Add(storyboard, EaseOut(fromScale, toScale, fade.durationMillis, fade.beginMillis),
          square.scale, L"ScaleY");
    }
  }
  storyboard.Completed([this, change](IInspectable const&, IInspectable const&) {
    if (change != change_ || !storyboard_) return;
    Settle(depth_);
    storyboard_.Stop();
    storyboard_ = nullptr;
  });
  storyboard_ = storyboard;
  storyboard.Begin();
}

// ---- the pill -----------------------------------------------------------------

SlowInternetPill::DirectionText SlowInternetPill::MakeDirection(wchar_t const* arrow) {
  DirectionText direction;
  direction.root = StackPanel();
  direction.root.Orientation(Orientation::Horizontal);
  direction.root.Spacing(kArrowSpacing);
  direction.root.VerticalAlignment(VerticalAlignment::Center);
  // Centred, the label and the numbers sit within half a pixel of one
  // baseline, and the arrow a pixel above it, where Apple's offset lifts it.
  direction.arrow = MakeText(10, colors::WithAlpha(colors::kText, kArrowAlpha));
  direction.arrow.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
  direction.arrow.VerticalAlignment(VerticalAlignment::Center);
  direction.arrow.Text(arrow);
  direction.number = MakeText(13, colors::kText);
  direction.number.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  direction.number.VerticalAlignment(VerticalAlignment::Center);
  // tabular digits: a number keeps its width as it changes
  documents::Typography::SetNumeralAlignment(direction.number, FontNumeralAlignment::Tabular);
  // the pill is read as one element (ApplyText)
  Automation::AutomationProperties::SetAccessibilityView(direction.arrow, AccessibilityView::Raw);
  Automation::AutomationProperties::SetAccessibilityView(direction.number, AccessibilityView::Raw);
  direction.root.Children().Append(direction.arrow);
  direction.root.Children().Append(direction.number);
  return direction;
}

SlowInternetPill::SlowInternetPill(std::function<void()> refresh) : refresh_(std::move(refresh)) {
  // The tunnel, and over it the check of a return to normal: square, like the
  // tunnel. The pill's words already say both, so neither is read (§2.3).
  Grid mark;
  mark.Width(kTunnelSize);
  mark.Height(kTunnelSize);
  mark.VerticalAlignment(VerticalAlignment::Center);
  mark.Children().Append(tunnel_.Root());
  FontIcon tick;
  tick.FontFamily(media::FontFamily(kit::kIconFontFamily));
  tick.Glyph(L"\uE73E");  // CheckMark, as the location rows use
  tick.FontSize(10);
  tick.Foreground(colors::MakeBrush(colors::kInverseText));
  check_.Width(kCheckSize);
  check_.Height(kCheckSize);
  check_.Background(colors::MakeBrush(colors::kUrGreen));
  check_.HorizontalAlignment(HorizontalAlignment::Center);
  check_.VerticalAlignment(VerticalAlignment::Center);
  check_.Child(tick);
  mark.Children().Append(check_);
  Automation::AutomationProperties::SetAccessibilityView(tunnel_.Root(), AccessibilityView::Raw);
  Automation::AutomationProperties::SetAccessibilityView(check_, AccessibilityView::Raw);
  Automation::AutomationProperties::SetAccessibilityView(tick, AccessibilityView::Raw);

  // the label in amber, then the numbers
  label_ = MakeText(14, colors::kUrAmber);
  label_.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  label_.VerticalAlignment(VerticalAlignment::Center);
  up_ = MakeDirection(L"↑");
  down_ = MakeDirection(L"↓");
  line_.Orientation(Orientation::Horizontal);
  line_.Spacing(kSpacing);
  line_.VerticalAlignment(VerticalAlignment::Center);
  line_.Children().Append(label_);
  line_.Children().Append(up_.root);
  line_.Children().Append(down_.root);
  // "Internet back to normal" takes the line's place
  recoveredText_ = MakeText(13, colors::kText);
  recoveredText_.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  recoveredText_.VerticalAlignment(VerticalAlignment::Center);
  Grid copy;
  copy.VerticalAlignment(VerticalAlignment::Center);
  copy.Children().Append(line_);
  copy.Children().Append(recoveredText_);

  StackPanel content;
  content.Orientation(Orientation::Horizontal);
  content.Spacing(kSpacing);
  content.VerticalAlignment(VerticalAlignment::Center);
  content.Children().Append(mark);
  content.Children().Append(copy);

  root_.Child(content);
  // grows past 30 with Windows' text size, as Apple's with Dynamic Type
  root_.MinHeight(kPillHeight);
  root_.Padding(ThicknessHelper::FromLengths(kPaddingLeading, 3, kPaddingTrailing, 3));
  root_.Background(colors::MakeBrush(kPillFill));
  // the outline inside the box, as Apple's strokeBorder draws it; square
  // corners, like the tunnel's
  root_.BorderBrush(colors::MakeBrush(colors::WithAlpha(colors::kUrAmber, kOutlineAlpha)));
  root_.BorderThickness(ThicknessHelper::FromUniformLength(kOutline));
  root_.CornerRadius(CornerRadiusHelper::FromUniformRadius(0));
  root_.VerticalAlignment(VerticalAlignment::Center);
  root_.HorizontalAlignment(HorizontalAlignment::Right);
  // the space after the header's title; collapsed, the pill takes none
  root_.Margin(ThicknessHelper::FromLengths(kPillMargin, 0, 0, 0));
  root_.Visibility(Visibility::Collapsed);
  root_.Opacity(0);
  // the face showing is the pill's one element (SetFace)
  Automation::AutomationProperties::SetAutomationId(label_, L"acceptance.connect.slow-internet");
  Automation::AutomationProperties::SetAutomationId(recoveredText_, L"acceptance.connect.slow-internet");
  SetFace(false, false);
  ApplyText();
}

SlowInternetPill::~SlowInternetPill() {
  // stopped, the storyboards and the timer call nothing into a destroyed pill
  if (fadeStoryboard_) fadeStoryboard_.Stop();
  if (faceStoryboard_) faceStoryboard_.Stop();
  if (refreshTimer_) refreshTimer_.Stop();
}

bool SlowInternetPill::AnimationsEnabled() const {
  // "Show animations in Windows" off: the fades stay and the scale goes (the
  // shared contract's reduce-motion rule)
  try {
    return winrt::Windows::UI::ViewManagement::UISettings().AnimationsEnabled();
  } catch (...) {
    return true;
  }
}

void SlowInternetPill::SetTitle(TextBlock const& title) {
  title_ = title;
  header_ = title.Parent().try_as<FrameworkElement>();
  headerResized_.revoke();
  if (header_) {
    headerResized_ = header_.SizeChanged(
        winrt::auto_revoke, [this](IInspectable const&, SizeChangedEventArgs const&) { Fit(); });
  }
  Fit();
}

void SlowInternetPill::Apply(std::optional<slow::Reading> const& reading) {
  reading_ = reading;
  Render();
}

void SlowInternetPill::SetSuppressed(bool suppressed) {
  if (suppressed_ == suppressed) return;
  suppressed_ = suppressed;
  Render();
}

void SlowInternetPill::SetPresentationActive(bool active) {
  if (presentationActive_ == active) return;
  presentationActive_ = active;
  // back on screen, the reading AppController replays draws it
  if (active) return;
  // Off screen the pill is put away at once, as Apple's stores reset with
  // the window: the replayed reading then shows from nothing, the slow content
  // or no pill, and never a recovery nobody saw.
  StopFade();
  shown_.reset();
  tunnel_.SetDepth(0, /*animate=*/false, /*scale=*/false);
  SetFace(false, false);
  root_.Opacity(0);
  root_.Visibility(Visibility::Collapsed);
  UpdateRefresh();
}

void SlowInternetPill::ApplyStrings() { ApplyText(); }

void SlowInternetPill::Render() {
  // off screen nothing moves (SetPresentationActive)
  if (!presentationActive_) return;
  const std::optional<slow::PillContent> previous = shown_;
  const std::optional<slow::PillContent> next =
      slow::NextPillContent(previous, reading_, suppressed_);
  shown_ = next;
  UpdateRefresh();
  if (!next) {
    // once: a pill already leaving keeps going
    if (previous) Leave();
    return;
  }
  if (next->recovered) {
    if (!previous || !previous->recovered) Recover(*next);
    return;
  }
  // the slow content, which also ends a leave or the recovered hold
  const bool appearing = root_.Visibility() == Visibility::Collapsed;
  tunnel_.SetDepth(slow::DepthOf(*reading_), /*animate=*/true, AnimationsEnabled());
  content_ = *next;
  ApplyText();
  SetFace(false, /*animate=*/!appearing);
  if (appearing) {
    FadeIn();
  } else if (leaving_) {
    StopFade();
    root_.Opacity(1.0);
  }
}

void SlowInternetPill::Recover(slow::PillContent const& next) {
  tunnel_.SetDepth(0, /*animate=*/true, AnimationsEnabled());
  content_ = next;
  ApplyText();
  SetFace(true, /*animate=*/true);
  FadeOut(slow::kRecoveredMillis);
}

void SlowInternetPill::Leave() {
  const int from = tunnel_.depth();
  tunnel_.SetDepth(0, /*animate=*/true, AnimationsEnabled());
  // from the recovered hold, whose squares are gone, at once
  FadeOut(tunnel::ContainerHideDelayFor(from, 0));
}

void SlowInternetPill::FadeIn() {
  StopFade();
  root_.Opacity(0);
  root_.Visibility(Visibility::Visible);
  anim::Storyboard storyboard;
  Add(storyboard, EaseOut(0, 1, tunnel::kFadeInMillis, 0), root_, L"Opacity");
  const uint64_t change = ++fadeChange_;
  storyboard.Completed([this, change](IInspectable const&, IInspectable const&) {
    if (change != fadeChange_ || !fadeStoryboard_) return;
    root_.Opacity(1.0);
    fadeStoryboard_.Stop();
    fadeStoryboard_ = nullptr;
  });
  fadeStoryboard_ = storyboard;
  storyboard.Begin();
}

void SlowInternetPill::FadeOut(int64_t beginMillis) {
  StopFade();
  leaving_ = true;
  // the base value holds it up until the fade begins
  root_.Opacity(1.0);
  anim::Storyboard storyboard;
  Add(storyboard, EaseOut(1, 0, tunnel::kFadeOutMillis, beginMillis), root_, L"Opacity");
  const uint64_t change = ++fadeChange_;
  storyboard.Completed([this, change](IInspectable const&, IInspectable const&) {
    if (change != fadeChange_ || !fadeStoryboard_) return;
    root_.Opacity(0.0);
    fadeStoryboard_.Stop();
    fadeStoryboard_ = nullptr;
    leaving_ = false;
    root_.Visibility(Visibility::Collapsed);
    // gone: the recovered hold no longer holds it up
    shown_.reset();
    UpdateRefresh();
  });
  fadeStoryboard_ = storyboard;
  storyboard.Begin();
}

void SlowInternetPill::StopFade() {
  leaving_ = false;
  if (!fadeStoryboard_) return;
  fadeStoryboard_.Stop();
  fadeStoryboard_ = nullptr;
}

void SlowInternetPill::SetFace(bool recovered, bool animate) {
  if (faceStoryboard_) {
    // a crossfade in flight lands where it was going first
    SettleFace();
    faceStoryboard_.Stop();
    faceStoryboard_ = nullptr;
  }
  const bool changed = recoveredFace_ != recovered;
  recoveredFace_ = recovered;
  // the face showing is the pill's one element, named in full (ApplyText)
  Automation::AutomationProperties::SetAccessibilityView(
      label_, recovered ? AccessibilityView::Raw : AccessibilityView::Content);
  Automation::AutomationProperties::SetAccessibilityView(
      recoveredText_, recovered ? AccessibilityView::Content : AccessibilityView::Raw);
  if (!changed || !animate) {
    SettleFace();
    return;
  }
  // both show while they cross; the face going out collapses at the end
  line_.Visibility(Visibility::Visible);
  recoveredText_.Visibility(Visibility::Visible);
  const double in = recovered ? 1.0 : 0.0;
  anim::Storyboard storyboard;
  Add(storyboard, EaseOut(1 - in, in, kFaceMillis, 0), check_, L"Opacity");
  Add(storyboard, EaseOut(1 - in, in, kFaceMillis, 0), recoveredText_, L"Opacity");
  Add(storyboard, EaseOut(in, 1 - in, kFaceMillis, 0), line_, L"Opacity");
  const uint64_t change = ++faceChange_;
  storyboard.Completed([this, change](IInspectable const&, IInspectable const&) {
    if (change != faceChange_ || !faceStoryboard_) return;
    SettleFace();
    faceStoryboard_.Stop();
    faceStoryboard_ = nullptr;
  });
  faceStoryboard_ = storyboard;
  storyboard.Begin();
}

void SlowInternetPill::SettleFace() {
  check_.Opacity(recoveredFace_ ? 1.0 : 0.0);
  recoveredText_.Opacity(recoveredFace_ ? 1.0 : 0.0);
  recoveredText_.Visibility(recoveredFace_ ? Visibility::Visible : Visibility::Collapsed);
  line_.Opacity(recoveredFace_ ? 0.0 : 1.0);
  line_.Visibility(recoveredFace_ ? Visibility::Collapsed : Visibility::Visible);
}

void SlowInternetPill::ApplyText() {
  // the last content's words stay while the pill fades out
  const winrt::hstring label = Loc(content_.label == slow::PillContent::Label::Fallback
                                       ? "slow_internet_fallback_short"
                                       : "slow_internet_poor_signal");
  label_.Text(label);
  // each direction's number, the worse one amber with its arrow at 75%
  const auto paint = [](DirectionText const& direction, std::string const& number, bool worse) {
    direction.number.Text(winrt::hstring{slow::Wide(number)});
    direction.number.Foreground(colors::MakeBrush(worse ? colors::kUrAmber : colors::kText));
    direction.arrow.Foreground(colors::MakeBrush(
        worse ? colors::WithAlpha(colors::kUrAmber, kWorseArrowAlpha)
              : colors::WithAlpha(colors::kText, kArrowAlpha)));
  };
  paint(up_, content_.up, content_.worse == slow::Direction::Up);
  paint(down_, content_.down, content_.worse == slow::Direction::Down);
  const winrt::hstring recovered = Loc("slow_internet_recovered");
  recoveredText_.Text(recovered);
  // One element, read whole whatever fits on screen: "Poor signal, Upload
  // delay 800 ms, Download delay 1.9 s", or "Internet back to normal" (§2.3).
  const winrt::hstring upWords = Loc("slow_internet_upload_delay");
  const winrt::hstring downWords = Loc("slow_internet_download_delay");
  Automation::AutomationProperties::SetName(
      label_, winrt::hstring{slow::PillName(label, content_, upWords, downWords)});
  Automation::AutomationProperties::SetName(
      recoveredText_, winrt::hstring{slow::PillName(recovered, content_, upWords, downWords)});
  Fit();
}

void SlowInternetPill::Fit() {
  slow::PillContent fitted = content_;
  const double headerWidth = header_ ? header_.ActualWidth() : 0;
  if (title_ && 0 < headerWidth) {
    // the room the title's own words leave in its header
    const double room = headerWidth - Natural(title_, title_.Text());
    const double fixed = kPillMargin + 2 * kOutline + kPaddingLeading + kPaddingTrailing +
                         kTunnelSize + kSpacing + Natural(label_, label_.Text());
    const auto widthOf = [this](DirectionText const& direction) {
      return kSpacing + Natural(direction.arrow, direction.arrow.Text()) + kArrowSpacing +
             Natural(direction.number, direction.number.Text());
    };
    const double up = widthOf(up_);
    const double down = widthOf(down_);
    fitted = slow::FitPill(content_, room, [&](slow::PillContent const& form) {
      return fixed + (form.up.empty() ? 0.0 : up) + (form.down.empty() ? 0.0 : down);
    });
  }
  up_.root.Visibility(fitted.up.empty() ? Visibility::Collapsed : Visibility::Visible);
  down_.root.Visibility(fitted.down.empty() ? Visibility::Collapsed : Visibility::Visible);
}

double SlowInternetPill::Natural(TextBlock const& like, winrt::hstring const& text) {
  ruler_.FontFamily(like.FontFamily());
  ruler_.FontSize(like.FontSize());
  ruler_.FontWeight(like.FontWeight());
  ruler_.CharacterSpacing(like.CharacterSpacing());
  documents::Typography::SetNumeralAlignment(ruler_,
                                             documents::Typography::GetNumeralAlignment(like));
  ruler_.Text(text);
  constexpr float kUnbounded = std::numeric_limits<float>::infinity();
  ruler_.Measure(winrt::Windows::Foundation::Size{kUnbounded, kUnbounded});
  return ruler_.DesiredSize().Width;
}

void SlowInternetPill::UpdateRefresh() {
  const bool refreshing = slow::PillRefreshes(shown_, presentationActive_);
  if (refreshing && !refreshTimer_) {
    refreshTimer_ =
        winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();
    refreshTimer_.Interval(std::chrono::milliseconds(slow::kPillRefreshMillis));
    refreshTimer_.IsRepeating(true);
    // the timer is the pill's and is stopped with it, so `this` outlives it
    refreshTimer_.Tick([this](auto const&, auto const&) {
      if (refresh_) refresh_();
    });
  }
  if (!refreshTimer_) return;
  if (refreshing && !refreshTimer_.IsRunning()) refreshTimer_.Start();
  if (!refreshing && refreshTimer_.IsRunning()) refreshTimer_.Stop();
}

}  // namespace urnw
