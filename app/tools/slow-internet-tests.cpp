// Executable spec for what the app says about a slow internet connection
// (App/SlowInternet.h): the numbers' formats, what the connect page's pill
// says and when it says "Internet back to normal", the dwell the tray follows
// so it does not flap, the tray's decision for the slow states, and one balloon
// per slow episode. Run against the header the app compiles, on any host with a
// C++20 compiler.
//
//   c++ -std=c++20 -I ../src/App slow-internet-tests.cpp -o /tmp/slow-tests && /tmp/slow-tests
//
// tests/slow_internet_test.go runs it, with negative controls that rewrite a
// copy of the header, and pins SdkHost's, AppController's, TrayIcon's and the
// connect page's use of it.
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "SlowInternet.h"

namespace slow = urnw::slow;
using slow::Phase;
using slow::Tip;

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

std::string Narrow(const std::wstring& text) {
  std::string out;
  for (wchar_t c : text) {
    switch (c) {
      case L'↑': out += "^"; break;
      case L'↓': out += "v"; break;
      case L'·': out += "*"; break;
      default: out += static_cast<char>(c); break;
    }
  }
  return out;
}

const char* Name(Phase phase) {
  switch (phase) {
    case Phase::Hidden: return "hidden";
    case Phase::Slow: return "slow";
    case Phase::Recovered: return "recovered";
  }
  return "?";
}

const char* Name(Tip tip) {
  switch (tip) {
    case Tip::Status: return "status";
    case Tip::Slow: return "slow";
    case Tip::Recovered: return "recovered";
  }
  return "?";
}

// "640 ms" under a second, "1.2 s" under ten, "12 s" from there; nothing for 0.
void TestTheDelayFormat() {
  const struct {
    int64_t millis;
    const char* text;
  } cases[] = {
      {0, ""},          {-5, ""},           {1, "1 ms"},       {640, "640 ms"},
      {999, "999 ms"},  {1000, "1.0 s"},    {1200, "1.2 s"},   {1249, "1.2 s"},
      {1250, "1.3 s"},  {9949, "9.9 s"},    {9950, "10 s"},    {9999, "10 s"},
      {10000, "10 s"},  {12000, "12 s"},    {12499, "12 s"},   {12500, "13 s"},
      {125000, "125 s"},
  };
  for (const auto& c : cases) {
    const std::string text = slow::FormatDelay(c.millis);
    Check(text == c.text, std::to_string(c.millis) + " ms reads \"" + text + "\", not \"" +
                              c.text + "\"");
  }
}

// "48 kb/s" under 1,000,000 bit/s, at least 1, then "1.4 Mb/s"; nothing for 0.
void TestTheRateFormat() {
  const struct {
    int64_t bitsPerSecond;
    const char* text;
  } cases[] = {
      {0, ""},
      {-1, ""},
      {1, "1 kb/s"},
      {499, "1 kb/s"},
      {48000, "48 kb/s"},
      {48499, "48 kb/s"},
      {48500, "49 kb/s"},
      {999499, "999 kb/s"},
      {999500, "1.0 Mb/s"},
      {1000000, "1.0 Mb/s"},
      {1400000, "1.4 Mb/s"},
      {1449999, "1.4 Mb/s"},
      {1450000, "1.5 Mb/s"},
      {12345678, "12.3 Mb/s"},
      {123456789, "123.5 Mb/s"},
  };
  for (const auto& c : cases) {
    const std::string text = slow::FormatRate(c.bitsPerSecond);
    Check(text == c.text, std::to_string(c.bitsPerSecond) + " bit/s reads \"" + text +
                              "\", not \"" + c.text + "\"");
  }
}

slow::Reading Delays(int64_t up, int64_t down) {
  slow::Reading reading;
  reading.upQueueDelayMillis = up;
  reading.downQueueDelayMillis = down;
  return reading;
}

