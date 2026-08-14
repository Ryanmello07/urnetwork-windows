// Phase E: the main window animates open rather than appearing. Not a splash
// - the window itself expands/reveals its content. See the design plan
// (docs/superpowers/plans/2026-08-11-design-update-super-plan.md, Phase E)
// for the choreography this implements.
//
// UI THREAD ONLY, and it must add ZERO latency to first interaction: Arm()
// writes the start pose synchronously so the first composed frame is already
// correct, and Start() only ever plays a Composition animation, never blocks.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <vector>

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>

#include "UrMotion.h"

namespace urnw {

// One ripple-ring element: signed rise start in dip (+ = starts below its
// final slot and settles UP; − = starts above and settles DOWN; 0 =
// opacity-only), the delay BOTH of its clocks start at, and its two
// durations. See MainWindow's constructor for the two per-state tables.
struct RevealRing {
  winrt::Microsoft::UI::Xaml::FrameworkElement element{nullptr};
  float riseDip = 0.0f;
  int64_t delayMs = 0;
  int64_t fadeMs = urnw::motion::kBaseMs;
  int64_t riseMs = urnw::motion::kSlowMs;
};

// One first frame: the hero that blooms and the rings that unfold around it.
struct RevealState {
  winrt::Microsoft::UI::Xaml::FrameworkElement hero{nullptr};
  std::vector<RevealRing> rings;
};

// E1: the HWND never animates; content springs inside it. `plate` is the
// opaque, never-moving background (WindowPlate in MainWindow.xaml);
// `revealRoot` is the surface that actually springs (RevealRoot). See
// WindowReveal.cpp's file comment for why the HWND itself is off limits.
class WindowReveal {
 public:
  // Bind once, right after the window's content exists. Two tables, one
  // machine: Arm() picks signedIn/signedOut from the tree's own
  // HomeNav/LoginRoot visibility. `revealRoot` remains bound ONLY for the
  // legacy defensive restore in CancelToFinal -- the root itself no longer
  // animates after Task 3.
  void Bind(winrt::Microsoft::UI::Xaml::FrameworkElement const& plate,
            winrt::Microsoft::UI::Xaml::FrameworkElement const& revealRoot,
            winrt::Microsoft::UI::Xaml::FrameworkElement const& homeNav,
            winrt::Microsoft::UI::Xaml::FrameworkElement const& loginRoot,
            RevealState signedIn, RevealState signedOut);

  // Write the START pose synchronously, BEFORE Window.Activate() — the reveal
  // must add zero latency, so the first composed frame is already correct.
  // Arm derives SignedIn/SignedOut ITSELF from the current HomeNav/LoginRoot
  // visibility: it runs synchronously pre-Activate, after every root-
  // visibility write, so the tree already reflects auth state and there is no
  // state parameter to race. `enabled` folds together every caller-side
  // reason not to reveal; "this is an un-minimize, which the OS's own restore
  // animation owns" stays the CALLER's decision (wasIconic latched before
  // SW_RESTORE). ShouldAnimate() is checked here: unarmed = zero property
  // writes = instant, fully-correct UI.
  void Arm(bool enabled);

  // Start the spring + opacity ripple. Call AFTER Activate() returns. A no-op
  // unless the matching Arm() armed a reveal. If the OS animation toggle
  // flips true->false in the window between Arm() and this call, Start()
  // settles rather than leaving Arm's start pose stranded on screen — see
  // the definition; this is deferred finding #1 from the Task 1/2 reviews.
  void Start();

  // Cancel whatever is in flight and snap straight to the settled pose (both
  // heroes at Scale 1/Opacity 1, every ring in BOTH tables at Opacity
  // 1/Translation 0 — the union, regardless of which table armed, since a
  // reveal in flight and the swap that cancels it can disagree about which
  // table is active). Several callers: Arm() itself, settling any reveal
  // still running from a previous show before writing the next start pose;
  // MainWindow::SetPresentationActive(false) - hiding mid-reveal must not
  // leave a hero pinned at ~0.92 scale forever, which is E6's "nastiest
  // failure mode, found not hypothetical"; and the login<->home root swap
  // (ApplyAuthState/ShowLoginRoot/ShowHomeRoot), which settles a reveal
  // before a page swap can inherit half-animated rings. This restores to the
  // exact same settled values Start()'s own per-ring Collapsed skip-guard
  // uses (WindowReveal.cpp's SettleRing) — see that function for where the
  // "no pose Arm writes is ever left orphaned" invariant (deferred finding
  // #2) actually lives.
  void CancelToFinal();

 private:
  winrt::Microsoft::UI::Xaml::FrameworkElement plate_{nullptr};
  winrt::Microsoft::UI::Xaml::FrameworkElement revealRoot_{nullptr};
  winrt::Microsoft::UI::Xaml::FrameworkElement homeNav_{nullptr};
  winrt::Microsoft::UI::Xaml::FrameworkElement loginRoot_{nullptr};
  RevealState signedIn_;
  RevealState signedOut_;
  bool signedInArmed_ = false;  // which table Arm() latched
  bool armed_ = false;
  // Retained so CancelToFinal can STOP them: a running Storyboard holds its
  // DP, and the union restore must release it before writing.
  std::vector<winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard> boards_;
};

}  // namespace urnw
