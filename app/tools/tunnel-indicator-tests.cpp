// Executable spec for the slow-internet tunnel (App/TunnelIndicator.h,
// apple/DELAY.md §2, §3): the depth table and its interpolation, the tray's
// four icons, the geometry at the sizes the app draws, and the staggered fades
// between depths. Run against the header the app compiles, on any host with a
// C++20 compiler.
//
//   c++ -std=c++20 -I ../src/App tunnel-indicator-tests.cpp -o /tmp/tunnel-tests && /tmp/tunnel-tests
//
// tests/tunnel_indicator_test.go runs it, with negative controls that rewrite
// a copy of the header, and holds tools/make-icons' constants to the header's.
//
// SPDX-License-Identifier: MPL-2.0

#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "TunnelIndicator.h"

namespace tunnel = urnw::tunnel;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << "\n";
  }
}

bool Near(double a, double b, double tolerance = 1e-9) { return std::abs(a - b) <= tolerance; }

std::string Number(double value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.4f", value);
  return text;
}

// §2.1's table at the quantized qualities, and nothing outside 0..1.
void TestTheDepthTable() {
  const struct {
    double quality;
    int depth;
  } cases[] = {
      {0, 0}, {0.25, 1}, {0.5, 2}, {0.75, 4}, {1, 6}, {-0.25, 0}, {1.5, 6},
  };
  for (const auto& c : cases) {
    const int depth = tunnel::DepthForQuality(c.quality);
    Check(depth == c.depth, "quality " + Number(c.quality) + " draws " + std::to_string(depth) +
                                " squares, not " + std::to_string(c.depth));
  }
  Check(tunnel::DepthForQuality(std::numeric_limits<double>::quiet_NaN()) == 0,
        "a quality that is not a number draws nothing");
}

// Another quality step interpolates the table and rounds, and any quality above
// 0 draws at least one square.
void TestAnotherStepInterpolates() {
  const struct {
    double quality;
    int depth;
  } cases[] = {
      {0.01, 1}, {0.1, 1}, {0.4, 2}, {0.6, 3}, {0.7, 4}, {0.8, 4}, {0.9, 5}, {0.99, 6},
  };
  for (const auto& c : cases) {
    const int depth = tunnel::DepthForQuality(c.quality);
    Check(depth == c.depth, "quality " + Number(c.quality) + " interpolates to " +
                                std::to_string(depth) + " squares, not " +
                                std::to_string(c.depth));
  }
}

// The tray has one icon per row of the table, and a depth between rows takes
// the deeper row.
void TestTheTrayIcons() {
  const int want[] = {0, 1, 2, 3, 3, 4, 4};
  for (int depth = 0; depth <= tunnel::kMaxDepth; ++depth) {
    const int icon = tunnel::IconLevelForDepth(depth);
    Check(icon == want[depth], std::to_string(depth) + " squares take icon " +
                                   std::to_string(icon) + ", not " +
                                   std::to_string(want[depth]));
  }
  Check(tunnel::IconLevelForDepth(-1) == 0, "no squares take no icon");
  Check(tunnel::IconLevelForDepth(9) == 4, "more squares than the tunnel holds take the full one");
  for (int level = 1; level <= 4; ++level) {
    Check(tunnel::IconLevelForDepth(tunnel::kDepthByLevel[level]) == level,
          "icon " + std::to_string(level) + " draws its row's " +
              std::to_string(tunnel::kDepthByLevel[level]) + " squares");
  }
}