// Each direction's delay, or its rate when only the rate was limited, upload
// first, and nothing for a direction with neither.
void TestTheSummaryNumbers() {
  Check(Narrow(slow::SummaryNumbers(Delays(640, 1200))) == "^ 640 ms * v 1.2 s",
        "both delays: " + Narrow(slow::SummaryNumbers(Delays(640, 1200))));
  slow::Reading rate = Delays(0, 1200);
  rate.upBitsPerSecond = 48000;
  rate.downBitsPerSecond = 1400000;
  Check(Narrow(slow::SummaryNumbers(rate)) == "^ 48 kb/s * v 1.2 s",
        "a rate stands in for a direction with no delay: " + Narrow(slow::SummaryNumbers(rate)));
  Check(Narrow(slow::SummaryNumbers(Delays(0, 12000))) == "v 12 s",
        "only download: " + Narrow(slow::SummaryNumbers(Delays(0, 12000))));
  Check(slow::SummaryNumbers(slow::Reading{}).empty(), "no numbers, no summary");
}

// The worse direction's quality is what the indicator draws.
void TestTheWorseDirectionDraws() {
  slow::Reading reading;
  reading.upQuality = 0.25;
  reading.downQuality = 0.75;
  Check(slow::DepthOf(reading) == 4, "down at .75 over up at .25 draws 4 squares");
  reading.upQuality = 1;
  Check(slow::DepthOf(reading) == 6, "up at 1 draws the full tunnel");
  Check(slow::DepthOf(slow::Reading{}) == 0, "a normal reading draws nothing");
}

// The tooltip's words take the numbers in parentheses while they fit the
// shell's 127 characters, and drop them before the words are cut.
void TestTheSlowStatusFits() {
  const slow::Reading reading = Delays(640, 1200);
  Check(Narrow(slow::SlowStatus(L"Slow internet", reading, 12)) ==
            "Slow internet (^ 640 ms * v 1.2 s)",
        "the numbers ride in parentheses");
  Check(Narrow(slow::SlowStatus(L"Slow internet", slow::Reading{}, 12)) == "Slow internet",
        "no numbers, no parentheses");
  const std::wstring words(100, L'w');
  Check(slow::SlowStatus(words, reading, 12) == words,
        "the numbers go first when the words leave them no room");
  // " (" + the numbers + ")"
  const std::size_t suffix = slow::SummaryNumbers(reading).size() + 3;
  const std::wstring fits(127 - 12 - suffix, L'w');
  Check(slow::SlowStatus(fits, reading, 12).size() == 127 - 12,
        "the numbers stay while the whole tooltip is 127 characters");
  const std::wstring over(127 - 12 - suffix + 1, L'w');
  Check(slow::SlowStatus(over, reading, 12) == over,
        "one character more and the numbers go");
}

// ---- the pill (apple's SlowInternetPillContentTests) ------------------------

using Content = slow::PillContent;
using slow::Direction;

// up at .25 and down at .75, both limited, both delayed: the larger delay down
slow::Reading Slow() {
  slow::Reading reading;
  reading.upQuality = 0.25;
  reading.downQuality = 0.75;
  reading.upBitsPerSecond = 48'000;
  reading.downBitsPerSecond = 85'000;
  reading.upQueueDelayMillis = 800;
  reading.downQueueDelayMillis = 1900;
  return reading;
}

// no standing delay either way, both directions limited
slow::Reading RatesOnly() {
  slow::Reading reading;
  reading.downQuality = 0.75;
  reading.upBitsPerSecond = 150'000;
  reading.downBitsPerSecond = 280'000;
  return reading;
}

std::string Describe(const std::optional<Direction>& worse) {
  if (!worse) return "neither";
  return *worse == Direction::Up ? "up" : "down";
}

std::string Describe(const Content& content) {
  std::string text = content.label == Content::Label::Fallback ? "Fallback" : "Poor signal";
  text += " ^ \"" + content.up + "\" v \"" + content.down + "\", worse " + Describe(content.worse);
  if (content.recovered) text += ", recovered";
  return text;
}

std::string Describe(const std::optional<Content>& content) {
  return content ? Describe(*content) : "nothing";
}

void ExpectContent(const std::optional<Content>& got, const std::optional<Content>& want,
                   const std::string& what) {
  Check(got == want, what + ": " + Describe(got) + ", not " + Describe(want));
}

// "Poor signal", or "Fallback" while the worst reading came from the fallback.
void TestThePillsLabel() {
  Check(slow::PillContentFor(Slow()).label == Content::Label::PoorSignal, "a slow reading says Poor signal");
  slow::Reading fallback = Slow();
  fallback.fallback = true;
  Check(slow::PillContentFor(fallback).label == Content::Label::Fallback, "the fallback says Fallback");
}

