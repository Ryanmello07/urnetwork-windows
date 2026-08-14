// The motion system: one place for durations, easings, distances and the
// standard enter/exit transition, so later animation work composes off these
// tokens instead of hand-rolling new ones. See the design plan (Phase C):
// docs/superpowers/plans/2026-08-11-design-update-super-plan.md.
//
// UI THREAD ONLY. Nothing in this file may run on the packet or placement
// path — it touches XAML/Composition objects exclusively.
//
// Two rules every animation in this app must follow, enforced here at one
// choke point rather than at each call site:
//   - ShouldAnimate() gates every non-trivial animation. "Show animations in
//     Windows" off means the user wants motion GONE, not reduced, and a
//     disabled user must get an instant, non-broken UI.
//   - Exits run one step faster than entrances (kFastMs vs kBaseMs, etc.) —
//     dismissing something should never feel slower than presenting it.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <vector>

#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Windows.Foundation.h>

namespace urnw::motion {

// ---- durations (ms) --------------------------------------------------------
inline constexpr int64_t kMicroMs = 90;    // a tick of feedback
inline constexpr int64_t kFastMs = 150;    // hover/press; exits
inline constexpr int64_t kBaseMs = 250;    // the default: page crossfades, disclosure
inline constexpr int64_t kSlowMs = 400;    // a large-surface change
// = ConnectCanvas::kStateFadeMs / kBlobMs. Not redeclared as the canonical
// values there — those two constants stay put so this pass does not touch a
// file with its own careful, already-shipped timing — but every NEW timeline
// in the app should reach for these names rather than inventing a seventh
// duration.
inline constexpr int64_t kHeroMs = 500;
inline constexpr int64_t kEpicMs = 1000;
inline constexpr int64_t kPulseMs = 1500;  // the idle invitation burst

// stagger between list/ripple steps, and how much two overlapping timelines
// may share before the second is judged to have started too soon
inline constexpr int64_t kStaggerMs = 40;
inline constexpr int kMaxStaggerSteps = 6;
inline constexpr int64_t kOverlapMs = 60;

// ---- easings ----------------------------------------------------------------
// Cubic-bezier control points (P1, P2 — P0=(0,0) and P3=(1,1) are implicit,
// as every CSS/Compositor cubic-bezier assumes).
using Bezier = winrt::Windows::Foundation::Point;
inline constexpr Bezier kStandardP1{0.10f, 0.90f};  // the default: settle, don't snap
inline constexpr Bezier kStandardP2{0.20f, 1.00f};
inline constexpr Bezier kExitP1{0.70f, 0.00f};  // dismissing something: leave fast
inline constexpr Bezier kExitP2{1.00f, 0.50f};
inline constexpr Bezier kSoftP1{0.40f, 0.00f};  // a gentle disclosure (Advanced Mode reveal)
inline constexpr Bezier kSoftP2{0.20f, 1.00f};

// ---- springs (Composition natural-motion) -----------------------------------
// damping ratio + period. The Connect button's spring is RESERVED here for a
// later pass — ConnectCanvas already has its own careful, shipped motion and
// this pass does not touch it. The reveal spring is what Phase E actually
// spends: a bigger, heavier surface (480x760dip) needs more damping and a
// slower period than a button, or it visibly overshoots.
inline constexpr float kConnectSpringDamping = 0.75f;
inline constexpr int64_t kConnectSpringPeriodMs = 40;
inline constexpr float kRevealSpringDamping = 0.86f;
inline constexpr int64_t kRevealSpringPeriodMs = 60;

// ---- distances (dip) / scale ------------------------------------------------
inline constexpr double kDist4 = 4.0;
inline constexpr double kDist8 = 8.0;
inline constexpr double kDist12 = 12.0;
inline constexpr double kDist24 = 24.0;
inline constexpr double kHoverScale = 1.03;
inline constexpr double kPressScale = 0.97;

// ---- Hero Bloom (motion-overhaul spec §2.1) ---------------------------------
// The held brand beat: the hero owns the frame for ~two visual fixations
// before the unfold. =6*kStaggerMs so it sits on the existing stagger grid;
// shorter reads as no beat, longer reads as a stall.
inline constexpr int64_t kHeroHoldMs = 240;
// =kHeroHoldMs/2 =3*kStaggerMs: the wordmark (and avatar) join mid-hero-settle
// so logo+hero read as one brand moment.
inline constexpr int64_t kBrandBeatMs = 120;
// ~15dip visible travel on the <=190dip globe, ~40dip on the ~512dip art
// card; deep enough to register as the composition's origin, and the 0.86/60
// spring damps both sizes without visible overshoot.
inline constexpr float kHeroScaleFrom = 0.92f;
static_assert(kBrandBeatMs * 2 == kHeroHoldMs, "the brand beat is half the hold");
static_assert(kHeroHoldMs == 6 * kStaggerMs, "the hold sits on the stagger grid");

// ---- the one reduce-motion choke point --------------------------------------
// Consult this before starting ANY animation that is not a stock XAML
// VisualState transition (those are the platform's own business). Before this
// file, UISettings was read only inside ConnectCanvas — LoginCarousel and the
// page-transition code had no gate at all.
bool ShouldAnimate();

// ---- TimeSpan / Duration helpers --------------------------------------------
winrt::Windows::Foundation::TimeSpan Ms(int64_t ms);
winrt::Microsoft::UI::Xaml::Duration XamlDuration(int64_t ms);

// A Storyboard double animation that follows an EXACT cubic-bezier, built on
// KeySpline/SplineDoubleKeyFrame — plain DoubleAnimation.EasingFunction only
// accepts the platform's named easing classes (CubicEase, EaseOut, ...), none
// of which take arbitrary control points, so a single-keyframe spline is how
// the Standard/Exit/Soft tokens above become a REAL curve rather than a
// decorative constant. For the hand-built Storyboard animations already
// established in this app (ConnectCanvas, LoginCarousel, and the page
// crossfade below all animate through Storyboard/DoubleAnimation rather than
// raw Composition — see UrMotion.cpp's file comment for why that split is
// deliberate). `beginMs` is a stagger delay, 0 for none.
winrt::Microsoft::UI::Xaml::Media::Animation::DoubleAnimationUsingKeyFrames MakeSplineDouble(
    double from, double to, int64_t ms, int64_t beginMs, Bezier p1, Bezier p2);

// The Composition equivalent, for code that already holds a Compositor (the
// window reveal, Phase E, which needs Composition for its spring and cannot
// get one from Storyboard).
winrt::Microsoft::UI::Composition::CompositionEasingFunction MakeCompositionEasing(
    winrt::Microsoft::UI::Composition::Compositor const& compositor, Bezier p1, Bezier p2);

// ---- the standard page transition ------------------------------------------
// There is no Frame in this window — seven sibling Grids toggled by
// Visibility (MainWindow.xaml.cpp) — so NavigationTransitionInfo is
// unreachable and this is the hand-built replacement every navigation and
// every page's first reveal goes through. `outgoing` may be null (nothing was
// showing yet): that is what the three former ConnectPage::AnimateDrawerIn
// call sites become — the drawer's one-shot entrance was a page transition
// with no outgoing page, not a different mechanism.
//
// Falls back to an instant swap when ShouldAnimate() is false. Safe to call
// with outgoing == incoming (no-ops other than ensuring it is visible).
void CrossfadePageSwap(winrt::Microsoft::UI::Xaml::FrameworkElement const& outgoing,
                       winrt::Microsoft::UI::Xaml::FrameworkElement const& incoming);

// ---- the four motion primitives (motion-overhaul spec §2.2) -----------------
// All gated on ShouldAnimate(); all keep the house split — geometry on
// Composition visuals, opacity on XAML Storyboard DPs, never compounded
// alpha. UI THREAD ONLY, like everything else in this file.

// Which way a RiseIn element travels as it settles INTO place. Up starts
// distDip BELOW its final slot (Translation.Y = +dist) and settles up; Down
// starts above (-dist) and settles down. The window open uses both: the
// outward bloom.
enum class Rise { Up, Down };

// Hero Bloom, split at the Arm/Start seam the window reveal needs (pose
// written synchronously pre-Activate, animation post-Activate). ArmHeroBloom
// writes the start pose: visual Scale = kHeroScaleFrom, XAML Opacity = 0,
// AnchorPoint {0,0} — ALWAYS {0,0}; (0.5,0.5) displaces content by half its
// size and shipped as v2026.8.13's off-screen bug — and a CenterPoint
// ExpressionAnimation bound to this.Target.Size*0.5, left running (it
// re-evaluates as XAML measures, so the center is right before layout and
// stays right across resize). StartHeroBloom plays the 0.86/60 reveal spring
// on Scale to (1,1,1) under a kHeroMs kStandard opacity fade, and RETURNS the
// fade's Storyboard so the caller can retain and stop it (CancelToFinal must
// release the DP). dampingRatio exists for the one sanctioned override: 0.90
// for the signed-out art card if it visibly wobbles (spec §3.7 risk 7).
void ArmHeroBloom(winrt::Microsoft::UI::Xaml::FrameworkElement const& hero);
winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard StartHeroBloom(
    winrt::Microsoft::UI::Xaml::FrameworkElement const& hero,
    float dampingRatio = kRevealSpringDamping);

// SetIsTranslationEnabled(true), once, idempotent. Forgetting it before the
// first Translation write is a SILENT no-op that reads as "the stagger feels
// flat", never as an error.
void EnableTranslation(winrt::Microsoft::UI::Xaml::UIElement const& element);

// RiseIn: XAML opacity 0->1 over fadeMs riding a Composition Translation.Y
// ±distDip->0 over riseMs — motion outlives alpha (riseMs > fadeMs), so the
// element is readable while it settles the last dip into place. Immediate
// mode: writes its own start pose, enables translation itself, then plays.
// Skip-if-Collapsed lives HERE, at the choke point. distDip 0 degrades to a
// pure fade (how opacity-only riders share the primitive).
void RiseIn(winrt::Microsoft::UI::Xaml::FrameworkElement const& element,
            Rise direction, double distDip, int64_t delayMs,
            int64_t fadeMs = kBaseMs, int64_t riseMs = kSlowMs);

// RippleGroup: staggered RiseIns. Entry i plays at baseDelayMs + i*staggerMs
// by LISTING position — a Collapsed entry is skipped but holds its slot, so
// delays never shift with auth-conditional visibility. staggerMs 0 moves a
// group as one row (the status-row rule: riders move WITH the row or it
// shears).
struct RippleEntry {
  winrt::Microsoft::UI::Xaml::FrameworkElement element{nullptr};
  Rise direction = Rise::Up;
  double distDip = kDist8;
};
void RippleGroup(std::vector<RippleEntry> const& elements, int64_t baseDelayMs,
                 int64_t staggerMs = kStaggerMs);

}  // namespace urnw::motion