// The outer square: a 0.094 S stroke ("2.25 pt at 24 pt"), inset by half of
// it so the stroke's outer edge is the box.
void TestTheOuterSquareFitsTheBox() {
  for (double size : {16.0, 18.0, 20.0, 24.0, 32.0, 48.0}) {
    const tunnel::Square outer = tunnel::SquareAt(size, 0);
    const std::string where = "at " + Number(size) + ": ";
    Check(Near(outer.stroke, 0.094 * size), where + "the outer stroke is " + Number(outer.stroke));
    Check(Near(outer.x - outer.stroke / 2, 0) && Near(outer.y - outer.stroke / 2, 0),
          where + "the outer stroke starts at the box's edge");
    Check(Near(outer.x + outer.side + outer.stroke / 2, size),
          where + "the outer stroke ends at the box's edge");
    Check(Near(outer.opacity, 1), where + "the outer square is opaque");
  }
  Check(Near(tunnel::SquareAt(24, 0).stroke, 2.256), "the outer stroke is 2.256 at 24");
}

// Each nested square is the outer one scaled by 0.70^i about (0.55 S, 0.55 S),
// its stroke 0.60 of the one outside it, its opacity 1 - 0.08 i.
void TestTheNestedSquares() {
  const double size = 24;
  const double vanishing = 0.55 * size;
  const tunnel::Square outer = tunnel::SquareAt(size, 0);
  for (int i = 1; i < tunnel::kMaxDepth; ++i) {
    const tunnel::Square square = tunnel::SquareAt(size, i);
    const tunnel::Square outside = tunnel::SquareAt(size, i - 1);
    const double scale = std::pow(0.70, i);
    const std::string where = "square " + std::to_string(i) + ": ";
    Check(Near(square.x - vanishing, (outer.x - vanishing) * scale) &&
              Near(square.y - vanishing, (outer.y - vanishing) * scale),
          where + "its top left corner is the outer one's scaled about the vanishing point");
    Check(Near(square.x + square.side - vanishing, (outer.x + outer.side - vanishing) * scale),
          where + "its bottom right corner is the outer one's scaled about the vanishing point");
    Check(Near(square.stroke, outside.stroke * 0.60), where + "its stroke is 0.60 of the one outside it");
    Check(Near(square.opacity, 1 - 0.08 * i), where + "its opacity is " + Number(square.opacity));
  }
}

// The vanishing point past the centre opens the left and top gaps about 1.65
// times the right and bottom ones (§2.1), and all six squares fit without
// touching at 16, the smallest size drawn.
void TestTheGaps() {
  for (double size : {16.0, 24.0}) {
    for (int i = 1; i < tunnel::kMaxDepth; ++i) {
      const tunnel::Square outside = tunnel::SquareAt(size, i - 1);
      const tunnel::Square inside = tunnel::SquareAt(size, i);
      const double left = (inside.x - inside.stroke / 2) - (outside.x + outside.stroke / 2);
      const double right = (outside.x + outside.side - outside.stroke / 2) -
                           (inside.x + inside.side + inside.stroke / 2);
      const std::string where = "at " + Number(size) + ", squares " + std::to_string(i - 1) +
                                " and " + std::to_string(i) + ": ";
      Check(0 < left && 0 < right, where + "do not touch (gaps " + Number(left) + ", " +
                                       Number(right) + ")");
      Check(right < left, where + "the left gap is the wider");
      if (i == 1) {
        Check(1.6 < left / right && left / right < 1.7,
              where + "the left gap is " + Number(left / right) + " times the right");
      }
    }
  }
}

std::string Describe(const std::vector<tunnel::Fade>& fades) {
  std::string text;
  for (const tunnel::Fade& fade : fades) {
    if (!text.empty()) text += ", ";
    text += std::to_string(fade.index) + (fade.in ? " in" : " out") + "@" +
            std::to_string(fade.beginMillis) + "+" + std::to_string(fade.durationMillis);
  }
  return text.empty() ? "nothing" : text;
}

void CheckFades(int from, int to, const std::string& want) {
  const std::string got = Describe(tunnel::FadesFor(from, to));
  Check(got == want, std::to_string(from) + " to " + std::to_string(to) + " runs " + got +
                         ", not " + want);
}