// Each direction shows its standing delay, the larger one the worse; equal
// delays mark neither.
void TestEachDirectionShowsItsDelay() {
  const Content content = slow::PillContentFor(Slow());
  Check(content.up == "800 ms" && content.down == "1.9 s", "the delays: " + Describe(content));
  Check(content.worse == Direction::Down, "the larger delay is the worse: " + Describe(content));
  Check(!content.recovered, "a slow reading is no recovery");

  slow::Reading upWorse = Slow();
  upWorse.upQueueDelayMillis = 2900;
  Check(slow::PillContentFor(upWorse).worse == Direction::Up, "the upload's larger delay is the worse");

  slow::Reading equal = Slow();
  equal.upQueueDelayMillis = 1900;
  Check(!slow::PillContentFor(equal).worse, "equal delays mark neither");
}

// No standing delay but a limited rate: the rate shows instead, and a rate is
// never the worse, which goes by delay.
void TestADirectionWithNoDelayShowsItsLimitedRate() {
  slow::Reading reading;
  reading.upQuality = 0.5;
  reading.upBitsPerSecond = 150'000;
  reading.downBitsPerSecond = 280'000;
  reading.downQueueDelayMillis = 620;
  const Content mixed = slow::PillContentFor(reading);
  Check(mixed.up == "150 kb/s" && mixed.down == "620 ms", "a rate stands in: " + Describe(mixed));
  Check(mixed.worse == Direction::Down, "the delay is the worse: " + Describe(mixed));

  const Content rates = slow::PillContentFor(RatesOnly());
  Check(rates.up == "150 kb/s" && rates.down == "280 kb/s", "both rates: " + Describe(rates));
  Check(!rates.worse, "rates alone mark neither: " + Describe(rates));
}

// Neither a delay nor a rate: the direction hides; with both hidden the pill
// is the tunnel and the label.
void TestADirectionWithNeitherHides() {
  slow::Reading downOnly;
  downOnly.downQuality = 0.25;
  downOnly.downQueueDelayMillis = 640;
  const Content down = slow::PillContentFor(downOnly);
  Check(down.up.empty() && down.down == "640 ms" && down.worse == Direction::Down,
        "the download alone: " + Describe(down));

  slow::Reading neither;
  neither.upQuality = 0.5;
  const Content none = slow::PillContentFor(neither);
  Check(none.up.empty() && none.down.empty() && !none.worse && none.label == Content::Label::PoorSignal,
        "no numbers, the label alone: " + Describe(none));
}

// With too little room the better direction goes first, then the numbers.
void TestTheBetterDirectionGoesFirstThenTheNumbers() {
  const Content content = slow::PillContentFor(Slow());
  const Content one = content.WorseOnly();
  Check(one.up.empty() && one.down == "1.9 s" && one.label == Content::Label::PoorSignal,
        "the worse download stays: " + Describe(one));
  const Content none = content.LabelOnly();
  Check(none.up.empty() && none.down.empty() && none.label == Content::Label::PoorSignal,
        "then the label alone: " + Describe(none));

  slow::Reading upWorse = Slow();
  upWorse.upQueueDelayMillis = 2900;
  const Content up = slow::PillContentFor(upWorse).WorseOnly();
  Check(up.up == "2.9 s" && up.down.empty(), "the worse upload stays: " + Describe(up));

  // neither worse: the download stays, or the upload when only it shows
  const Content rates = slow::PillContentFor(RatesOnly()).WorseOnly();
  Check(rates.up.empty() && rates.down == "280 kb/s", "neither worse keeps the download: " + Describe(rates));
  slow::Reading upRate;
  upRate.upQuality = 0.5;
  upRate.upBitsPerSecond = 150'000;
  const Content upOnly = slow::PillContentFor(upRate);
  Check(upOnly.WorseOnly() == upOnly, "the upload alone stays: " + Describe(upOnly.WorseOnly()));
}

