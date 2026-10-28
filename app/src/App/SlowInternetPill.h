// The connect page's slow-internet pill (apple/DELAY.md §1, §5.2, variant A+;
// SlowInternet.h), at the top right of the rightmost pane header on screen: a
// black box with square corners, like the tunnel's, in a 35% amber outline.
// The 18 px tunnel, "Poor signal" in amber ("Fallback" when the worst reading
// came from the fallback transport), then "↑ 800 ms" and "↓ 1.9 s" in tabular
// digits, the worse direction amber. With too little room beside the pane's
// title it drops the better direction, then the numbers. It is a readout, not
// a button: nothing opens from it, and it reads as one element.
//
// It follows the sdk's level directly; the tray is the surface that dwells. It
// hides while the can't-reach banner (RegionPromptBar) shows, the more severe
// state, and while the reading is unavailable; neither is a recovery. On a
// return to normal the squares fade out as a green check and "Internet back to
// normal" fade in; that holds 3 s, then the pill fades.
//
// The tunnel is six XAML Paths kept in the tree, so a change of depth fades
// squares in place (TunnelIndicator.h, §3). Windows' "Show animations" off
// keeps the fades and their stagger and drops the 1.06 scale. A window that
// stops presenting puts the pill away at once, and the reading replayed when
// it presents again shows from nothing: no recovery nobody saw.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include "SlowInternet.h"

namespace urnw {

// The tunnel's six squares in a `size` box, drawn from TunnelIndicator.h.
class TunnelShapes {
 public:
  explicit TunnelShapes(double size);
  ~TunnelShapes();
  TunnelShapes(const TunnelShapes&) = delete;
  TunnelShapes& operator=(const TunnelShapes&) = delete;

  winrt::Microsoft::UI::Xaml::Controls::Canvas Root() const { return canvas_; }
  // the depth the tunnel shows, or is fading to
  int depth() const { return depth_; }
  // To `depth`: faded per §3 when `animate`, settling from kSettleScale only
  // when `scale` too; otherwise at once. A change still in flight lands on its
  // own target first.
  void SetDepth(int depth, bool animate, bool scale);

 private:
  struct Square {
    winrt::Microsoft::UI::Xaml::Shapes::Path path{nullptr};
    winrt::Microsoft::UI::Xaml::Media::ScaleTransform scale{nullptr};
    // its opacity while shown, 1 - 0.08 i
    double opacity = 0;
  };
  // every square's resting values at `depth`
  void Settle(int depth);

  winrt::Microsoft::UI::Xaml::Controls::Canvas canvas_;
  std::vector<Square> squares_;
  winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard storyboard_{nullptr};
  // which change a completion belongs to
  std::uint64_t change_ = 0;
  int depth_ = 0;
};

class SlowInternetPill {
 public:
  // `refresh` asks for fresh numbers (SdkHost::RefreshTransportDelay): the
  // listener carries them only with a change of level, so the pill calls it
  // every slow::kPillRefreshMillis while it shows them.
  explicit SlowInternetPill(std::function<void()> refresh);
  ~SlowInternetPill();
  SlowInternetPill(const SlowInternetPill&) = delete;
  SlowInternetPill& operator=(const SlowInternetPill&) = delete;

  // Collapsed while there is nothing to say. Its host holds nothing else.
  winrt::Microsoft::UI::Xaml::FrameworkElement Root() const { return root_; }
  // The title of the pane header the pill is in. The title keeps its words:
  // the pill fits itself to the room the title leaves it in the header, and
  // fits again as the header resizes (slow::FitPill).
  void SetTitle(winrt::Microsoft::UI::Xaml::Controls::TextBlock const& title);
  // The device's transport delay, or nullopt while unavailable
  // (SdkHost::SetTransportDelayHandler).
  void Apply(std::optional<slow::Reading> const& reading);
  // The can't-reach banner is open.
  void SetSuppressed(bool suppressed);
  void SetPresentationActive(bool active);
  void ApplyStrings();

