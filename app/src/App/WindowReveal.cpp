// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "WindowReveal.h"

#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>

#include <cassert>

#include "Log.h"
#include "UrMotion.h"

// E1: THE HWND NEVER ANIMATES; CONTENT SPRINGS INSIDE IT.
//
// Two reasons, both decisive:
//  (a) Composition gives a genuine off-thread spring
//      (Compositor.CreateSpringVector3Animation) but it can only animate a
//      VISUAL's Scale/Offset/Opacity, never an HWND rect — animating the
//      window itself would mean hand-ticking a spring on the UI thread during
//      the app's busiest 200ms.
//  (b) every animated HWND frame would raise AppWindow.Changed with a rect
//      matching no saved placement, which AppController::OnWindowPlacementChanged
//      would read as the user moving the window and persist an intermediate
//      rect to the registry.
//
// So MainWindow.xaml wraps its root Grid in an opaque WindowPlate that never
// moves, containing a RevealRoot (kept only for the legacy defensive restore
// below) and the two hero elements — ConnectCanvasHost, LoginCarouselHost —
// that actually spring, under Hero Bloom (motion-overhaul spec §2.1). Nothing
// here ever touches the HWND, AppWindow, or window placement.
//
// THE PLATE FADE IS A COMPOSITION VISUAL OPACITY ANIMATION, not a Win32
// SetLayeredWindowAttributes/WS_EX_LAYERED fade of the whole window surface.
// The design plan flags that stronger version (E6) as an unverified
// capability — WS_EX_LAYERED over a WinUI 3 DirectComposition island is not
// universally safe, and proving it needs a real screen capture of an ACTIVE
// window, the same verification gap that let the Mica regression ship
// (WindowShell.cpp) — with an explicit sanctioned fallback of "no plate
// fade". This file starts ON that fallback rather than the unverified path,
// since nobody has produced that proof yet.
//
// HERO BLOOM, NOT ORIGIN-ANCHORED SCALE: the reveal used to spring RevealRoot
// from a tray-icon-derived origin with a guessed left/right direction
// (kOffsetDip, dirX — E2). That whole mechanism is gone, along with the bug a
// stray (0.5,0.5) AnchorPoint shipped once: AnchorPoint is the point ON the
// visual placed at Offset, not the point Scale pivots around (that is
// CenterPoint), so (0.5,0.5) DISPLACES the content by half its own size
// rather than centering it. The ban this file now runs under: AnchorPoint is
// never set here except to restore it to {0,0} in CancelToFinal, and the
// scale origin is always a CenterPoint ExpressionAnimation bound to the
// visual's own Size, left running. Both the ban and the mechanism now live in
// one place, UrMotion::ArmHeroBloom/StartHeroBloom — this file only calls
// them.
namespace urnw {
namespace {
using winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview;
using winrt::Microsoft::UI::Xaml::UIElement;
using winrt::Microsoft::UI::Xaml::Visibility;
namespace anim = winrt::Microsoft::UI::Xaml::Media::Animation;

// The one settled pose a ring ever has: Opacity 1, Translation 0 (both inert
// on a Collapsed element — Visibility is never touched here, its owner
// controls that). Shared by CancelToFinal's union restore AND by Start()'s
// per-ring Collapsed skip-guard below, so the settled values have exactly one
// implementation and the two paths cannot drift apart.
//
// THIS is where deferred finding #2 (Task 1/2 ledger) is closed. Arm() can
// write a ring's start pose (Opacity 0, maybe a Translation.Y offset) while
// it is Visible; between Arm() (pre-Activate) and Start() (post-Activate —
// Window.Activate() pumps messages, and a message can land a SizeChanged
// that flips ApplyBreakpoint's pane bucket) its owner can Collapse it before
// Start ever gets to animate it back. A bare `continue` in that skip-guard
// used to leave that Opacity(0) orphaned: not the reveal's problem (one-shot,
// already skipping it) and not its owner's (a Collapsed element has no
// reason to touch Opacity). Routing the skip-guard through SettleRing means
// every pose Arm ever writes is EITHER animated back to settled by the
// running code below OR restored to that same settled value right here —
// never left stranded in between.
void SettleRing(RevealRing const& ring) {
  if (!ring.element) return;
  if (ring.riseDip != 0.0f) {
    auto visual = ElementCompositionPreview::GetElementVisual(ring.element);
    visual.StopAnimation(L"Translation.Y");
    ring.element.Translation(winrt::Windows::Foundation::Numerics::float3{0.0f, 0.0f, 0.0f});
  }
  ring.element.Opacity(1.0);
}

}  // namespace

void WindowReveal::Bind(winrt::Microsoft::UI::Xaml::FrameworkElement const& plate,
                        winrt::Microsoft::UI::Xaml::FrameworkElement const& revealRoot,
                        winrt::Microsoft::UI::Xaml::FrameworkElement const& homeNav,
                        winrt::Microsoft::UI::Xaml::FrameworkElement const& loginRoot,
                        RevealState signedIn, RevealState signedOut) {
  plate_ = plate;
  revealRoot_ = revealRoot;
  homeNav_ = homeNav;
  loginRoot_ = loginRoot;
  signedIn_ = std::move(signedIn);
  signedOut_ = std::move(signedOut);
  // SetIsTranslationEnabled ONCE per translated element, here — forgetting it
  // makes every Translation write a silent no-op that reads as "the stagger
  // feels flat", never as an error (spec §3.7 risk 3).
  for (auto const* state : {&signedIn_, &signedOut_}) {
    for (auto const& ring : state->rings) {
      if (ring.element && ring.riseDip != 0.0f) urnw::motion::EnableTranslation(ring.element);
    }
  }
#ifdef _DEBUG
  // The ancestor-alpha rule, enforced where the tables land (spec §3.7 risk
  // 1): HomeNav is the signed-in hero's alpha ancestor — stage beat only,
  // opacity-only, never delayed, never translated. LoginRoot (the signed-out
  // hero's ancestor) is never listed at all.
  for (auto const& ring : signedIn_.rings) {
    if (ring.element == homeNav_) {
      assert(ring.riseDip == 0.0f && ring.delayMs == 0 &&
             "HomeNav is the STAGE: opacity-only, delay 0");
    }
  }
  for (auto const* state : {&signedIn_, &signedOut_}) {
    for (auto const& ring : state->rings) {
      assert(ring.element != loginRoot_ && "LoginRoot must never be animated");
    }
  }
#endif
}

void WindowReveal::Arm(bool enabled) {
  if (!plate_ || !homeNav_ || !loginRoot_) return;
  // A reveal still running from a PREVIOUS show settles before this one
  // writes its own start pose — two overlapping springs on the same visual is
  // how E6's "nastiest failure mode" starts.
  CancelToFinal();
  armed_ = enabled && urnw::motion::ShouldAnimate();
  // [rel]-style breadcrumbs: the reveal fails SILENTLY by design (a wrong
  // choreography is still a working window), so the log is the only witness.
  // The beta-2 report "no animations" was undiagnosable without these lines.
  if (!armed_) {
    LogInfo("reveal: not armed ({})", !enabled ? "un-minimize or caller declined"
                                               : "animations off in Windows");
    return;
  }

  // Which first frame is this? Read the tree, don't be told.
  signedInArmed_ = homeNav_.Visibility() == Visibility::Visible;
  LogInfo("reveal: armed ({} table)", signedInArmed_ ? "signed-in" : "signed-out");
  auto const& state = signedInArmed_ ? signedIn_ : signedOut_;

  ElementCompositionPreview::GetElementVisual(plate_).Opacity(0.0f);
  // Hero pose: Scale kHeroScaleFrom, XAML opacity 0, AnchorPoint {0,0}
  // ALWAYS, CenterPoint expression bound to Size (left running).
  urnw::motion::ArmHeroBloom(state.hero);
  for (auto const& ring : state.rings) {
    if (!ring.element) continue;
    if (ring.element.Visibility() == Visibility::Collapsed) continue;
    ring.element.Opacity(0.0);
    if (ring.riseDip != 0.0f) {
      ring.element.Translation({0.0f, ring.riseDip, 0.0f});
    }
  }
}

void WindowReveal::Start() {
  if (!armed_ || !plate_) return;
  // DEFERRED FINDING #1 (Task 1/2 ledger): the OS "show animations" toggle is
  // consulted twice on a reveal — once by Arm() (pre-Activate), and again,
  // transitively, by ArmHeroBloom/StartHeroBloom's own ShouldAnimate() gate —
  // and nothing stops it flipping true->false in between: Window.Activate()
  // pumps messages, and a settings change can land on this thread while it
  // does. If it flips, armed_ is still true (Arm() latched it before the
  // flip), but StartHeroBloom would silently no-op and leave the hero exactly
  // where Arm() posed it — Scale kHeroScaleFrom, Opacity 0 — forever, with no
  // animation ever coming to release it. A bare `return` here would be that
  // bug outright, so this early-out (the only one in this function that runs
  // after Arm has written poses) settles instead of abandoning them.
  if (!urnw::motion::ShouldAnimate()) {
    LogInfo("reveal: start fell back to settled pose (animations toggled off between arm and start)");
    CancelToFinal();
    return;
  }
  auto const& state = signedInArmed_ ? signedIn_ : signedOut_;
  auto plateVisual = ElementCompositionPreview::GetElementVisual(plate_);
  auto compositor = plateVisual.Compositor();
  auto standard = urnw::motion::MakeCompositionEasing(compositor, urnw::motion::kStandardP1,
                                                      urnw::motion::kStandardP2);

  // The stage's floor: plate alpha 0->1 over kFastMs, kStandard — unchanged
  // from the shipped reveal.
  auto plateFade = compositor.CreateScalarKeyFrameAnimation();
  plateFade.InsertKeyFrame(1.0f, 1.0f, standard);
  plateFade.Duration(urnw::motion::Ms(urnw::motion::kFastMs));
  plateVisual.StartAnimation(L"Opacity", plateFade);

  // The hero: 0.86/60 spring on Scale under a kHeroMs fade. The returned
  // board is retained so CancelToFinal can release the hero's Opacity DP.
  if (auto heroBoard = urnw::motion::StartHeroBloom(state.hero)) boards_.push_back(heroBoard);

  // The rings: two clocks per entry, BOTH started inside this one UI-thread
  // call — rises on Composition DelayTime, fades on Storyboard BeginTime.
  // Sub-frame drift between the clocks is fine; spreading starts across
  // ticks is not (spec §3.7 risk 6).
  for (auto const& ring : state.rings) {
    if (!ring.element) continue;
    // Same guard as Arm — and NEVER a Visibility write: a Collapsed ring
    // element belongs to its owner (spec §3.7 risk 4). Settle rather than
    // bare-skip: deferred finding #2, see SettleRing's comment above.
    if (ring.element.Visibility() == Visibility::Collapsed) {
      SettleRing(ring);
      continue;
    }
    if (ring.riseDip != 0.0f) {
      auto visual = ElementCompositionPreview::GetElementVisual(ring.element);
      auto rise = compositor.CreateScalarKeyFrameAnimation();
      rise.InsertKeyFrame(1.0f, 0.0f, standard);
      rise.Duration(urnw::motion::Ms(ring.riseMs));
      if (0 < ring.delayMs) rise.DelayTime(urnw::motion::Ms(ring.delayMs));
      visual.StartAnimation(L"Translation.Y", rise);
    }
    anim::Storyboard sb;
    auto fade = urnw::motion::MakeSplineDouble(0.0, 1.0, ring.fadeMs, ring.delayMs,
                                               urnw::motion::kStandardP1,
                                               urnw::motion::kStandardP2);
    anim::Storyboard::SetTarget(fade, ring.element);
    anim::Storyboard::SetTargetProperty(fade, L"Opacity");
    sb.Children().Append(fade);
    sb.Begin();
    boards_.push_back(sb);
  }
  LogInfo("reveal: started");
  armed_ = false;  // one-shot: a second Start() without an Arm() does nothing
}

void WindowReveal::CancelToFinal() {
  if (armed_) LogInfo("reveal: cancel-to-final while armed (hidden or superseded mid-bloom)");
  if (!plate_) return;
  // Boards first: a running Storyboard HOLDS its DP; stop releases it so the
  // XAML writes below actually land.
  for (auto const& board : boards_) board.Stop();
  boards_.clear();

  // BOTH heroes, unconditionally — the restore is auth-state-independent, so
  // it survives an auth flip between Arm and Cancel. CenterPoint expressions
  // are deliberately left running: they evaluate to the settled center.
  auto settleHero = [](winrt::Microsoft::UI::Xaml::FrameworkElement const& hero) {
    if (!hero) return;
    auto visual = ElementCompositionPreview::GetElementVisual(hero);
    visual.StopAnimation(L"Scale");
    visual.AnchorPoint({0.0f, 0.0f});
    visual.Scale({1.0f, 1.0f, 1.0f});
    hero.Opacity(1.0);
  };
  settleHero(signedIn_.hero);
  settleHero(signedOut_.hero);

  // Legacy defensive: the root no longer animates, but a prior build's
  // spring may have left Scale/Offset behind, and "cancel to final" includes
  // the origin geometry is measured from.
  if (revealRoot_) {
    auto rootVisual = ElementCompositionPreview::GetElementVisual(revealRoot_);
    rootVisual.StopAnimation(L"Scale");
    rootVisual.StopAnimation(L"Offset");
    rootVisual.AnchorPoint({0.0f, 0.0f});
    rootVisual.Scale({1.0f, 1.0f, 1.0f});
    rootVisual.Offset({0.0f, 0.0f, 0.0f});
  }

  auto plateVisual = ElementCompositionPreview::GetElementVisual(plate_);
  plateVisual.StopAnimation(L"Opacity");
  plateVisual.Opacity(1.0f);

  // The UNION of both tables regardless of which armed: opacity 1.0 /
  // Translation 0 on Collapsed elements is inert, and Visibility is NEVER
  // touched — step panels and home views own their own. SettleRing is the
  // SAME function Start()'s per-ring Collapsed skip-guard calls (deferred
  // finding #2 above) — one settled value, one implementation.
  for (auto const& ring : signedIn_.rings) SettleRing(ring);
  for (auto const& ring : signedOut_.rings) SettleRing(ring);

  armed_ = false;
}

}  // namespace urnw
