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
// moves, containing a RevealRoot that springs (Scale + Offset) while the
// plate's own opacity ramps in underneath it. Nothing here ever touches the
// HWND, AppWindow, or window placement.
//
// THE PLATE FADE IS A COMPOSITION VISUAL OPACITY ANIMATION, not a Win32
// SetLayeredWindowAttributes/WS_EX_LAYERED fade of the whole window surface.
// The design plan flags that stronger version (E6) as an unverified
// capability — WS_EX_LAYERED over a WinUI 3 DirectComposition island is not
// universally safe, and proving it needs a real screen capture of an ACTIVE
// window, the same verification gap that let the Mica regression ship
// (WindowShell.cpp) — with an explicit sanctioned fallback of "no plate
// fade". This file starts ON that fallback rather than the unverified path,
// since nobody has produced that proof yet; the effect it still delivers
// (RevealRoot's spring + the opacity ripple, both ordinary, safe Composition
// APIs) is most of what reads as "fancy" in the choreography anyway.
namespace urnw {
namespace {
using winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview;
using winrt::Microsoft::UI::Xaml::UIElement;
using winrt::Microsoft::UI::Xaml::Visibility;
namespace anim = winrt::Microsoft::UI::Xaml::Media::Animation;

// A ~20dip cosmetic nudge, not a placement — deliberately NOT DPI-corrected.
// The window's own physical/DIP split lives one layer down, in WindowShell;
// a Composition Visual hosted by XAML already operates in the same effective
// (DIP) coordinate space as its XAML content, so this is consistent with
// every other distance in UrMotion.h.
constexpr float kOffsetDip = 20.0f;

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

// TRANSITIONAL (Task 2): today's root-spring/opacity-fade behavior, now
// reading the ACTIVE per-state table instead of the old flat `rings_`. The
// full Hero Bloom choreography (per-ring rise + hero bloom spring) lands in
// Task 3 — this keeps the build green and the tables Bind-level-asserted
// while that rewrite is staged.
void WindowReveal::Arm(bool enabled, std::optional<POINT> originScreen,
                       RECT const& windowScreenRect) {
  if (!plate_ || !revealRoot_) return;
  // A reveal still running from a PREVIOUS show (hide-to-tray mid-animation,
  // then clicked again quickly) settles before this one writes its own start
  // pose — two overlapping springs on the same visual is how E6's "nastiest
  // failure mode" starts.
  CancelToFinal();
  armed_ = enabled && urnw::motion::ShouldAnimate();
  if (!armed_) return;

  auto plateVisual = ElementCompositionPreview::GetElementVisual(plate_);
  auto rootVisual = ElementCompositionPreview::GetElementVisual(revealRoot_);
  // Relative (0..1) anchor, not an absolute CenterPoint: Arm() runs BEFORE
  // Activate() (E4), so on a brand-new window ActualWidth/Height can still be
  // 0 at this instant — a relative anchor scales correctly regardless of
  // whether XAML has measured yet.
  // CenterPoint bound to Size, NOT AnchorPoint. AnchorPoint is the point ON
  // the visual that gets placed at Offset, so setting it to (0.5, 0.5)
  // DISPLACES the content by -0.5*Size -- half its own width and height, up
  // and to the left -- and keeps it there. The spring below animates Scale
  // and Offset back to their settled values and completes cleanly, so the
  // reveal reports success while leaving every subsequent frame shifted off
  // the left edge. That shipped in v2026.8.13-1018112070-beta and is exactly
  // the failure CancelToFinal was written to prevent, one property over.
  //
  // CenterPoint moves only the origin that Scale and Rotation are applied
  // about; it never repositions the visual. The reason AnchorPoint was
  // reached for -- Arm() runs BEFORE Activate() (E4), so ActualWidth/Height
  // can still be 0 here -- is solved properly by an ExpressionAnimation that
  // tracks the visual's own Size: it re-evaluates as XAML measures, so the
  // center is right by the time Start() runs, and stays right across resize.
  rootVisual.AnchorPoint({0.0f, 0.0f});
  auto centerBind = rootVisual.Compositor().CreateExpressionAnimation(
      L"Vector3(this.Target.Size.X * 0.5f, this.Target.Size.Y * 0.5f, 0.0f)");
  rootVisual.StartAnimation(L"CenterPoint", centerBind);

  // Origin-anchored (E2), direction only — see the file comment on kOffsetDip
  // for why this is not a full screen-space projection. Vertical: the tray
  // sits at the screen edge nearest the icon, which for the overwhelming
  // majority of Windows desktops is the bottom, so content settles UP into
  // place. Horizontal: which side of the window the anchor falls on, when one
  // was found at all.
  float dirX = 0.0f;
  if (originScreen) {
    const float windowCenterX =
        static_cast<float>(windowScreenRect.left + windowScreenRect.right) / 2.0f;
    dirX = static_cast<float>(originScreen->x) < windowCenterX ? -1.0f : 1.0f;
  }
  constexpr float kDirY = 1.0f;  // settle upward, toward the taskbar edge

  rootVisual.Scale({0.94f, 0.94f, 1.0f});
  rootVisual.Offset({dirX * kOffsetDip, kDirY * kOffsetDip, 0.0f});
  plateVisual.Opacity(0.0f);
  // Geometry and opacity are kept strictly separate (E3): the rings start
  // invisible on the XAML Opacity DP, independent of the plate/root's
  // Composition-layer animation above, so nested alpha never compounds.
  // ACTIVE table, latched from the tree — Task 3 does the full per-ring
  // rise/bloom; this transitional body only needs which rings exist.
  signedInArmed_ = homeNav_ && homeNav_.Visibility() == Visibility::Visible;
  auto const& active = signedInArmed_ ? signedIn_ : signedOut_;
  for (auto const& ring : active.rings) {
    if (!ring.element) continue;
    if (ring.element.Visibility() == Visibility::Collapsed) continue;  // spec §3.7 risk 4
    ring.element.Opacity(0.0);
  }
}

void WindowReveal::Start() {
  if (!armed_ || !plate_ || !revealRoot_) return;
  auto plateVisual = ElementCompositionPreview::GetElementVisual(plate_);
  auto rootVisual = ElementCompositionPreview::GetElementVisual(revealRoot_);
  auto compositor = rootVisual.Compositor();

  // Plate alpha 0->1 over Fast150, Standard ease (E3).
  auto plateFade = compositor.CreateScalarKeyFrameAnimation();
  plateFade.InsertKeyFrame(
      1.0f, 1.0f, urnw::motion::MakeCompositionEasing(compositor, urnw::motion::kStandardP1,
                                                       urnw::motion::kStandardP2));
  plateFade.Duration(urnw::motion::Ms(urnw::motion::kFastMs));
  plateVisual.StartAnimation(L"Opacity", plateFade);

  // RevealRoot Scale 0.94->1 and Offset -> 0, both on the reveal spring
  // (damping 0.86, period 60ms — E2's "spring, not easing"; the existing
  // 0.75/40ms spring reserved for the Connect button visibly wobbles a
  // surface this size). Composition is the only tool in this app that can do
  // this at all: Storyboard has no spring easing.
  using winrt::Windows::Foundation::Numerics::float3;
  auto scaleSpring = compositor.CreateSpringVector3Animation();
  scaleSpring.DampingRatio(urnw::motion::kRevealSpringDamping);
  scaleSpring.Period(urnw::motion::Ms(urnw::motion::kRevealSpringPeriodMs));
  scaleSpring.FinalValue(float3{1.0f, 1.0f, 1.0f});
  rootVisual.StartAnimation(L"Scale", scaleSpring);

  auto offsetSpring = compositor.CreateSpringVector3Animation();
  offsetSpring.DampingRatio(urnw::motion::kRevealSpringDamping);
  offsetSpring.Period(urnw::motion::Ms(urnw::motion::kRevealSpringPeriodMs));
  offsetSpring.FinalValue(float3{0.0f, 0.0f, 0.0f});
  rootVisual.StartAnimation(L"Offset", offsetSpring);

  // The opacity ripple (E3), staggered after the geometry starts. Ordinary
  // Storyboard fades on the XAML DP — the same family every other hand-built
  // animation in this app uses — not Composition, keeping geometry and
  // opacity on two independent systems as the plan specifies. ACTIVE table,
  // per-ring fadeMs, ring.delayMs as BeginTime — never force-Visible (the
  // ring's owner controls Visibility); every board retained so
  // CancelToFinal can stop it before the union restore writes.
  auto const& active = signedInArmed_ ? signedIn_ : signedOut_;
  for (auto const& ring : active.rings) {
    if (!ring.element) continue;
    if (ring.element.Visibility() == Visibility::Collapsed) continue;
    anim::Storyboard sb;
    auto fade = urnw::motion::MakeSplineDouble(0.0, 1.0, ring.fadeMs, ring.delayMs,
                                               urnw::motion::kStandardP1, urnw::motion::kStandardP2);
    anim::Storyboard::SetTarget(fade, ring.element);
    anim::Storyboard::SetTargetProperty(fade, L"Opacity");
    sb.Children().Append(fade);
    sb.Begin();
    boards_.push_back(sb);
  }
  armed_ = false;  // one-shot: a second Start() without an intervening Arm() does nothing
}

void WindowReveal::CancelToFinal() {
  if (!plate_ || !revealRoot_) return;
  auto plateVisual = ElementCompositionPreview::GetElementVisual(plate_);
  auto rootVisual = ElementCompositionPreview::GetElementVisual(revealRoot_);
  // StopAnimation leaves the property wherever the animation last wrote it —
  // explicitly setting the settled pose after stopping is the fix for E6's
  // "nastiest failure mode": hiding mid-reveal must not leave Scale pinned at
  // ~0.96 forever, rendering every subsequent open a fraction small with no
  // error anywhere.
  rootVisual.StopAnimation(L"Scale");
  rootVisual.StopAnimation(L"Offset");
  plateVisual.StopAnimation(L"Opacity");
  // AnchorPoint is restored too: a settled pose means geometry AND the
  // origin it is measured from, otherwise "cancel to final" still leaves the
  // content displaced by half its size.
  rootVisual.AnchorPoint({0.0f, 0.0f});
  rootVisual.Scale({1.0f, 1.0f, 1.0f});
  rootVisual.Offset({0.0f, 0.0f, 0.0f});
  plateVisual.Opacity(1.0f);
  // Boards first: a running Storyboard HOLDS its DP; stop releases it so the
  // Opacity writes below actually land.
  for (auto const& board : boards_) board.Stop();
  boards_.clear();
  // The UNION of both tables regardless of which armed, plus both heroes:
  // Opacity 1.0 on a Collapsed element is inert, and Visibility is NEVER
  // touched here — step panels and home views own their own.
  for (auto const& ring : signedIn_.rings) {
    if (ring.element) ring.element.Opacity(1.0);
  }
  for (auto const& ring : signedOut_.rings) {
    if (ring.element) ring.element.Opacity(1.0);
  }
  if (signedIn_.hero) signedIn_.hero.Opacity(1.0);
  if (signedOut_.hero) signedOut_.hero.Opacity(1.0);
  armed_ = false;
}

}  // namespace urnw
