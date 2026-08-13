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

}  // namespace urnw::motion
