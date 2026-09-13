// Executable spec for the extender UI's pure logic (connect/EXTENDER.md K2,
// K3, K4, K6, K7): the ring geometry a provider dot is drawn with
// (App/ExtenderRingGeometry.h), the status mapping, settings form, QR
// placement and import decision the panel and the sheets act on
// (App/ExtenderPresentation.h), and a build+encode of the vendored Nayuki
// encoder the share screen renders with — run against the SAME sources the app
// compiles, on any host with a C++20 compiler.
//
// The WinUI halves (ExtenderPanel, ExtenderSheets, the ConnectCanvas and
// IpFamilyHistogram drawing) cannot be built off Windows at all; what is
// verified here is every decision they make before they touch a XAML object.
//
//   c++ -std=c++20 -I ../src/App -I ../third_party/qrcodegen \
//       extender-tests.cpp ../src/App/ExtenderPresentation.cpp \
//       ../third_party/qrcodegen/qrcodegen.cpp \
//       -o /tmp/extender-tests && /tmp/extender-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <cmath>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "ExtenderPresentation.h"
#include "ExtenderRingGeometry.h"
#include "qrcodegen.hpp"

using namespace urnw;

namespace {

int gFailures = 0;
int gCases = 0;
std::string gCurrentCase;

void Fail(const std::string& message) {
  ++gFailures;
  std::cout << "  FAIL [" << gCurrentCase << "] " << message << "\n";
}

void Check(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}

void CheckNear(double expected, double actual, double tolerance, const std::string& what) {
  if (!(std::fabs(expected - actual) <= tolerance)) {
    std::ostringstream out;
    out << what << ": expected " << expected << " +/- " << tolerance << ", got " << actual;
    Fail(out.str());
  }
}

void CheckEq(const std::string& expected, const std::string& actual,
             const std::string& what) {
  if (expected != actual) {
    Fail(what + ": expected \"" + expected + "\", got \"" + actual + "\"");
  }
}

void CheckEq(long long expected, long long actual, const std::string& what) {
  if (expected != actual) {
    Fail(what + ": expected " + std::to_string(expected) + ", got " +
         std::to_string(actual));
  }
}

struct Case {
  explicit Case(const char* name) {
    gCurrentCase = name;
    ++gCases;
  }
};
#define TEST_CASE(name) Case case_##__LINE__(name)

std::vector<ExtenderMark> Marks(std::initializer_list<const char*> colorHexes) {
  std::vector<ExtenderMark> marks;
  int i = 0;
  for (const char* hex : colorHexes) {
    ExtenderMark mark;
    mark.ip = "192.0.2." + std::to_string(++i);
    mark.colorHex = hex;
    marks.push_back(std::move(mark));
  }
  return marks;
}

// ---- ExtenderRingGeometry.h: colours ---------------------------------------

void ColorTests() {
  {
    TEST_CASE("theSdkHexParses");
    ExtenderRgb rgb{};
    // the pinned values of the SDK's GetExtenderColorHex (K3)
    Check(ParseExtenderColorHex("3cdd67", rgb), "192.0.2.1 -> 3cdd67 parses");
    Check(rgb == ExtenderRgb{0x3c, 0xdd, 0x67}, "3cdd67 channels");
    Check(ParseExtenderColorHex("dd4f3c", rgb), "2001:db8::1 -> dd4f3c parses");
    Check(rgb == ExtenderRgb{0xdd, 0x4f, 0x3c}, "dd4f3c channels");
    Check(ParseExtenderColorHex("#DD4F3C", rgb), "a leading # and upper case parse");
    Check(rgb == ExtenderRgb{0xdd, 0x4f, 0x3c}, "case folds");
  }
  {
    TEST_CASE("anythingElseIsNotAColor");
    ExtenderRgb rgb{0x11, 0x22, 0x33};
    const ExtenderRgb before = rgb;
    for (const char* bad : {"", "fff", "3cdd6", "3cdd677", "3cdd6g", "ff00ff00", "  "}) {
      Check(!ParseExtenderColorHex(bad, rgb), std::string("rejects \"") + bad + "\"");
    }
    Check(rgb == before, "a rejected value leaves the output alone");
  }
}

// ---- ExtenderRingGeometry.h: the comma-separated pair ------------------------

void PairingTests() {
  {
    TEST_CASE("theTwoListsPairByPosition");
    const auto marks = PairExtenderMarks("192.0.2.1,2001:db8::1", "3cdd67,dd4f3c");
    CheckEq(2, static_cast<long long>(marks.size()), "two marks");
    CheckEq("192.0.2.1", marks[0].ip, "first ip");
    CheckEq("3cdd67", marks[0].colorHex, "first colour");
    CheckEq("2001:db8::1", marks[1].ip, "second ip");
    CheckEq("dd4f3c", marks[1].colorHex, "second colour");
  }
  {
    TEST_CASE("aDirectRouteHasNoRings");
    Check(PairExtenderMarks("", "").empty(), "empty fields -> no marks");
    Check(PairExtenderMarks(",", ",").empty(), "separators alone -> no marks");
    Check(PairExtenderMarks("  ", "  ").empty(), "whitespace alone -> no marks");
  }
  {
    TEST_CASE("theAddressListDecidesHowManyRings");
    // an SDK that sent no colours (or fewer than it sent addresses) must not
    // lose a ring, and must not gain one from a stray colour either
    auto marks = PairExtenderMarks("192.0.2.1,192.0.2.2", "3cdd67");
    CheckEq(2, static_cast<long long>(marks.size()), "two addresses, one colour");
    CheckEq("", marks[1].colorHex, "the unpaired address has no colour");
    marks = PairExtenderMarks("192.0.2.1", "3cdd67,dd4f3c");
    CheckEq(1, static_cast<long long>(marks.size()), "one address, two colours");
  }
  {
    TEST_CASE("entriesAreTrimmed");
    const auto marks = PairExtenderMarks(" 192.0.2.1 , 2001:db8::1 ", " 3cdd67 , dd4f3c ");
    CheckEq("192.0.2.1", marks[0].ip, "leading/trailing space is not part of an address");
    CheckEq("dd4f3c", marks[1].colorHex, "nor of a colour");
  }
}

// ---- ExtenderRingGeometry.h: K2's geometry ----------------------------------

void RingGeometryTests() {
  {
    TEST_CASE("noExtendersIsTheDotWeAlreadyDraw");
    const auto layout = ExtenderRingsFor(64, {});
    CheckNear(64, layout.dotDiameter, 1e-9, "the dot fills the cell");
    Check(layout.rings.empty(), "no rings");
    Check(!layout.Collapsed() && !layout.scaled, "nothing collapsed, nothing scaled");
    CheckEq(0, layout.totalCount, "no extenders");
  }
  {
    TEST_CASE("k2NumbersHoldAtAWorkableCellSize");
    // 64px: three rings need 24px of the diameter, leaving 40 - well over the
    // floor, so this is K2 verbatim
    for (int n = 1; n <= 3; ++n) {
      const auto layout =
          ExtenderRingsFor(64, Marks(n == 1   ? std::initializer_list<const char*>{"3cdd67"}
                                     : n == 2 ? std::initializer_list<const char*>{"3cdd67",
                                                                                   "dd4f3c"}
                                              : std::initializer_list<const char*>{
                                                    "3cdd67", "dd4f3c", "67dd3c"}));
      const std::string at = " (n=" + std::to_string(n) + ")";
      Check(!layout.scaled, "not scaled" + at);
      CheckEq(n, static_cast<long long>(layout.rings.size()), "one ring per extender" + at);
      // the filled dot shrinks inward by 4px per ring => 8px of diameter
      CheckNear(64 - 8.0 * n, layout.dotDiameter, 1e-9, "dot diameter" + at);
      for (int k = 0; k < n; ++k) {
        const ExtenderRing& ring = layout.rings[static_cast<std::size_t>(k)];
        CheckNear(2.0, ring.stroke, 1e-9, "stroke is 2px" + at);
        // the outermost ring's OUTER EDGE is the cell edge; each ring in is 4px
        CheckNear(64 - 8.0 * k, ring.OuterDiameter(), 1e-9,
                  "ring " + std::to_string(k) + " outer edge" + at);
      }
      // 2px of air between the dot and the first ring in, and between rings
      const ExtenderRing& innermost = layout.rings[static_cast<std::size_t>(n - 1)];
      CheckNear(2.0, (innermost.InnerDiameter() - layout.dotDiameter) / 2, 1e-9,
                "gap between the dot and the innermost ring" + at);
      for (int k = 1; k < n; ++k) {
        const double outer = layout.rings[static_cast<std::size_t>(k - 1)].InnerDiameter();
        const double inner = layout.rings[static_cast<std::size_t>(k)].OuterDiameter();
        CheckNear(2.0, (outer - inner) / 2, 1e-9,
                  "gap between rings " + std::to_string(k - 1) + " and " +
                      std::to_string(k) + at);
      }
    }
  }
  {
    TEST_CASE("theFootprintNeverGrowsIntoANeighbour");
    for (double cell : {10.0, 12.0, 18.285714, 20.0, 32.0, 64.0, 120.0}) {
      for (int n = 1; n <= 6; ++n) {
        std::vector<ExtenderMark> marks;
        for (int i = 0; i < n; ++i) marks.push_back(ExtenderMark{"ip", "3cdd67"});
        const auto layout = ExtenderRingsFor(cell, marks);
        const std::string at =
            " (cell=" + std::to_string(cell) + " n=" + std::to_string(n) + ")";
        CheckNear(cell, layout.rings.front().OuterDiameter(), 1e-9,
                  "the outermost ring sits exactly on the cell edge" + at);
        Check(0 < layout.dotDiameter, "the filled dot is still visible" + at);
        Check(layout.dotDiameter <= cell, "the dot never exceeds the cell" + at);
        Check(layout.rings.back().InnerDiameter() >= layout.dotDiameter - 1e-9,
              "the dot never touches the innermost ring" + at);
      }
    }
  }
  {
    TEST_CASE("theHeroCellScalesRatherThanLosingTheDot");
    // the connect hero's own cell: a 256pt canvas over a 14-wide grid
    const double cell = 256.0 / 14;
    const auto layout = ExtenderRingsFor(cell, Marks({"3cdd67", "dd4f3c", "67dd3c"}));
    Check(layout.scaled, "three rings do not fit at 4px each on an 18px cell");
    CheckNear(cell / 3, layout.dotDiameter, 1e-9, "the dot keeps a third of the cell");
    CheckNear(cell, layout.rings[0].OuterDiameter(), 1e-9, "outer edge still the cell edge");
    Check(0 < layout.rings[0].stroke && layout.rings[0].stroke < 2,
          "the stroke scaled with the gap rather than being dropped");
    // stroke and gap keep their 1:1 ratio through the scale
    const double gap = (layout.rings[2].InnerDiameter() - layout.dotDiameter) / 2;
    CheckNear(layout.rings[0].stroke, gap, 1e-9, "stroke and gap stay equal");
  }
  {
    TEST_CASE("oneRingStillFitsUnscaledOnAHeroCell");
    const double cell = 256.0 / 14;
    const auto layout = ExtenderRingsFor(cell, Marks({"3cdd67"}));
    Check(!layout.scaled, "a single ring costs 4px of an 18px cell, which fits");
    CheckNear(2.0, layout.rings[0].stroke, 1e-9, "K2's 2px stroke");
    CheckNear(cell - 8, layout.dotDiameter, 1e-9, "the dot shrank by 4px of radius");
  }
  {
    TEST_CASE("fourOrMoreCollapseIntoADashedThirdRing");
    const auto three = ExtenderRingsFor(64, Marks({"3cdd67", "dd4f3c", "67dd3c"}));
    Check(!three.Collapsed(), "three extenders are three plain rings");
    for (const auto& ring : three.rings) Check(!ring.dashed, "none of them is dashed");

    for (int n = 4; n <= 9; ++n) {
      std::vector<ExtenderMark> marks;
      for (int i = 0; i < n; ++i) marks.push_back(ExtenderMark{"ip", "3cdd67"});
      const auto layout = ExtenderRingsFor(64, marks);
      const std::string at = " (n=" + std::to_string(n) + ")";
      CheckEq(3, static_cast<long long>(layout.rings.size()), "still three rings" + at);
      CheckEq(n, layout.totalCount, "the total is preserved for the caller" + at);
      Check(layout.Collapsed(), "collapsed" + at);
      Check(!layout.rings[0].dashed && !layout.rings[1].dashed,
            "the outer two stay solid" + at);
      Check(layout.rings[2].dashed, "the THIRD ring is the dashed one" + at);
      CheckNear(64 - 24.0, layout.dotDiameter, 1e-9,
                "the dot shrinks for three rings, not for n" + at);
    }
  }
  {
    TEST_CASE("ringsAreOutermostFirstAndKeepTheirPairing");
    const auto layout = ExtenderRingsFor(64, Marks({"3cdd67", "dd4f3c", "67dd3c"}));
    CheckEq("192.0.2.1", layout.rings[0].ip, "the first extender is the outermost ring");
    CheckEq("3cdd67", layout.rings[0].colorHex, "with its own colour");
    CheckEq("192.0.2.3", layout.rings[2].ip, "the third is the innermost");
    for (const auto& ring : layout.rings) Check(ring.hasColor, "every colour parsed");
    Check(layout.rings[1].color == ExtenderRgb{0xdd, 0x4f, 0x3c}, "ring 1 rgb");
  }
  {
    TEST_CASE("aMissingColourIsReportedNotInvented");
    std::vector<ExtenderMark> marks{ExtenderMark{"192.0.2.1", ""},
                                    ExtenderMark{"192.0.2.2", "nope"}};
    const auto layout = ExtenderRingsFor(64, marks);
    CheckEq(2, static_cast<long long>(layout.rings.size()), "both rings are drawn");
    Check(!layout.rings[0].hasColor && !layout.rings[1].hasColor,
          "neither claims a colour it was not given");
    Check(layout.rings[0].color == ExtenderRgb{}, "and none was invented");
  }
  {
    TEST_CASE("anUnmeasuredCellDrawsNothing");
    for (double cell : {0.0, -1.0}) {
      const auto layout = ExtenderRingsFor(cell, Marks({"3cdd67"}));
      Check(layout.rings.empty(), "no rings before the canvas has a size");
      CheckNear(0, layout.dotDiameter, 1e-9, "and no dot");
    }
  }
}

// ---- ExtenderPresentation.h: the gossip dot ---------------------------------

void GossipStateTests() {
  {
    TEST_CASE("theThreeStatesMapToTheThreeColours");
    Check(ExtenderGossipToneFor("connected") == GossipTone::Green, "connected -> green");
    Check(ExtenderGossipToneFor("connecting") == GossipTone::Yellow,
          "connecting -> yellow");
    Check(ExtenderGossipToneFor("disconnected") == GossipTone::Red,
          "disconnected -> red");
    CheckEq("connected", ExtenderGossipStateKey("connected"), "connected key");
    // the store's yellow word is the gossip network's own, not the shared one
    CheckEq("gossip_connecting", ExtenderGossipStateKey("connecting"), "connecting key");
    CheckEq("disconnected", ExtenderGossipStateKey("disconnected"), "disconnected key");
  }
  {
    TEST_CASE("anUnknownStateIsNeverReassuring");
    for (const char* unknown : {"", "CONNECTED", "up", "degraded", "member"}) {
      Check(ExtenderGossipToneFor(unknown) == GossipTone::Red,
            std::string("\"") + unknown + "\" is red, not green");
      CheckEq("disconnected", ExtenderGossipStateKey(unknown),
              std::string("\"") + unknown + "\" says disconnected");
    }
  }
}

// ---- ExtenderPresentation.h: the panel's model ------------------------------

void PanelModelTests() {
  ExtenderStatusView status;
  status.gossipState = "connected";
  status.activeCount = 2;
  status.reserveCount = 7;
  status.eventCountLastMinute = 3;
  status.extenders = {
      ExtenderInfoView{"192.0.2.1", "3cdd67", 2},
      ExtenderInfoView{"198.51.100.7", "dd4f3c", 0},
      ExtenderInfoView{"2001:db8::1", "67dd3c", 1},
  };
  {
    TEST_CASE("onlyInUseExtendersGetARing");
    const auto model = ExtenderPanelModelFor(status);
    CheckEq(2, static_cast<long long>(model.activeMarks.size()), "two active extenders");
    CheckEq("192.0.2.1", model.activeMarks[0].ip, "in status order");
    CheckEq("2001:db8::1", model.activeMarks[1].ip, "the idle one is skipped");
    CheckEq("67dd3c", model.activeMarks[1].colorHex, "each ring keeps its colour");
  }
  {
    TEST_CASE("theCountIsActiveOfReserve");
    const auto model = ExtenderPanelModelFor(status);
    CheckEq(2, model.activeCount, "N = carrying a live connection now");
    CheckEq(7, model.reserveCount, "M = every usable entry");
    CheckEq(3, model.eventCountLastMinute, "the trailing-60s event count");
    Check(model.tone == GossipTone::Green, "green");
    CheckEq("connected", model.stateKey, "connected");
  }
  {
    TEST_CASE("anEmptyStatusIsARedDotAndZeroOfZero");
    const auto model = ExtenderPanelModelFor(ExtenderStatusView{});
    Check(model.tone == GossipTone::Red, "red before anything is known");
    CheckEq("disconnected", model.stateKey, "disconnected");
    CheckEq(0, model.activeCount, "0");
    CheckEq(0, model.reserveCount, "of 0");
    Check(model.activeMarks.empty(), "no rings");
  }
  {
    TEST_CASE("negativeCountsAreNeverRendered");
    ExtenderStatusView broken;
    broken.activeCount = -1;
    broken.reserveCount = -4;
    broken.eventCountLastMinute = -9;
    const auto model = ExtenderPanelModelFor(broken);
    CheckEq(0, model.activeCount, "clamped");
    CheckEq(0, model.reserveCount, "clamped");
    CheckEq(0, model.eventCountLastMinute, "clamped");
  }
  {
    TEST_CASE("anExtenderWithNoAddressIsNotARing");
    ExtenderStatusView anonymous;
    anonymous.extenders = {ExtenderInfoView{"", "3cdd67", 4}};
    Check(ExtenderPanelModelFor(anonymous).activeMarks.empty(),
          "there is nothing to identify, so there is nothing to draw");
  }
  {
    TEST_CASE("theModelComparesByValueSoAnUnchangedPushCostsNothing");
    const auto a = ExtenderPanelModelFor(status);
    const auto b = ExtenderPanelModelFor(status);
    Check(a == b, "same status, equal model");
    ExtenderStatusView moved = status;
    moved.extenders[1].inUse = 1;  // an extender started carrying traffic
    Check(a != ExtenderPanelModelFor(moved), "a new ring is a change");
    ExtenderStatusView recoloured = status;
    recoloured.extenders[0].colorHex = "ffffff";
    Check(a != ExtenderPanelModelFor(recoloured), "so is a colour");
  }
}

// ---- ExtenderPresentation.h: the settings form ------------------------------

void SettingsFormTests() {
  {
    TEST_CASE("aDefaultValueIsAPlaceholderNotAValue");
    ExtenderSettingsView settings;
    settings.dnsName = "extender.bringyour.com";
    settings.dnsNameDefault = true;
    settings.gossipUrl = "wss://gossip.bringyour.com";
    settings.gossipUrlDefault = true;
    const auto form = ExtenderSettingsFormFor(settings);
    CheckEq("", form.dnsNameText, "the box is empty, because empty MEANS the default");
    CheckEq("extender.bringyour.com", form.dnsNameDefaultValue,
            "and the default is named in the placeholder");
    CheckEq("", form.gossipUrlText, "same for the gossip url");
    CheckEq("wss://gossip.bringyour.com", form.gossipUrlDefaultValue, "same placeholder");
  }
  {
    TEST_CASE("aConfiguredValueIsShownInTheBox");
    ExtenderSettingsView settings;
    settings.dnsName = "ext.example.com";
    settings.dnsNameDefault = false;
    settings.gossipUrl = "wss://g.example.com";
    settings.gossipUrlDefault = false;
    const auto form = ExtenderSettingsFormFor(settings);
    CheckEq("ext.example.com", form.dnsNameText, "the configured name");
    CheckEq("", form.dnsNameDefaultValue,
            "no placeholder: the box is not empty, so one would never be seen, and "
            "the derived default is not in this reading");
    CheckEq("wss://g.example.com", form.gossipUrlText, "the configured url");
  }
  {
    TEST_CASE("hostsAreOnePerLine");
    ExtenderSettingsView settings;
    settings.hosts = {"ext1.example.com", "203.0.113.9", "  ", "2001:db8::5"};
    const auto form = ExtenderSettingsFormFor(settings);
    CheckEq("ext1.example.com\n203.0.113.9\n2001:db8::5", form.hostsText,
            "in order, blanks dropped");
  }
  {
    TEST_CASE("theHostsBoxRoundTrips");
    ExtenderSettingsView settings;
    settings.hosts = {"ext1.example.com", "203.0.113.9", "2001:db8::5"};
    const auto form = ExtenderSettingsFormFor(settings);
    Check(ParseExtenderHostLines(form.hostsText) == settings.hosts,
          "what the form shows is what SetSettings gets back");
  }
  {
    TEST_CASE("aPastedListIsAccepted");
    // a user with a comma-separated list in the clipboard should not have to
    // reformat it by hand
    const auto hosts = ParseExtenderHostLines("a.example.com, b.example.com\r\nc.example.com,");
    CheckEq(3, static_cast<long long>(hosts.size()), "three hosts");
    CheckEq("a.example.com", hosts[0], "first");
    CheckEq("b.example.com", hosts[1], "second, trimmed");
    CheckEq("c.example.com", hosts[2], "third, past the CRLF; the trailing comma is not a host");
  }
  {
    TEST_CASE("anEmptyBoxIsAnEmptyList");
    Check(ParseExtenderHostLines("").empty(), "nothing typed");
    Check(ParseExtenderHostLines("\n\n  \r\n").empty(), "nothing but whitespace");
  }
}

// ---- ExtenderPresentation.h: share ------------------------------------------

void ShareTests() {
  {
    TEST_CASE("theShapeCheckIsThePayloadPrefix");
    Check(LooksLikeExtenderShare("ur-ext:1:AAAA"), "a payload");
    Check(LooksLikeExtenderShare("  ur-ext:1:AAAA \n"), "pasted with its whitespace");
    Check(!LooksLikeExtenderShare("ur-ext:1:"), "the prefix alone carries nothing");
    Check(!LooksLikeExtenderShare(""), "empty");
    Check(!LooksLikeExtenderShare("https://ur.io/c/abc"), "a connect link is not a share");
    Check(!LooksLikeExtenderShare("UR-EXT:1:AAAA"), "the scheme is lower case");
  }
  {
    TEST_CASE("pastedTextIsTrimmed");
    CheckEq("ur-ext:1:AAAA", TrimShareText("\n  ur-ext:1:AAAA\t\r\n "), "trimmed");
    CheckEq("", TrimShareText("   \n"), "whitespace alone is nothing");
  }
}

void QrLayoutTests() {
  {
    TEST_CASE("theGlyphIsCentredOnWholeModules");
    // version 5 (37 modules) drawn into 296px
    const auto layout = ExtenderQrLayoutFor(37, 296);
    CheckEq(37, layout.moduleCount, "37 modules");
    CheckNear(8, layout.moduleSize, 1e-9, "8px per module");
    CheckNear(296, layout.side, 1e-9, "the code fills the box exactly");
    // the cleared run is centred, so the margins on both sides are equal
    CheckEq(layout.clearFrom, layout.moduleCount - layout.clearTo,
            "the cleared run is centred");
    Check(layout.clearFrom < layout.clearTo, "something is cleared");
    CheckNear(layout.glyphLeft, layout.glyphTop, 1e-9, "square, so both offsets agree");
    CheckNear(layout.side - layout.glyphLeft - layout.glyphSide, layout.glyphLeft, 1e-9,
              "the glyph is centred in the code");
  }
  {
    TEST_CASE("theOutlineIsClearedToo");
    const auto layout = ExtenderQrLayoutFor(37, 296);
    CheckNear(4.0, layout.outlineThickness, 1e-9, "K7's 4px outline");
    const double cleared = (layout.clearTo - layout.clearFrom) * layout.moduleSize;
    Check(layout.glyphSide + 2 * layout.outlineThickness <= cleared + 1e-9,
          "the glyph AND its outline fit inside the blanked modules");
  }
  {
    TEST_CASE("theOcclusionStaysInsideLevelHsBudget");
    // level H recovers ~30% of the codewords; a centred patch far below that
    // is what makes the glyph safe at all
    for (int modules : {21, 25, 33, 37, 45, 57, 77}) {
      const auto layout = ExtenderQrLayoutFor(modules, 320);
      const double cleared = static_cast<double>(layout.clearTo - layout.clearFrom);
      const double areaFraction = (cleared * cleared) / (modules * modules);
      Check(areaFraction < 0.15,
            "occlusion at " + std::to_string(modules) + " modules is " +
                std::to_string(areaFraction) + ", well inside level H");
      Check(0 < cleared, "and something is actually cleared");
    }
  }
  {
    TEST_CASE("nothingToDrawIsAZeroLayout");
    for (const auto& layout :
         {ExtenderQrLayoutFor(0, 296), ExtenderQrLayoutFor(37, 0), ExtenderQrLayoutFor(37, -5)}) {
      CheckEq(0, layout.moduleCount, "no modules");
      CheckNear(0, layout.side, 1e-9, "no side");
      CheckEq(0, layout.clearTo - layout.clearFrom, "nothing cleared");
    }
  }
}

// The vendored encoder itself: this is the only place on a non-Windows host
// where third_party/qrcodegen is compiled and run, so it is what catches a
// re-vendor that does not build or an API that moved under us.
void QrEncoderTests() {
  {
    TEST_CASE("theShareTextEncodesAtLevelH");
    const std::string payload =
        "ur-ext:1:AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4vMDEyMzQ1Njc4OTo7PD0-P0A";
    const qrcodegen::QrCode code =
        qrcodegen::QrCode::encodeText(payload.c_str(), qrcodegen::QrCode::Ecc::HIGH);
    Check(0 < code.getSize(), "the encoder produced a code");
    Check(code.getSize() % 2 == 1, "every QR version has an odd module count");
    Check(code.getErrorCorrectionLevel() == qrcodegen::QrCode::Ecc::HIGH,
          "at the level K7 asks for");
    // the three finder patterns are where a decoder expects them
    Check(code.getModule(0, 0) && code.getModule(6, 0) && code.getModule(0, 6),
          "top-left finder");
    Check(code.getModule(code.getSize() - 1, 0), "top-right finder");
    Check(code.getModule(0, code.getSize() - 1), "bottom-left finder");
    Check(!code.getModule(-1, 0) && !code.getModule(code.getSize(), 0),
          "out of range reads as light, so the quiet zone needs no special case");

    // and the layout the sheet draws it with lands on that size
    const auto layout = ExtenderQrLayoutFor(code.getSize(), 296);
    CheckEq(code.getSize(), layout.moduleCount, "the layout follows the encoder");
    Check(0 < layout.moduleSize, "with a drawable module size");
  }
}

// ---- ExtenderPresentation.h: import -----------------------------------------

ExtenderShareDecodeView GoodDecode() {
  ExtenderShareDecodeView decoded;
  decoded.ok = true;
  decoded.networkHost = "bringyour.com";
  decoded.count = 12;
  return decoded;
}

void ImportTests() {
  {
    TEST_CASE("aPlainPayloadImports");
    const auto decision = DecideExtenderImport(GoodDecode(), false);
    Check(decision.canImport, "importable");
    Check(!decision.showSettingsToggle, "no settings block, no toggle");
    Check(!decision.showForeignHost, "our own operator");
    Check(!decision.needsConfirm, "nothing to confirm");
    CheckEq("", decision.messageKey, "and nothing to say");
  }
  {
    TEST_CASE("aSettingsBlockOffersTheToggleWithoutForcingIt");
    auto decoded = GoodDecode();
    decoded.hasSettings = true;
    decoded.settingsHost = "bringyour.com";
    const auto off = DecideExtenderImport(decoded, false);
    Check(off.showSettingsToggle, "the toggle is offered");
    Check(off.canImport && !off.needsConfirm,
          "importing the addresses alone takes no confirmation");
    const auto on = DecideExtenderImport(decoded, true);
    Check(on.canImport, "still importable");
    Check(on.needsConfirm, "replacing the operator settings does take one");
    CheckEq("bringyour.com", on.confirmArg,
            "and the confirmation names the operator host it would switch to");
  }
  {
    TEST_CASE("aForeignCodeIsRefusedUntilItsSettingsAreTaken");
    auto decoded = GoodDecode();
    decoded.foreignHost = true;
    decoded.networkHost = "other.example";
    // no settings block at all: nothing can rescue it
    const auto bare = DecideExtenderImport(decoded, true);
    Check(!bare.canImport, "a foreign code with no settings can never be imported");
    Check(bare.showForeignHost, "and says so");
    CheckEq("import_extenders_foreign_host", bare.messageKey, "with the store's line");
    CheckEq("other.example", bare.messageArg, "naming the other network");

    decoded.hasSettings = true;
    decoded.settingsHost = "other.example";
    const auto refused = DecideExtenderImport(decoded, false);
    Check(!refused.canImport, "refused while the toggle is off");
    Check(refused.showSettingsToggle, "but the way through is offered");
    const auto accepted = DecideExtenderImport(decoded, true);
    Check(accepted.canImport, "taking the settings is what lets it through");
    Check(accepted.needsConfirm, "and it is confirmed first");
    Check(accepted.showForeignHost, "the foreign host stays on screen throughout");
    CheckEq("other.example", accepted.confirmArg, "the confirmation names it");
  }
  {
    TEST_CASE("aFailedDecodeReportsTheSdksOwnKey");
    ExtenderShareDecodeView bad;
    bad.ok = false;
    bad.error = "import_extenders_invalid";
    const auto decision = DecideExtenderImport(bad, false);
    Check(!decision.canImport, "nothing to import");
    CheckEq("import_extenders_invalid", decision.messageKey, "the SDK's key, unchanged");
    Check(!decision.showSettingsToggle, "and no toggle for a payload that did not parse");
  }
  {
    TEST_CASE("aFailedDecodeWithNoReasonStillSaysSomething");
    ExtenderShareDecodeView bad;
    bad.ok = false;
    const auto decision = DecideExtenderImport(bad, false);
    CheckEq("import_extenders_invalid", decision.messageKey,
            "silence is not a state a user can act on");
  }
  {
    TEST_CASE("aForeignHostRejectedAtDecodeTimeStillNamesTheHost");
    ExtenderShareDecodeView bad;
    bad.ok = false;
    bad.error = "import_extenders_foreign_host";
    bad.networkHost = "other.example";
    const auto decision = DecideExtenderImport(bad, false);
    Check(decision.showForeignHost, "shown as a foreign host, not as a parse failure");
    CheckEq("other.example", decision.messageArg, "named");
  }
  {
    TEST_CASE("theOutcomeIsAPluralOnSuccessAndAKeyOnFailure");
    ExtenderImportResultView ok;
    ok.ok = true;
    ok.importedCount = 7;
    const auto good = ExtenderImportOutcomeFor(ok);
    Check(good.ok && good.isPlural, "a plural line");
    CheckEq("import_extenders_imported", good.messageKey, "the store's key");
    CheckEq(7, good.count, "with the count the SDK reported");

    ExtenderImportResultView failed;
    failed.error = "import_extenders_foreign_host";
    const auto bad = ExtenderImportOutcomeFor(failed);
    Check(!bad.ok && !bad.isPlural, "not a plural");
    CheckEq("import_extenders_foreign_host", bad.messageKey, "the SDK's key");

    ExtenderImportResultView zero;
    zero.ok = true;
    const auto none = ExtenderImportOutcomeFor(zero);
    Check(none.ok, "importing nothing is still a success");
    CheckEq(0, none.count, "\"Imported 0 extenders\" is a real answer");
  }
}

}  // namespace

int main() {
  std::cout << "ExtenderRingGeometry colours\n";
  ColorTests();
  std::cout << "ExtenderRingGeometry pairing\n";
  PairingTests();
  std::cout << "ExtenderRingGeometry K2\n";
  RingGeometryTests();
  std::cout << "gossip state\n";
  GossipStateTests();
  std::cout << "extender panel model\n";
  PanelModelTests();
  std::cout << "extender settings form\n";
  SettingsFormTests();
  std::cout << "share payload\n";
  ShareTests();
  std::cout << "share QR layout\n";
  QrLayoutTests();
  std::cout << "vendored qrcodegen\n";
  QrEncoderTests();
  std::cout << "import decisions\n";
  ImportTests();

  std::cout << "\n" << gCases << " cases, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