 private:
  // One direction: its arrow, then its number.
  struct DirectionText {
    winrt::Microsoft::UI::Xaml::Controls::StackPanel root{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock arrow{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock number{nullptr};
  };
  static DirectionText MakeDirection(wchar_t const* arrow);
  // What the pill shows next (slow::NextPillContent), drawn as Apple's host
  // draws it: the slow content, the recovered state, or the pill leaving.
  void Render();
  // The squares fade out from the inside as the check and "Internet back to
  // normal" fade in; that holds slow::kRecoveredMillis, then the pill fades.
  void Recover(slow::PillContent const& next);
  // The squares fade out from the inside first; the pill follows once the last
  // of them has started (TunnelIndicator.h ContainerHideDelayFor), keeping its
  // copy.
  void Leave();
  // The pill's own fade: in with the first square; out after `beginMillis`,
  // then gone.
  void FadeIn();
  void FadeOut(int64_t beginMillis);
  void StopFade();
  // The slow copy, or the check and "Internet back to normal", crossfaded when
  // `animate`; the face showing is the one element read.
  void SetFace(bool recovered, bool animate);
  // The face's resting values: the one showing at full, the other collapsed.
  void SettleFace();
  // The words, the numbers and their colours, and the one name, from content_.
  void ApplyText();
  // Shows the richest form that fits the room the title leaves.
  void Fit();
  // The width `text` takes in the face of `like`, measured on ruler_.
  double Natural(winrt::Microsoft::UI::Xaml::Controls::TextBlock const& like,
                 winrt::hstring const& text);
  // The numbers' re-read, while the slow content shows on a window that
  // presents (slow::PillRefreshes).
  void UpdateRefresh();
  bool AnimationsEnabled() const;

  std::function<void()> refresh_;
  std::optional<slow::Reading> reading_;
  bool suppressed_ = false;
  bool presentationActive_ = false;
  // what the pill shows; nullopt while it is not up or is leaving
  std::optional<slow::PillContent> shown_;
  // what the pill draws: it keeps the last content while it fades out
  slow::PillContent content_;

  // apple/DELAY.md §2.2: the tunnel 18 in a pill 30 tall, the check 16
  static constexpr double kTunnelSize = 18;
  static constexpr double kCheckSize = 16;

  winrt::Microsoft::UI::Xaml::Controls::Border root_;
  TunnelShapes tunnel_{kTunnelSize};
  winrt::Microsoft::UI::Xaml::Controls::Border check_;
  winrt::Microsoft::UI::Xaml::Controls::StackPanel line_;
  winrt::Microsoft::UI::Xaml::Controls::TextBlock label_{nullptr};
  DirectionText up_;
  DirectionText down_;
  winrt::Microsoft::UI::Xaml::Controls::TextBlock recoveredText_{nullptr};
  bool recoveredFace_ = false;
  winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard faceStoryboard_{nullptr};
  std::uint64_t faceChange_ = 0;
  winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard fadeStoryboard_{nullptr};
  std::uint64_t fadeChange_ = 0;
  // a fade out is waiting or running: the pill is leaving
  bool leaving_ = false;
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer refreshTimer_{nullptr};

  // The pane title the pill shares its header with, and the header.
  winrt::Microsoft::UI::Xaml::Controls::TextBlock title_{nullptr};
  winrt::Microsoft::UI::Xaml::FrameworkElement header_{nullptr};
  winrt::Microsoft::UI::Xaml::FrameworkElement::SizeChanged_revoker headerResized_;
  // Never on screen: each text the fit weighs is measured on it, in its own
  // face, so a title its star column trims and a pill that is collapsed or
  // crossfading measure as they would stand alone.
  winrt::Microsoft::UI::Xaml::Controls::TextBlock ruler_;
};

}  // namespace urnw
