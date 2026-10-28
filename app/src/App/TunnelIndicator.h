// The slow-internet tunnel (apple/DELAY.md §2, §3): nested square outlines
// that deepen as the sdk's transport quality worsens, and the staggered fade
// that moves between depths.
//
// Geometry, relative to the box side S (§2.1):
//
//   * the outer square's stroke is 0.094 S, and the square is inset by half of
//     it, so the stroke fits the box exactly;
//   * each nested square is the outer one scaled by 0.70^i about the vanishing
//     point (0.55 S, 0.55 S), past the centre, so the left and top gaps open
//     wider than the right and bottom (about 1.65 times, between the strokes);
//   * each nested stroke is 0.60 times the one outside it;
//   * the corners are square, with mitered joins: rounded corners soften the
//     tunnel;
//   * square i draws at opacity 1 - 0.08 i, in the amber #F5C242.
//
// Depth follows the worse direction's quantized quality: 0, 1, 2, 4 and 6
// squares at 0, .25, .5, .75 and 1. Six fit legibly at 16 pt; a seventh would
// crowd the tight side.
//
// Motion (§3): the squares never move, so a change only fades squares in place.
// Deeper fades the new squares in from the outside inward, 420 ms each, ease
// out, started 110 ms apart, each settling from 1.06 times its size about the
// vanishing point. Shallower fades the old ones out from the inside outward,
// 320 ms each, 90 ms apart, easing back toward 1.06. A skipped level runs the
// same stagger for every square that changes. The pill around the tunnel
// appears with the first square and, on a change to none, waits until the last
// square starts to fade. Windows' "Show animations" off keeps the fades and
// their stagger and drops the scale (the shared contract's reduce-motion rule).
//
// Pure standard C++: no WinRT, no XAML, no sdk header, so tools/tunnel-
// indicator-tests.cpp runs it on any host. The connect page's pill
// (SlowInternetPill) draws from these numbers, and tools/make-icons draws the
// tray's tunnel icons from the same ones; tests/tunnel_indicator_test.go holds
// the generator's constants to these.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace urnw::tunnel {

// §2.1, relative to the box side.
inline constexpr double kOuterStroke = 0.094;
// each nested stroke, times the one outside it
inline constexpr double kStrokeRatio = 0.60;
// each nested square, times the one outside it, about the vanishing point
inline constexpr double kScaleRatio = 0.70;
// the vanishing point, on both axes
inline constexpr double kVanishingPoint = 0.55;
// square i draws at 1 - kOpacityStep * i
inline constexpr double kOpacityStep = 0.08;
// the full tunnel
inline constexpr int kMaxDepth = 6;
// the squares at the quantized qualities 0, .25, .5, .75 and 1
inline constexpr std::array<int, 5> kDepthByLevel = {0, 1, 2, 4, 6};

// §3.
inline constexpr int64_t kFadeInMillis = 420;
inline constexpr int64_t kFadeInStaggerMillis = 110;
inline constexpr int64_t kFadeOutMillis = 320;
inline constexpr int64_t kFadeOutStaggerMillis = 90;
// a square fading in starts this much larger, about the vanishing point, and
// settles to its size; one fading out eases back toward it
inline constexpr double kSettleScale = 1.06;

// The squares for a quality, 0 (normal) to 1 (worst); the indicator draws the
// worse direction's. At the sdk's 0.25 step this is the table above. At any
// other step the table is interpolated and rounded (§2.1), and a quality above
// 0 draws at least one square, as a level above 0 is at least one step.
inline int DepthForQuality(double quality) {
  // also NaN
  if (!(0 < quality)) return 0;
  if (1 <= quality) return kMaxDepth;
  const double position = quality * (kDepthByLevel.size() - 1);
  const int below = static_cast<int>(position);
  const double fraction = position - below;
  const double depth =
      kDepthByLevel[below] + (kDepthByLevel[below + 1] - kDepthByLevel[below]) * fraction;
  return std::clamp(static_cast<int>(std::lround(depth)), 1, kMaxDepth);
}