// The richest form that fits the room, ViewThatFits' order: the whole content,
// the worse direction alone, then the label alone whatever the room.
void TestThePillFitsItsRoom() {
  // the tunnel, the label and the chrome 100; each direction 8 more for the
  // space before it
  const auto width = [](const Content& content) {
    return 100.0 + (content.up.empty() ? 0 : 8 + 60) + (content.down.empty() ? 0 : 8 + 70);
  };
  const Content content = slow::PillContentFor(Slow());
  ExpectContent(slow::FitPill(content, 1000, width), content, "room to spare shows it all");
  ExpectContent(slow::FitPill(content, 246, width), content, "exactly enough room shows it all");
  ExpectContent(slow::FitPill(content, 245.5, width), content.WorseOnly(), "a little less drops the upload");
  ExpectContent(slow::FitPill(content, 178, width), content.WorseOnly(), "the download alone fits");
  ExpectContent(slow::FitPill(content, 177.5, width), content.LabelOnly(), "then the label alone");
  ExpectContent(slow::FitPill(content, 0, width), content.LabelOnly(), "the label alone with no room");

  slow::Reading upWorse = Slow();
  upWorse.upQueueDelayMillis = 2900;
  const Content up = slow::PillContentFor(upWorse);
  ExpectContent(slow::FitPill(up, 200, width), up.WorseOnly(), "the worse upload keeps its place");
}

// The store's state with `reading`, available.
std::optional<slow::Reading> Live(const slow::Reading& reading) { return reading; }

// The reading returns to normal while the pill shows it: "Internet back to
// normal" with no numbers, which then holds.
void TestAReturnToNormalShowsTheRecoveredState() {
  const std::optional<Content> slowShown = slow::NextPillContent(std::nullopt, Live(Slow()), false);
  ExpectContent(slowShown, slow::PillContentFor(Slow()), "a slow reading shows the slow content");

  const std::optional<Content> recovered = slow::NextPillContent(slowShown, Live(slow::Reading{}), false);
  Check(recovered && recovered->recovered && recovered->up.empty() && recovered->down.empty() &&
            !recovered->worse,
        "back to normal shows the recovered state: " + Describe(recovered));

  // later normal readings, numbers under the thresholds included, hold it
  ExpectContent(slow::NextPillContent(recovered, Live(slow::Reading{}), false), recovered,
                "a normal reading holds it");
  slow::Reading quiet;
  quiet.upQueueDelayMillis = 120;
  quiet.downQueueDelayMillis = 90;
  ExpectContent(slow::NextPillContent(recovered, Live(quiet), false), recovered,
                "numbers under the thresholds hold it");
  ExpectContent(slow::NextPillContent(slowShown, Live(quiet), false), recovered,
                "numbers under the thresholds are a return to normal");
}

// A new slow level during the hold ends it: the slow content returns.
void TestASlowLevelDuringTheHoldReturnsToTheSlowContent() {
  const std::optional<Content> recovered =
      slow::NextPillContent(slow::PillContentFor(Slow()), Live(slow::Reading{}), false);
  slow::Reading again = Slow();
  again.downQueueDelayMillis = 2400;
  ExpectContent(slow::NextPillContent(recovered, Live(again), false), slow::PillContentFor(again),
                "slow again during the hold");
}

// The reading going away (no session, the remote down) or the can't-reach
// banner taking the top is no recovery: the pill just leaves.
void TestAReadingThatGoesAwayIsNoRecovery() {
  const Content slowShown = slow::PillContentFor(Slow());
  ExpectContent(slow::NextPillContent(slowShown, std::nullopt, false), std::nullopt,
                "the reading going away");
  ExpectContent(slow::NextPillContent(slowShown, Live(Slow()), true), std::nullopt,
                "the banner over a slow reading");
  ExpectContent(slow::NextPillContent(slowShown, Live(slow::Reading{}), true), std::nullopt,
                "the banner over a normal reading");

  // the banner during the hold ends it at once
  const std::optional<Content> recovered = slow::NextPillContent(slowShown, Live(slow::Reading{}), false);
  Check(recovered && recovered->recovered, "recovered before the banner: " + Describe(recovered));
  ExpectContent(slow::NextPillContent(recovered, Live(slow::Reading{}), true), std::nullopt,
                "the banner during the hold");
  ExpectContent(slow::NextPillContent(recovered, std::nullopt, false), std::nullopt,
                "the reading going away during the hold");
}

