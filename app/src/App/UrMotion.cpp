// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "UrMotion.h"

#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.ViewManagement.h>

// Two animation families live in this app, on purpose, not by accident:
//
//   Storyboard/DoubleAnimation — every hand-built animation that shipped
//   before this file (ConnectCanvas's fades and blob slides, LoginCarousel's
//   crossfade) uses it, it is a first-class WinUI built-in (not a bespoke
//   timer), and it keeps a XAML dependency property and its Composition-layer
//   visual in agreement automatically. CrossfadePageSwap below joins that
//   family so it composes with the code around it instead of introducing a
//   second, competing way to fade an element.
//
//   Composition (Compositor/Visual) — reserved for motion Storyboard cannot
//   express at all, chiefly the window reveal's spring
//   (CreateSpringVector3Animation has no Storyboard equivalent — see
//   WindowReveal.cpp). This file's MakeCompositionEasing exists for that one
//   consumer.
//
// UI THREAD ONLY.

namespace urnw::motion {
namespace {
namespace anim = winrt::Microsoft::UI::Xaml::Media::Animation;
using winrt::Microsoft::UI::Xaml::Duration;
using winrt::Microsoft::UI::Xaml::DurationType;
using winrt::Windows::Foundation::TimeSpan;
}  // namespace

bool ShouldAnimate() {
  // "Show animations in Windows" off means the user wants motion GONE, not
  // reduced — same reading ConnectCanvas::AnimationsEnabled already used, now
  // the one place every OTHER animation in the app asks too.
  try {
    return winrt::Windows::UI::ViewManagement::UISettings().AnimationsEnabled();
  } catch (...) {
    return true;
  }
}

TimeSpan Ms(int64_t ms) {
  return std::chrono::duration_cast<TimeSpan>(std::chrono::milliseconds(ms));
}

Duration XamlDuration(int64_t ms) { return Duration{Ms(ms), DurationType::TimeSpan}; }

anim::DoubleAnimationUsingKeyFrames MakeSplineDouble(double from, double to, int64_t ms,
                                                      int64_t beginMs, Bezier p1, Bezier p2) {
  anim::KeySpline spline;
  spline.ControlPoint1(p1);
  spline.ControlPoint2(p2);

  anim::SplineDoubleKeyFrame startFrame;
  startFrame.KeyTime(anim::KeyTime{TimeSpan{0}});
  startFrame.Value(from);

  anim::SplineDoubleKeyFrame endFrame;
  endFrame.KeyTime(anim::KeyTime{Ms(ms)});
  endFrame.Value(to);
  endFrame.KeySpline(spline);

  anim::DoubleAnimationUsingKeyFrames animation;
  animation.KeyFrames().Append(startFrame);
  animation.KeyFrames().Append(endFrame);
  if (0 < beginMs) animation.BeginTime(Ms(beginMs));
  return animation;
}

winrt::Microsoft::UI::Composition::CompositionEasingFunction MakeCompositionEasing(
    winrt::Microsoft::UI::Composition::Compositor const& compositor, Bezier p1, Bezier p2) {
  return compositor.CreateCubicBezierEasingFunction({p1.X, p1.Y}, {p2.X, p2.Y});
}

namespace {

// Both fades share one storyboard so they finish together: an independent
// fade-out and fade-in can end a frame or two apart, which reads as a flicker
// exactly at the moment the incoming page settles.
void RunCrossfade(winrt::Microsoft::UI::Xaml::FrameworkElement const& outgoing,
                  winrt::Microsoft::UI::Xaml::FrameworkElement const& incoming) {
  namespace xaml = winrt::Microsoft::UI::Xaml;
  incoming.Opacity(0.0);
  incoming.Visibility(xaml::Visibility::Visible);

  anim::Storyboard sb;
  // Entrance: the default page-transition duration, the Standard ease (settle,
  // not snap).
  auto fadeIn = MakeSplineDouble(0.0, 1.0, kBaseMs, 0, kStandardP1, kStandardP2);
  anim::Storyboard::SetTarget(fadeIn, incoming);
  anim::Storyboard::SetTargetProperty(fadeIn, L"Opacity");
  sb.Children().Append(fadeIn);

  if (outgoing) {
    // Exit: one step faster (kFastMs vs kBaseMs) and the Exit ease — dismissing
    // a page should never feel slower than presenting the next one.
    auto fadeOut = MakeSplineDouble(1.0, 0.0, kFastMs, 0, kExitP1, kExitP2);
    anim::Storyboard::SetTarget(fadeOut, outgoing);
    anim::Storyboard::SetTargetProperty(fadeOut, L"Opacity");
    sb.Children().Append(fadeOut);
    sb.Completed([outgoing](auto const&, auto const&) {
      // Guard against a second swap starting (and re-showing `outgoing`)
      // before this Completed fires: only collapse it if it is still the one
      // fading out.
      if (outgoing.Opacity() <= 0.01) {
        outgoing.Visibility(xaml::Visibility::Collapsed);
        outgoing.Opacity(1.0);  // restored for its NEXT entrance
      }
    });
  }
  sb.Begin();
}

}  // namespace

void CrossfadePageSwap(winrt::Microsoft::UI::Xaml::FrameworkElement const& outgoing,
                       winrt::Microsoft::UI::Xaml::FrameworkElement const& incoming) {
  namespace xaml = winrt::Microsoft::UI::Xaml;
  if (!incoming) return;
  if (outgoing == incoming) {
    incoming.Opacity(1.0);
    incoming.Visibility(xaml::Visibility::Visible);
    return;
  }
  if (!ShouldAnimate()) {
    if (outgoing) {
      outgoing.Visibility(xaml::Visibility::Collapsed);
      outgoing.Opacity(1.0);
    }
    incoming.Opacity(1.0);
    incoming.Visibility(xaml::Visibility::Visible);
    return;
  }
  RunCrossfade(outgoing, incoming);
}

void EnableTranslation(winrt::Microsoft::UI::Xaml::UIElement const& element) {
  if (!element) return;
  winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::SetIsTranslationEnabled(
      element, true);
}

void ArmHeroBloom(winrt::Microsoft::UI::Xaml::FrameworkElement const& hero) {
  if (!hero || !ShouldAnimate()) return;
  auto visual =
      winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(hero);
  // AnchorPoint {0,0} ALWAYS; the scale origin is CenterPoint, bound to the
  // visual's own Size (see the header comment for the shipped bug this rule
  // exists to prevent).
  visual.AnchorPoint({0.0f, 0.0f});
  auto centerBind = visual.Compositor().CreateExpressionAnimation(
      L"Vector3(this.Target.Size.X * 0.5f, this.Target.Size.Y * 0.5f, 0.0f)");
  visual.StartAnimation(L"CenterPoint", centerBind);
  visual.Scale({kHeroScaleFrom, kHeroScaleFrom, 1.0f});
  hero.Opacity(0.0);
}

winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard StartHeroBloom(
    winrt::Microsoft::UI::Xaml::FrameworkElement const& hero, float dampingRatio) {
  if (!hero || !ShouldAnimate()) return nullptr;
  auto visual =
      winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(hero);
  auto compositor = visual.Compositor();
  using winrt::Windows::Foundation::Numerics::float3;
  auto spring = compositor.CreateSpringVector3Animation();
  spring.DampingRatio(dampingRatio);
  spring.Period(Ms(kRevealSpringPeriodMs));
  spring.FinalValue(float3{1.0f, 1.0f, 1.0f});
  visual.StartAnimation(L"Scale", spring);

  anim::Storyboard sb;
  auto fade = MakeSplineDouble(0.0, 1.0, kHeroMs, 0, kStandardP1, kStandardP2);
  anim::Storyboard::SetTarget(fade, hero);
  anim::Storyboard::SetTargetProperty(fade, L"Opacity");
  sb.Children().Append(fade);
  sb.Begin();
  return sb;
}

void RiseIn(winrt::Microsoft::UI::Xaml::FrameworkElement const& element, Rise direction,
            double distDip, int64_t delayMs, int64_t fadeMs, int64_t riseMs) {
  namespace xaml = winrt::Microsoft::UI::Xaml;
  if (!element) return;
  // Skip-if-Collapsed, at the choke point: never force-Visible, never pose a
  // hidden element — its owner controls its Visibility.
  if (element.Visibility() == xaml::Visibility::Collapsed) return;
  if (!ShouldAnimate()) return;  // current pose IS the settled pose

  if (distDip != 0.0) {
    using winrt::Windows::Foundation::Numerics::float3;
    EnableTranslation(element);  // idempotent; a silent no-op otherwise
    const float fromY = static_cast<float>(direction == Rise::Up ? distDip : -distDip);
    auto visual = xaml::Hosting::ElementCompositionPreview::GetElementVisual(element);
    element.Translation(float3{0.0f, fromY, 0.0f});  // pre-delay frames correct
    auto rise = visual.Compositor().CreateScalarKeyFrameAnimation();
    rise.InsertKeyFrame(1.0f, 0.0f,
                        MakeCompositionEasing(visual.Compositor(), kStandardP1, kStandardP2));
    rise.Duration(Ms(riseMs));
    if (0 < delayMs) rise.DelayTime(Ms(delayMs));
    visual.StartAnimation(L"Translation.Y", rise);
  }

  // Alpha on the XAML clock (BeginTime = the same delay). Both clocks start
  // in this one call — sub-frame drift is fine, spread starts are not.
  element.Opacity(0.0);
  anim::Storyboard sb;
  auto fade = MakeSplineDouble(0.0, 1.0, fadeMs, delayMs, kStandardP1, kStandardP2);
  anim::Storyboard::SetTarget(fade, element);
  anim::Storyboard::SetTargetProperty(fade, L"Opacity");
  sb.Children().Append(fade);
  sb.Begin();
}

void RippleGroup(std::vector<RippleEntry> const& elements, int64_t baseDelayMs,
                 int64_t staggerMs) {
  int64_t slot = 0;
  for (auto const& entry : elements) {
    // slot advances for EVERY listed entry, present or not: delays are a
    // property of the composition, not of what happens to be visible.
    RiseIn(entry.element, entry.direction, entry.distDip, baseDelayMs + slot * staggerMs);
    ++slot;
  }
}

}  // namespace urnw::motion
