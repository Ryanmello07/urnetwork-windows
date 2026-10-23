// The extender rings a provider dot wears (connect/EXTENDER.md K2, K3): one
// ring per extender address carrying this client's live transports to that
// exit, in the colour the SDK paired with the address.
//
// K2 states the geometry in absolute pixels:
//
//   * stroke 2 px,
//   * a 2 px gap between the filled dot and the first ring and between
//     successive rings,
//   * the OUTERMOST ring's outer edge sits exactly on the cell edge, so a dot
//     with rings has the same footprint as one without and never grows into a
//     neighbour,
//   * which means the filled dot shrinks inward by 4 px (stroke + gap) per
//     ring,
//   * at most three rings; four or more collapse into a dashed third ring.
//
// THE ONE PLACE THIS FILE ADDS A RULE. Those are absolute pixels, and the
// connect hero's cell is not: it is the globe's side over the grid's larger
// dimension, which on this app is roughly 168..288 over 14..16 -- a cell of
// 10 to 20 px. Three rings at 4 px of radius each is 12 px of radius, so the
// literal rule leaves a dot of zero or less diameter at every size the hero
// actually draws, and the provider would vanish the moment it gained a third
// extender. A dot that disappears is worse than a ring that is a little
// thinner, so the ring budget is SCALED DOWN, uniformly (stroke and gap keep
// their 1:1 ratio), whenever the nominal budget would leave the filled dot
// smaller than kExtenderMinDotFraction of the cell. Above that size -- the
// drawer's dots at a small grid, and every surface that draws a dot at a
// deliberate size, such as the account panel's rings -- the numbers are K2's
// exactly. `ExtenderRingsFor(cell, marks).scaled` reports which happened.
//
// The provider rows and their globe (K9) ask for the other trade,
// ExtenderRingFit::Drop. Their dots are small -- the rows' 12 px, and the
// globe's 14 units of a 600-unit space, a few pixels on screen -- where one
// full ring reads and two thinned ones blur into one. So K2's numbers are
// kept, the rings past what the cell holds above the same dot floor are
// dropped, and the last one drawn is dashed for them, as a fourth extender
// dashes the third ring: either dot holds one ring and a second extender
// dashes it, as on the other apps.
//
// Pure standard C++: no WinRT, no XAML, no SDK header, so tools/extender-
// tests.cpp runs it on any host and every drawing surface (ConnectCanvas,
// ExtenderPanel, the provider rows and their globe) shares one source of truth
// for the numbers.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace urnw {

// K2's numbers, in the pixels K2 states them in.
inline constexpr double kExtenderRingStroke = 2.0;
inline constexpr double kExtenderRingGap = 2.0;
// the radial budget one ring costs the filled dot
inline constexpr double kExtenderRingStep = kExtenderRingStroke + kExtenderRingGap;
// "At most three rings are drawn; four or more collapse into a dashed third
// ring." The dashed ring is the THIRD one (the innermost of the three), so a
// dot with four extenders still reads as three rings, the last one open.
inline constexpr int kExtenderMaxRings = 3;
// The floor under the filled dot, as a fraction of the cell. See the header
// comment: this is what keeps a three-ring provider visible at the hero's own
// cell size. A third of the cell is still unmistakably a dot.
inline constexpr double kExtenderMinDotFraction = 1.0 / 3.0;
// A ring thinner than this is not a ring, it is an artifact of a rounding
// error, so the scale never goes below it.
inline constexpr double kExtenderMinRingStroke = 0.5;

// The dash pattern of the collapsed third ring, in stroke widths (XAML's
// StrokeDashArray unit), so it scales with the stroke rather than with the
// cell.
inline constexpr double kExtenderRingDashOn = 2.0;
inline constexpr double kExtenderRingDashOff = 2.0;

// What a cell too small for its rings at K2's numbers gives up so its filled
// dot keeps kExtenderMinDotFraction of the cell (see the header comment).
enum class ExtenderRingFit {
  // the stroke and the gap, together: every ring is drawn, thinner. The
  // connect hero.
  Scale,
  // rings: K2's stroke and gap are kept and the rings that do not fit are
  // dropped, the last one drawn dashed for them. The provider rows and their
  // globe.
  Drop,
};