// Nothing shown, a normal reading shows nothing: no recovered state without a
// slow spell on screen (the pill appearing, or one that left).
void TestNormalWithNothingShownShowsNothing() {
  ExpectContent(slow::NextPillContent(std::nullopt, Live(slow::Reading{}), false), std::nullopt,
                "a normal reading with nothing shown");
  ExpectContent(slow::NextPillContent(std::nullopt, std::nullopt, false), std::nullopt,
                "no reading with nothing shown");
}

// The numbers are read again while the slow content shows on a window that
// presents, and not while the pill says "Internet back to normal".
void TestThePillRefreshesWhileItShowsTheNumbers() {
  const std::optional<Content> slowShown = slow::PillContentFor(Slow());
  const std::optional<Content> recovered =
      slow::NextPillContent(slowShown, Live(slow::Reading{}), false);
  Check(slow::PillRefreshes(slowShown, true), "the slow content on screen refreshes");
  Check(!slow::PillRefreshes(slowShown, false), "the slow content off screen does not");
  Check(!slow::PillRefreshes(recovered, true), "the recovered state does not");
  Check(!slow::PillRefreshes(std::nullopt, true), "no pill does not");
  Check(slow::kPillRefreshMillis == 10'000, "every 10 s, not " + std::to_string(slow::kPillRefreshMillis));
}

// One automation name: the words, then each direction that shows, after its
// own words; "Internet back to normal" alone while recovered.
void TestThePillReadsAsOneName() {
  const Content content = slow::PillContentFor(Slow());
  const std::wstring name = slow::PillName(L"Poor signal", content, L"Upload delay", L"Download delay");
  Check(name == L"Poor signal, Upload delay 800 ms, Download delay 1.9 s", "both directions: " + Narrow(name));

  slow::Reading downOnly;
  downOnly.downQuality = 0.25;
  downOnly.downQueueDelayMillis = 640;
  const std::wstring down =
      slow::PillName(L"Poor signal", slow::PillContentFor(downOnly), L"Upload delay", L"Download delay");
  Check(down == L"Poor signal, Download delay 640 ms", "the download alone: " + Narrow(down));

  const std::optional<Content> recovered =
      slow::NextPillContent(content, Live(slow::Reading{}), false);
  const std::wstring back =
      slow::PillName(L"Internet back to normal", *recovered, L"Upload delay", L"Download delay");
  Check(back == L"Internet back to normal", "recovered: " + Narrow(back));
}

std::string Describe(const slow::Presentation& p) {
  std::string text = std::string(Name(p.phase));
  if (p.phase == Phase::Slow) text += " " + std::to_string(p.depth);
  text += p.nextMillis < 0 ? ", no timer" : ", next " + std::to_string(p.nextMillis);
  return text;
}

void Expect(const slow::Presentation& got, Phase phase, int depth, int64_t next,
            const std::string& what) {
  slow::Presentation want;
  want.phase = phase;
  want.depth = phase == Phase::Slow ? depth : 0;
  want.nextMillis = next;
  Check(got == want, what + ": " + Describe(got) + ", not " + Describe(want));
}

// The level above 0 for 30 s of the last 60 s shows the slow state, and not a
// millisecond before; the timer asks again exactly when it can.
void TestASteadySlowLevelAppearsAfterThirtySeconds() {
  slow::Dwell dwell;
  Expect(dwell.Observe(4, 0), Phase::Hidden, 0, 30000, "slow from 0");
  Expect(dwell.Observe(4, 29999), Phase::Hidden, 0, 30000, "29.999 s slow");
  Expect(dwell.Observe(4, 30000), Phase::Slow, 4, -1, "30 s slow");
}

// Short slow stretches never add up to 30 s of a minute: nothing shows.
void TestBlipsNeverShow() {
  slow::Dwell dwell;
  for (int64_t t = 0; t < 600'000; t += 30'000) {
    // slow for the first 10 s of every 30 s
    Check(dwell.Observe(2, t).phase == Phase::Hidden, "a blip at " + std::to_string(t) + " shows");
    if (const auto p = dwell.Observe(2, t + 9'000); p.phase != Phase::Hidden) {
      Check(false, "a blip shows at " + std::to_string(t + 9'000));
    }
    Check(dwell.Observe(0, t + 10'000).phase == Phase::Hidden,
          "the end of a blip at " + std::to_string(t + 10'000) + " shows");
  }
}

// Old slow time ages out of the window: 20 s slow, 50 s normal, then slow
// again shows only once the new stretch makes 30 s on its own.
void TestTheHistoryAgesOut() {
  slow::Dwell dwell;
  dwell.Observe(3, 0);
  dwell.Observe(0, 20'000);
  Expect(dwell.Observe(3, 70'000), Phase::Hidden, 0, 90'000, "slow again at 70 s with 10 s left in the window");
  Expect(dwell.Observe(3, 90'000), Phase::Hidden, 0, 100'000, "at 90 s the old stretch has aged out");
  Expect(dwell.Observe(3, 100'000), Phase::Slow, 3, -1, "at 100 s the new stretch is 30 s");
}

// Once shown, the slow state stays 60 s, then reads "back to normal" for 3 s,
// then is gone. Back to 0 within the stay keeps the last depth.
void TestTheStayAndTheRecoveredState() {
  slow::Dwell dwell;
  dwell.Observe(2, 0);
  Expect(dwell.Observe(2, 30'000), Phase::Slow, 2, -1, "shown at 30 s");
  Expect(dwell.Observe(4, 40'000), Phase::Slow, 4, -1, "the depth follows the reading");
  Expect(dwell.Observe(1, 50'000), Phase::Slow, 1, -1, "shallower too");
  Expect(dwell.Observe(0, 55'000), Phase::Slow, 1, 90'000, "back to 0 within the stay keeps the last depth");
  Expect(dwell.Observe(0, 89'999), Phase::Slow, 1, 90'000, "still within the stay");
  Expect(dwell.Observe(0, 90'000), Phase::Recovered, 0, 93'000, "the stay is over: back to normal");
  Expect(dwell.Observe(0, 92'000), Phase::Recovered, 0, 93'000, "for 3 s");
  Expect(dwell.Observe(0, 93'000), Phase::Hidden, 0, -1, "then gone");
}

// A level still above 0 when the stay ends keeps the slow state until it
// returns to 0, which then recovers at once.
void TestASlowLevelOutlastsTheStay() {
  slow::Dwell dwell;
  dwell.Observe(6, 0);
  dwell.Observe(6, 30'000);
  Expect(dwell.Observe(6, 120'000), Phase::Slow, 6, -1, "still slow after the stay");
  Expect(dwell.Observe(0, 130'000), Phase::Recovered, 0, 133'000, "back to 0 after the stay recovers at once");
}

// Slow again while it reads "back to normal": a minute that is still mostly
// slow returns to the slow state, the same episode; one that is not lets the
// recovered state run out.
void TestSlowAgainDuringTheRecoveredState() {
  slow::Dwell continuing;
  continuing.Observe(4, 0);
  continuing.Observe(4, 30'000);
  Expect(continuing.Observe(0, 100'000), Phase::Recovered, 0, 103'000, "recovered at 100 s");
  Expect(continuing.Observe(4, 101'000), Phase::Slow, 4, -1, "59 s of the last minute slow: slow again");

  slow::Dwell lapsing;
  lapsing.Observe(4, 0);
  lapsing.Observe(4, 30'000);
  lapsing.Observe(0, 35'000);
  Expect(lapsing.Observe(0, 90'000), Phase::Recovered, 0, 93'000, "recovered at 90 s");
  Expect(lapsing.Observe(4, 91'000), Phase::Recovered, 0, 93'000, "4 s of the last minute slow: still recovering");
  Expect(lapsing.Observe(4, 93'000), Phase::Hidden, 0, 119'000, "gone, and the appear rule starts from the history");
}

// Unavailable hides at once and forgets the history.
void TestResetForgetsTheHistory() {
  slow::Dwell dwell;
  dwell.Observe(4, 0);
  dwell.Observe(4, 30'000);
  dwell.Reset();
  Check(dwell.phase() == Phase::Hidden, "a reset hides the slow state");
  Expect(dwell.Observe(4, 31'000), Phase::Hidden, 0, 61'000, "after a reset the 30 s start again");
}

// The tray takes the slow state only over Connected, draws the tunnel icon of
// the depth's row, and posts the balloon at the episode's first slow moment
// when the setting is on.
void TestTheTrayDecision() {
  const Phase phases[] = {Phase::Hidden, Phase::Slow, Phase::Recovered};
  for (Phase phase : phases) {
    for (int depth = 0; depth <= 6; ++depth) {
      for (bool connected : {false, true}) {
        for (bool notify : {false, true}) {
          for (bool notified : {false, true}) {
            slow::TrayFacts facts;
            facts.presentation.phase = phase;
            facts.presentation.depth = depth;
            facts.connected = connected;
            facts.notify = notify;
            facts.notified = notified;
            const slow::TrayView view = slow::DecideTray(facts);
            const std::string where = std::string(Name(phase)) + " " + std::to_string(depth) +
                                      (connected ? ", connected" : ", not connected") +
                                      (notify ? ", notify" : ", quiet") +
                                      (notified ? ", notified" : "") + ": ";
            const bool slowShows = connected && phase == Phase::Slow;
            Check(view.icon == (slowShows ? urnw::tunnel::IconLevelForDepth(depth) : 0),
                  where + "icon " + std::to_string(view.icon));
            const Tip tip = !connected                 ? Tip::Status
                            : phase == Phase::Slow      ? Tip::Slow
                            : phase == Phase::Recovered ? Tip::Recovered
                                                        : Tip::Status;
            Check(view.tip == tip, where + "the tooltip says " + Name(view.tip) + ", not " +
                                       Name(tip));
            Check(view.balloonDue == (slowShows && !notified), where + "the balloon moment");
            Check(view.postBalloon == (slowShows && !notified && notify),
                  where + (view.postBalloon ? "posts the balloon" : "posts nothing"));
          }
        }
      }
    }
  }
}

// What the tray did over a run of readings, observed as AppController does:
// at each reading and at each timer the presentation asks for.
struct Run {
  int balloons = 0;
  int balloonsDue = 0;
  bool everSlow = false;
  bool everRecovered = false;
  Phase last = Phase::Hidden;
};

struct Step {
  int64_t millis;
  // nullopt: unavailable
  std::optional<int> depth;
};

Run Simulate(const std::vector<Step>& steps, int64_t endMillis, bool connected = true,
             bool notify = true) {
  slow::TrayEpisode episode;
  Run run;
  auto settle = [&](const slow::Presentation&) {
    const slow::TrayView view = episode.Decide(connected, notify);
    if (view.balloonDue) ++run.balloonsDue;
    if (view.postBalloon) ++run.balloons;
    if (view.tip == Tip::Slow) run.everSlow = true;
    if (view.tip == Tip::Recovered) run.everRecovered = true;
    run.last = episode.presentation().phase;
  };
  std::optional<int> depth;
  int64_t next = -1;
  for (size_t i = 0; i <= steps.size(); ++i) {
    const int64_t until = i < steps.size() ? steps[i].millis : endMillis;
    // the timer fires before the next reading
    while (0 <= next && next < until) {
      const slow::Presentation p = episode.Observe(depth, next);
      settle(p);
      next = p.nextMillis;
    }
    if (i == steps.size()) break;
    depth = steps[i].depth;
    const slow::Presentation p = episode.Observe(depth, steps[i].millis);
    settle(p);
    next = p.nextMillis;
  }
  return run;
}

// One balloon per episode, a flapping network posts none, a second episode
// posts its own, and a return to slow during the recovered state is the same
// episode.
void TestOneBalloonPerEpisode() {
  const Run one = Simulate({{0, 4}, {200'000, 0}}, 400'000);
  Check(one.balloons == 1 && one.everSlow && one.everRecovered && one.last == Phase::Hidden,
        "a slow stretch posts one balloon, recovers and ends (" +
            std::to_string(one.balloons) + " balloons)");

  std::vector<Step> flapping;
  for (int64_t t = 0; t < 600'000; t += 30'000) {
    flapping.push_back({t, 3});
    flapping.push_back({t + 10'000, 0});
  }
  const Run flaps = Simulate(flapping, 700'000);
  Check(flaps.balloons == 0 && !flaps.everSlow, "a flapping network posts nothing (" +
                                                    std::to_string(flaps.balloons) + " balloons)");

  const Run two = Simulate({{0, 4}, {200'000, 0}, {300'000, 2}, {500'000, 0}}, 700'000);
  Check(two.balloons == 2, "two episodes post two balloons (" + std::to_string(two.balloons) + ")");

  const Run continued = Simulate({{0, 4}, {100'000, 0}, {101'000, 4}, {300'000, 0}}, 500'000);
  Check(continued.balloons == 1 && continued.everRecovered,
        "slow again while recovering is the same episode (" +
            std::to_string(continued.balloons) + " balloons)");
}

// The setting off spends the episode's moment without a balloon; the tray not
// connected shows nothing and waits; unavailable ends the episode.
void TestTheBalloonsConditions() {
  const Run quiet = Simulate({{0, 4}, {200'000, 0}}, 400'000, /*connected=*/true, /*notify=*/false);
  Check(quiet.balloons == 0 && quiet.balloonsDue == 1 && quiet.everSlow,
        "the setting off posts nothing and still shows the slow state");

  const Run elsewhere = Simulate({{0, 4}, {200'000, 0}}, 400'000, /*connected=*/false);
  Check(elsewhere.balloons == 0 && !elsewhere.everSlow && !elsewhere.everRecovered,
        "a tray that does not read Connected shows nothing of it");

  const Run interrupted =
      Simulate({{0, 4}, {40'000, std::nullopt}, {50'000, 4}, {200'000, 0}}, 400'000);
  Check(interrupted.balloons == 2,
        "unavailable ends the episode, and the next slow stretch is a new one (" +
            std::to_string(interrupted.balloons) + " balloons)");
}

}  // namespace

int main() {
  const struct {
    const char* name;
    void (*run)();
  } tests[] = {
      {"the delay format", TestTheDelayFormat},
      {"the rate format", TestTheRateFormat},
      {"the summary numbers", TestTheSummaryNumbers},
      {"the worse direction draws", TestTheWorseDirectionDraws},
      {"the slow status fits", TestTheSlowStatusFits},
      {"the pill's label", TestThePillsLabel},
      {"each direction shows its delay", TestEachDirectionShowsItsDelay},
      {"a direction with no delay shows its limited rate", TestADirectionWithNoDelayShowsItsLimitedRate},
      {"a direction with neither hides", TestADirectionWithNeitherHides},
      {"the better direction goes first, then the numbers", TestTheBetterDirectionGoesFirstThenTheNumbers},
      {"the pill fits its room", TestThePillFitsItsRoom},
      {"a return to normal shows the recovered state", TestAReturnToNormalShowsTheRecoveredState},
      {"a slow level during the hold returns to the slow content",
       TestASlowLevelDuringTheHoldReturnsToTheSlowContent},
      {"a reading that goes away is no recovery", TestAReadingThatGoesAwayIsNoRecovery},
      {"normal with nothing shown shows nothing", TestNormalWithNothingShownShowsNothing},
      {"the pill refreshes while it shows the numbers", TestThePillRefreshesWhileItShowsTheNumbers},
      {"the pill reads as one name", TestThePillReadsAsOneName},
      {"a steady slow level appears after 30 s", TestASteadySlowLevelAppearsAfterThirtySeconds},
      {"blips never show", TestBlipsNeverShow},
      {"the history ages out", TestTheHistoryAgesOut},
      {"the stay and the recovered state", TestTheStayAndTheRecoveredState},
      {"a slow level outlasts the stay", TestASlowLevelOutlastsTheStay},
      {"slow again during the recovered state", TestSlowAgainDuringTheRecoveredState},
      {"a reset forgets the history", TestResetForgetsTheHistory},
      {"the tray decision", TestTheTrayDecision},
      {"one balloon per episode", TestOneBalloonPerEpisode},
      {"the balloon's conditions", TestTheBalloonsConditions},
  };
  for (const auto& test : tests) {
    const int before = gFailures;
    test.run();
    std::cout << (gFailures == before ? "ok   " : "FAIL ") << test.name << "\n";
  }
  std::cout << gCases << " checks, " << gFailures << " failed\n";
  return gFailures == 0 ? 0 : 1;
}