// The tray icon for a depth: one icon per row of the table above, so icon 1
// to 4 draws 1, 2, 4 and 6 squares. A depth between two rows takes the deeper
// one, so an interpolated depth never reads shallower than it is. 0 draws no
// tunnel.
inline int IconLevelForDepth(int depth) {
  if (depth <= 0) return 0;
  for (int level = 1; level < static_cast<int>(kDepthByLevel.size()); ++level) {
    if (depth <= kDepthByLevel[level]) return level;
  }
  return static_cast<int>(kDepthByLevel.size()) - 1;
}

// One square of the tunnel, in a box of side `size`: its outline's centre line
// is the square at (x, y) with sides `side`, drawn `stroke` wide with mitered
// corners at `opacity`. Index 0 is the outer square.
struct Square {
  double x = 0;
  double y = 0;
  double side = 0;
  double stroke = 0;
  double opacity = 0;
};

constexpr double Power(double base, int exponent) {
  double value = 1;
  for (int i = 0; i < exponent; ++i) value *= base;
  return value;
}

constexpr Square SquareAt(double size, int index) {
  const double outerStroke = kOuterStroke * size;
  // inset by half the stroke, so the stroke's outer edge is the box
  const double outerOrigin = outerStroke / 2;
  const double outerSide = size - outerStroke;
  const double vanishing = kVanishingPoint * size;
  const double scale = Power(kScaleRatio, index);
  Square square;
  square.x = vanishing + (outerOrigin - vanishing) * scale;
  square.y = square.x;
  square.side = outerSide * scale;
  square.stroke = outerStroke * Power(kStrokeRatio, index);
  square.opacity = 1 - kOpacityStep * index;
  return square;
}

// One square's fade in a change of depth.
struct Fade {
  int index = 0;
  // fading in (the tunnel deepens) or out
  bool in = false;
  // after the change
  int64_t beginMillis = 0;
  int64_t durationMillis = 0;
};

// The fades a change of depth runs, in the order they start: deeper fades the
// new squares in from the outside inward, shallower fades the old ones out
// from the inside outward, each staggered from the first square that changes.
// An unchanged depth runs nothing.
inline std::vector<Fade> FadesFor(int fromDepth, int toDepth) {
  fromDepth = std::clamp(fromDepth, 0, kMaxDepth);
  toDepth = std::clamp(toDepth, 0, kMaxDepth);
  std::vector<Fade> fades;
  if (fromDepth < toDepth) {
    for (int index = fromDepth; index < toDepth; ++index) {
      const int order = index - fromDepth;
      fades.push_back(Fade{index, true, order * kFadeInStaggerMillis, kFadeInMillis});
    }
  } else {
    for (int index = fromDepth - 1; toDepth <= index; --index) {
      const int order = fromDepth - 1 - index;
      fades.push_back(Fade{index, false, order * kFadeOutStaggerMillis, kFadeOutMillis});
    }
  }
  return fades;
}

// From the change to the last square settled: 0 to 6 builds in 970 ms, 6 to 0
// clears in 770 ms.
inline int64_t DurationFor(int fromDepth, int toDepth) {
  int64_t end = 0;
  for (const Fade& fade : FadesFor(fromDepth, toDepth)) {
    end = std::max(end, fade.beginMillis + fade.durationMillis);
  }
  return end;
}

// When the pill around the tunnel starts to fade on a change to no squares: as
// the last square starts to fade. It fades over kFadeOutMillis. Any other
// change keeps it (a change from none fades it in with the first square, at 0,
// over kFadeInMillis).
inline int64_t ContainerHideDelayFor(int fromDepth, int toDepth) {
  if (std::clamp(toDepth, 0, kMaxDepth) != 0) return 0;
  const std::vector<Fade> fades = FadesFor(fromDepth, toDepth);
  return fades.empty() ? 0 : fades.back().beginMillis;
}

}  // namespace urnw::tunnel