struct ExtenderRgb {
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
  bool operator==(const ExtenderRgb& o) const { return r == o.r && g == o.g && b == o.b; }
  bool operator!=(const ExtenderRgb& o) const { return !(*this == o); }
};

// One extender on a dot: the address and the colour the SDK computed for it
// (K3 -- every app draws the value as given, none of them computes a hue).
struct ExtenderMark {
  std::string ip;
  std::string colorHex;
  bool operator==(const ExtenderMark& o) const {
    return ip == o.ip && colorHex == o.colorHex;
  }
  bool operator!=(const ExtenderMark& o) const { return !(*this == o); }
};

struct ExtenderRing {
  // The ellipse's GEOMETRY diameter. A XAML/Win2D stroke straddles the
  // geometry, so the drawn outer edge is this plus one stroke: see
  // OuterDiameter(), which is the number K2 pins to the cell edge.
  double diameter = 0;
  double stroke = 0;
  bool dashed = false;
  // false when the SDK paired no colour with this address (an older SDK, or a
  // truncated list); the drawing surface falls back to its muted brush rather
  // than inventing a hue.
  bool hasColor = false;
  ExtenderRgb color{};
  std::string colorHex;
  std::string ip;

  double OuterDiameter() const { return diameter + stroke; }
  double InnerDiameter() const { return diameter - stroke; }
};

struct ExtenderRingLayout {
  // The filled dot inside the rings. Equals the cell when there are no rings,
  // so an extender-less provider is drawn exactly as it is today.
  double dotDiameter = 0;
  // Every extender on the dot, before the cap -- what the collapse is decided
  // from and what a screen reader should say.
  int totalCount = 0;
  // Outermost first: rings[0] is the ring whose outer edge is the cell edge.
  std::vector<ExtenderRing> rings;
  // The ring budget did not fit at K2's absolute numbers and was scaled down.
  bool scaled = false;

  // More extenders than drawn rings: past the third, or past what a Drop cell
  // holds. The last drawn ring is the dashed one.
  bool Collapsed() const {
    return !rings.empty() && static_cast<int>(rings.size()) < totalCount;
  }

  // The same glyph `scale` times the size: the dot, every ring and the stroke
  // together, for a surface that lays its dots out in units of its own and
  // draws them at another size (the provider globe's 600-unit space, K9), so
  // the rings shrink with the dot rather than being fitted again at the drawn
  // size. The dash pattern is in stroke widths and follows the stroke.
  ExtenderRingLayout ScaledBy(double scale) const {
    if (scale < 0) scale = 0;
    ExtenderRingLayout layout = *this;
    layout.dotDiameter *= scale;
    for (ExtenderRing& ring : layout.rings) {
      ring.diameter *= scale;
      ring.stroke *= scale;
    }
    return layout;
  }
};

// "3cdd67", "#3cdd67", "3CDD67" -> rgb. Returns false (and leaves `out`
// untouched) for anything else, including the 3- and 8-digit forms: the SDK
// emits exactly six hex digits and a surface that guessed at another length
// would be painting a colour nobody chose.
inline bool ParseExtenderColorHex(std::string_view hex, ExtenderRgb& out) {
  if (!hex.empty() && hex.front() == '#') hex.remove_prefix(1);
  if (hex.size() != 6) return false;
  std::uint32_t value = 0;
  for (char c : hex) {
    std::uint32_t digit = 0;
    if ('0' <= c && c <= '9') digit = static_cast<std::uint32_t>(c - '0');
    else if ('a' <= c && c <= 'f') digit = static_cast<std::uint32_t>(c - 'a') + 10;
    else if ('A' <= c && c <= 'F') digit = static_cast<std::uint32_t>(c - 'A') + 10;
    else return false;
    value = (value << 4) | digit;
  }
  out.r = static_cast<std::uint8_t>((value >> 16) & 0xFF);
  out.g = static_cast<std::uint8_t>((value >> 8) & 0xFF);
  out.b = static_cast<std::uint8_t>(value & 0xFF);
  return true;
}