// Deeper fades the new squares in from the outside inward, 420 ms, 110 ms
// apart; 0 to the full tunnel builds in 970 ms.
void TestDeeperFadesInFromTheOutside() {
  CheckFades(0, 6,
             "0 in@0+420, 1 in@110+420, 2 in@220+420, 3 in@330+420, 4 in@440+420, 5 in@550+420");
  CheckFades(0, 1, "0 in@0+420");
  CheckFades(2, 4, "2 in@0+420, 3 in@110+420");
  Check(tunnel::DurationFor(0, 6) == 970, "0 to 6 builds in " +
                                              std::to_string(tunnel::DurationFor(0, 6)) +
                                              " ms, not 970");
}

// Shallower fades the old squares out from the inside outward, 320 ms, 90 ms
// apart; the full tunnel clears in 770 ms.
void TestShallowerFadesOutFromTheInside() {
  CheckFades(6, 0,
             "5 out@0+320, 4 out@90+320, 3 out@180+320, 2 out@270+320, 1 out@360+320, 0 out@450+320");
  CheckFades(6, 4, "5 out@0+320, 4 out@90+320");
  CheckFades(1, 0, "0 out@0+320");
  Check(tunnel::DurationFor(6, 0) == 770, "6 to 0 clears in " +
                                              std::to_string(tunnel::DurationFor(6, 0)) +
                                              " ms, not 770");
}

// A skipped level runs the same stagger for every square that changes; no
// change runs nothing; depths outside the tunnel are clamped to it.
void TestSkippedLevelsAndNoChange() {
  CheckFades(1, 6, "1 in@0+420, 2 in@110+420, 3 in@220+420, 4 in@330+420, 5 in@440+420");
  CheckFades(4, 1, "3 out@0+320, 2 out@90+320, 1 out@180+320");
  CheckFades(4, 4, "nothing");
  CheckFades(0, 0, "nothing");
  CheckFades(-3, 9,
             "0 in@0+420, 1 in@110+420, 2 in@220+420, 3 in@330+420, 4 in@440+420, 5 in@550+420");
  Check(tunnel::DurationFor(3, 3) == 0, "no change takes no time");
}

// The pill waits for the last square to start fading before it fades itself,
// and only on a change to none.
void TestThePillWaitsForTheLastSquare() {
  Check(tunnel::ContainerHideDelayFor(6, 0) == 450, "the pill fades at 450 ms after 6 to 0");
  Check(tunnel::ContainerHideDelayFor(2, 0) == 90, "the pill fades at 90 ms after 2 to 0");
  Check(tunnel::ContainerHideDelayFor(1, 0) == 0, "the pill fades with the one square");
  Check(tunnel::ContainerHideDelayFor(6, 4) == 0, "a change to some squares keeps the pill");
  Check(tunnel::ContainerHideDelayFor(0, 6) == 0, "a change from none keeps the pill");
  Check(0 < tunnel::kSettleScale - 1 && tunnel::kSettleScale < 1.1,
        "squares settle from a little larger than their size");
}

}  // namespace

int main() {
  const struct {
    const char* name;
    void (*run)();
  } tests[] = {
      {"the depth table", TestTheDepthTable},
      {"another step interpolates", TestAnotherStepInterpolates},
      {"the tray icons", TestTheTrayIcons},
      {"the outer square fits the box", TestTheOuterSquareFitsTheBox},
      {"the nested squares", TestTheNestedSquares},
      {"the gaps", TestTheGaps},
      {"deeper fades in from the outside", TestDeeperFadesInFromTheOutside},
      {"shallower fades out from the inside", TestShallowerFadesOutFromTheInside},
      {"skipped levels and no change", TestSkippedLevelsAndNoChange},
      {"the pill waits for the last square", TestThePillWaitsForTheLastSquare},
  };
  for (const auto& test : tests) {
    const int before = gFailures;
    test.run();
    std::cout << (gFailures == before ? "ok   " : "FAIL ") << test.name << "\n";
  }
  std::cout << gCases << " checks, " << gFailures << " failed\n";
  return gFailures == 0 ? 0 : 1;
}
