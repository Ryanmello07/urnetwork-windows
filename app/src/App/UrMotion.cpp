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

}  // namespace urnw::motion