// The SDK binds no string slice, so the grid point carries its extender
// addresses and colours as two comma-separated strings in the same order
// (K1). Splits one of them; entries are trimmed and empties dropped, so a
// trailing comma or a blank field is an empty list rather than one ghost
// entry.
inline std::vector<std::string> SplitExtenderList(std::string_view csv) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= csv.size()) {
    const std::size_t comma = csv.find(',', start);
    const std::size_t end = comma == std::string_view::npos ? csv.size() : comma;
    std::string_view item = csv.substr(start, end - start);
    while (!item.empty() && (item.front() == ' ' || item.front() == '\t' ||
                             item.front() == '\r' || item.front() == '\n')) {
      item.remove_prefix(1);
    }
    while (!item.empty() && (item.back() == ' ' || item.back() == '\t' ||
                             item.back() == '\r' || item.back() == '\n')) {
      item.remove_suffix(1);
    }
    if (!item.empty()) out.emplace_back(item);
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return out;
}

// The grid point's two lists paired back up. The ADDRESSES decide how many
// rings there are -- a colour with no address is not an extender -- and a
// missing colour leaves the mark's colorHex empty, which the layout reports as
// hasColor == false.
inline std::vector<ExtenderMark> PairExtenderMarks(std::string_view ips,
                                                   std::string_view colorHexes) {
  const std::vector<std::string> addresses = SplitExtenderList(ips);
  const std::vector<std::string> colors = SplitExtenderList(colorHexes);
  std::vector<ExtenderMark> marks;
  marks.reserve(addresses.size());
  for (std::size_t i = 0; i < addresses.size(); ++i) {
    ExtenderMark mark;
    mark.ip = addresses[i];
    if (i < colors.size()) mark.colorHex = colors[i];
    marks.push_back(std::move(mark));
  }
  return marks;
}

// The rings and the shrunken dot for a cell of `cellDiameter` px wearing
// `marks`. A cell of zero or less, or no marks at all, yields the plain dot, as
// does a Drop cell too small for even one ring.
inline ExtenderRingLayout ExtenderRingsFor(double cellDiameter,
                                           const std::vector<ExtenderMark>& marks,
                                           ExtenderRingFit fit = ExtenderRingFit::Scale) {
  ExtenderRingLayout layout;
  if (cellDiameter <= 0) return layout;
  layout.dotDiameter = cellDiameter;
  layout.totalCount = static_cast<int>(marks.size());
  if (marks.empty()) return layout;

  int drawn = layout.totalCount < kExtenderMaxRings ? layout.totalCount : kExtenderMaxRings;

  // The radial budget K2 asks for, and what the cell can actually spare while
  // still leaving a visible dot.
  double step = kExtenderRingStep;
  const double available = (cellDiameter - cellDiameter * kExtenderMinDotFraction) / 2.0;
  if (available < step * drawn) {
    if (fit == ExtenderRingFit::Drop) {
      // as many whole rings as the spare radius holds at K2's step
      drawn = static_cast<int>(available / step);
      if (drawn <= 0) return layout;
    } else {
      layout.scaled = true;
      step = available / drawn;
    }
  }
  // the last drawn ring stands in for every extender that did not get one
  const bool collapsed = drawn < layout.totalCount;
  double stroke = kExtenderRingStroke * (step / kExtenderRingStep);
  if (stroke < kExtenderMinRingStroke) stroke = kExtenderMinRingStroke;
  // A stroke floor can push the ring back over its budget on an absurdly small
  // cell; clamp the geometry rather than draw a ring wider than the cell.
  if (cellDiameter < stroke) stroke = cellDiameter;

  layout.dotDiameter = cellDiameter - 2.0 * step * drawn;
  if (layout.dotDiameter < 0) layout.dotDiameter = 0;

  layout.rings.reserve(static_cast<std::size_t>(drawn));
  for (int k = 0; k < drawn; ++k) {
    ExtenderRing ring;
    // outer edge = cell - 2*k*step; the stroke straddles the geometry
    ring.diameter = cellDiameter - 2.0 * step * k - stroke;
    if (ring.diameter < 0) ring.diameter = 0;
    ring.stroke = stroke;
    // the innermost drawn ring stands in for every extender past the third, or
    // past the rings a Drop cell holds
    ring.dashed = collapsed && k == drawn - 1;
    const ExtenderMark& mark = marks[static_cast<std::size_t>(k)];
    ring.ip = mark.ip;
    ring.colorHex = mark.colorHex;
    ring.hasColor = ParseExtenderColorHex(mark.colorHex, ring.color);
    layout.rings.push_back(std::move(ring));
  }
  return layout;
}

}  // namespace urnw
