// What the app says about a slow internet connection (apple/DELAY.md §1, §4;
// the sdk's transport delay, connect/DELAY.md §10): the reading the sdk pushes,
// the formats of its numbers, what the connect page's pill says, the dwell
// policy the tray follows so it does not flap, and the tray's decision for the
// slow states.
//
// Two surfaces, two clocks. The connect page's pill (SlowInternetPill) follows
// the sdk's level directly. The tray is the app's notification-area surface and
// follows the dwell instead, as the other platforms' notifications do: it
// shows the slow state only after the level has been above 0 for 30 s of the
// last 60 s, keeps it at least 60 s, and says "Internet back to normal" for
// 3 s before it returns to the ordinary tray. Its balloon is posted once per
// slow episode, when the setting "Notify me when my internet is slow" is on.
//
// Nothing shows while the reading is unavailable: no session, the service's
// device remote down, or no tunnel up. A level kept from before would be stale
// (the device-remote stat rule), so the dwell starts over instead.
//
// Pure standard C++: no WinRT, no sdk header, so tools/slow-internet-tests.cpp
// runs it on any host. SdkHost maps the sdk's TransportDelay onto Reading;
// AppController runs the dwell and the tray decision; the words are the store's
// slow_internet keys, looked up where they are drawn.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "TunnelIndicator.h"

namespace urnw::slow {

// The sdk's TransportDelay, field for field (connect/DELAY.md §10.4).
struct Reading {
  // 0 (normal) to 1 (worst), quantized
  double upQuality = 0;
  double downQuality = 0;
  // the worst network-limited rate in the window; 0 when none was limited
  int64_t upBitsPerSecond = 0;
  int64_t downBitsPerSecond = 0;
  // the worst standing queue in the window; 0 when none
  int64_t upQueueDelayMillis = 0;
  int64_t downQueueDelayMillis = 0;
  // the worst reading came from a DNS-carried fallback transport: the copy
  // says URnetwork is using a slower fallback connection
  bool fallback = false;

  bool operator==(const Reading&) const = default;
};

// The squares the indicator draws: the worse direction's.
inline int DepthOf(const Reading& reading) {
  return tunnel::DepthForQuality(std::max(reading.upQuality, reading.downQuality));
}

// ---- formats ----------------------------------------------------------------
// The units are not localized. A value of 0 or less has no line: "".

// "640 ms" under a second, "1.2 s" under ten, "12 s" from there. Rounded to
// the unit shown, and a value that rounds up to the next range is written in
// that range's form (9,960 ms is "10 s", not "10.0 s").
inline std::string FormatDelay(int64_t millis) {
  if (millis <= 0) return "";
  char text[32];
  if (millis < 1000) {
    std::snprintf(text, sizeof(text), "%lld ms", static_cast<long long>(millis));
    return text;
  }
  const int64_t tenths = (millis + 50) / 100;
  if (tenths < 100) {
    std::snprintf(text, sizeof(text), "%lld.%lld s", static_cast<long long>(tenths / 10),
                  static_cast<long long>(tenths % 10));
    return text;
  }
  std::snprintf(text, sizeof(text), "%lld s", static_cast<long long>((millis + 500) / 1000));
  return text;
}

// "48 kb/s" under 1,000,000 bit/s, in whole kilobits and at least 1, then
// "1.4 Mb/s" with one decimal. Rounded the same way: 999,600 bit/s is
// "1.0 Mb/s", not "1000 kb/s".
inline std::string FormatRate(int64_t bitsPerSecond) {
  if (bitsPerSecond <= 0) return "";
  char text[32];
  const int64_t kilobits = (bitsPerSecond + 500) / 1000;
  if (kilobits < 1000) {
    std::snprintf(text, sizeof(text), "%lld kb/s",
                  static_cast<long long>(std::max<int64_t>(1, kilobits)));
    return text;
  }
  const int64_t tenths = (bitsPerSecond + 50'000) / 100'000;
  std::snprintf(text, sizeof(text), "%lld.%lld Mb/s", static_cast<long long>(tenths / 10),
                static_cast<long long>(tenths % 10));
  return text;
}

// An ASCII format as the wide text it is drawn in.
inline std::wstring Wide(std::string_view ascii) { return std::wstring(ascii.begin(), ascii.end()); }

// One direction's number for a one-line summary: its delay, or its rate when
// only the rate was limited; "" when it has neither.
inline std::string DirectionNumber(int64_t delayMillis, int64_t bitsPerSecond) {
  const std::string delay = FormatDelay(delayMillis);
  return delay.empty() ? FormatRate(bitsPerSecond) : delay;
}

// The tooltip's numbers, "↑ 640 ms · ↓ 1.2 s": each direction that has one,
// upload first; "" when neither has.
inline std::wstring SummaryNumbers(const Reading& reading) {
  std::wstring summary;
  const std::string up = DirectionNumber(reading.upQueueDelayMillis, reading.upBitsPerSecond);
  const std::string down = DirectionNumber(reading.downQueueDelayMillis, reading.downBitsPerSecond);
  if (!up.empty()) summary += L"↑ " + Wide(up);
  if (!down.empty()) {
    if (!summary.empty()) summary += L" · ";
    summary += L"↓ " + Wide(down);
  }
  return summary;
}

// ---- the pill -----------------------------------------------------------------
// What the connect page's pill says (apple/DELAY.md §5.2, variant A+), the
// Apple pill's SlowInternetPillContent rules: "Poor signal", or "Fallback" on
// the fallback connection; then each direction's standing delay, or its
// limited rate when it has no delay, and nothing when it has neither. The
// larger delay is the worse direction, drawn amber. Recovered, the pill says
// "Internet back to normal" instead.

// While the pill shows the slow content on a window that presents, its numbers
// are read again this often: they move with no change of level, which is all
// the listener carries.
inline constexpr int64_t kPillRefreshMillis = 10'000;

enum class Direction {
  Up,
  Down,
};

// The larger standing delay's direction; nullopt when the delays are equal, so
// a limited rate never makes a direction the worse.
inline std::optional<Direction> WorseDirection(int64_t upDelayMillis, int64_t downDelayMillis) {
  if (downDelayMillis < upDelayMillis) return Direction::Up;
  if (upDelayMillis < downDelayMillis) return Direction::Down;
  return std::nullopt;
}

struct PillContent {
  enum class Label {
    // "Poor signal"
    PoorSignal,
    // "Fallback": the worst reading came from the fallback transport
    Fallback,
  };

  Label label = Label::PoorSignal;
  // "640 ms", or "48 kb/s" with no delay; "" hides the direction
  std::string up;
  std::string down;
  // the larger delay's direction; nullopt when the delays are equal
  std::optional<Direction> worse;
  // the few seconds after a return to normal, before the pill leaves
  bool recovered = false;

  // With room for one direction: the worse one, or the download when neither
  // is worse (the upload when only it shows).
  PillContent WorseOnly() const {
    PillContent content = *this;
    if (worse.value_or(down.empty() ? Direction::Up : Direction::Down) == Direction::Up) {
      content.down.clear();
    } else {
      content.up.clear();
    }
    return content;
  }

  // With no room for numbers: the tunnel and the label.
  PillContent LabelOnly() const {
    PillContent content = *this;
    content.up.clear();
    content.down.clear();
    return content;
  }

  bool operator==(const PillContent&) const = default;
};

// What the pill says for a slow reading.
inline PillContent PillContentFor(const Reading& reading) {
  PillContent content;
  content.label = reading.fallback ? PillContent::Label::Fallback : PillContent::Label::PoorSignal;
  content.up = DirectionNumber(reading.upQueueDelayMillis, reading.upBitsPerSecond);
  content.down = DirectionNumber(reading.downQueueDelayMillis, reading.downBitsPerSecond);
  content.worse = WorseDirection(reading.upQueueDelayMillis, reading.downQueueDelayMillis);
  return content;
}

// What the pill shows once `reading` arrives (nullopt: unavailable), after
// `shown` (nullopt when the pill is not up, or is leaving):
//   * while the level is above 0, the slow content, which also takes the
//     recovered state's place;
//   * on a return to normal from the slow content, the recovered state, which
//     then holds;
//   * nullopt, the pill leaving, when the reading goes away (no session, the
//     remote down) or something more severe has the top of the page
//     (`hidden`: the can't-reach banner). Neither is a recovery.
inline std::optional<PillContent> NextPillContent(const std::optional<PillContent>& shown,
                                                  const std::optional<Reading>& reading,
                                                  bool hidden) {
  if (hidden) return std::nullopt;
  if (reading && 0 < DepthOf(*reading)) return PillContentFor(*reading);
  if (!reading || !shown) return std::nullopt;
  if (shown->recovered) return shown;
  PillContent recovered = shown->LabelOnly();
  recovered.worse.reset();
  recovered.recovered = true;
  return recovered;
}

// The form the pill takes in `room`, the first that fits, as SwiftUI's
// ViewThatFits picks it: the whole content, else the worse direction alone,
// else the label alone, which shows whatever the room. `width` is what a form
// needs on screen.
template <typename Width>
PillContent FitPill(const PillContent& content, double room, const Width& width) {
  if (width(content) <= room) return content;
  const PillContent one = content.WorseOnly();
  if (width(one) <= room) return one;
  return content.LabelOnly();
}

// The numbers are read again (kPillRefreshMillis) while the slow content shows
// on a window that presents: not while the pill says "Internet back to
// normal", and not off screen.
inline bool PillRefreshes(const std::optional<PillContent>& shown, bool presenting) {
  return presenting && shown && !shown->recovered;
}

// The pill's one automation name: `words`, the label or "Internet back to
// normal", then each direction that shows a number, after its own words:
// "Poor signal, Upload delay 640 ms, Download delay 1.2 s". A rate is read
// after the delay words too, as the Apple pill reads it.
inline std::wstring PillName(std::wstring_view words, const PillContent& content,
                             std::wstring_view upWords, std::wstring_view downWords) {
  std::wstring name(words);
  if (!content.up.empty()) name += L", " + std::wstring(upWords) + L" " + Wide(content.up);
  if (!content.down.empty()) name += L", " + std::wstring(downWords) + L" " + Wide(content.down);
  return name;
}

// ---- the dwell ----------------------------------------------------------------
// The shared contract's defaults for the notification surfaces.

// the window the level's history is read over
inline constexpr int64_t kDwellWindowMillis = 60'000;
// above 0 for this long of the window before the slow state shows
inline constexpr int64_t kDwellAppearMillis = 30'000;
// then shown at least this long
inline constexpr int64_t kDwellStayMillis = 60'000;
// "Internet back to normal" this long before the slow state is gone; the
// pill's recovered state holds as long
inline constexpr int64_t kRecoveredMillis = 3'000;
// While the tray says slow, its numbers are read again this often: the
// listener carries them only with a change of level (apple/DELAY.md §5.5).
inline constexpr int64_t kNumbersRefreshMillis = 30'000;

enum class Phase {
  // nothing shows
  Hidden,
  // the slow state: the tunnel and the slow words
  Slow,
  // "Internet back to normal", then Hidden
  Recovered,
};

struct Presentation {
  Phase phase = Phase::Hidden;
  // while Slow, the squares to draw: the reading's, or while the reading is
  // back to 0 within the minimum stay, the last depth above 0
  int depth = 0;
  // observe again at this time with the same depth if no reading comes before
  // (a timer); -1 when nothing changes without a new reading
  int64_t nextMillis = -1;

  bool operator==(const Presentation&) const = default;
};

// The dwell over the readings' depths, on a monotonic clock in milliseconds.
// Observe each reading's depth as it comes, and the same depth again at
// Presentation::nextMillis; Reset when the reading is unavailable. A slow
// episode runs from Hidden to Hidden: a slow reading during the recovered
// state that still meets the appear rule continues the episode rather than
// starting another, so its balloon is not posted twice.
class Dwell {
 public:
  Presentation Observe(int depth, int64_t nowMillis) {
    depth = std::clamp(depth, 0, tunnel::kMaxDepth);
    // the history: a span opens when the depth leaves 0 and closes when it
    // returns
    const bool open = !spans_.empty() && spans_.back().end < 0;
    if (0 < depth && !open) spans_.push_back(Span{nowMillis, -1});
    if (depth == 0 && open) spans_.back().end = nowMillis;
    spans_.erase(std::remove_if(spans_.begin(), spans_.end(),
                                [&](const Span& span) {
                                  return 0 <= span.end && span.end <= nowMillis - kDwellWindowMillis;
                                }),
                 spans_.end());
    const int64_t slowMillis = SlowMillis(nowMillis);
    const bool appears = 0 < depth && kDwellAppearMillis <= slowMillis;

    switch (phase_) {
      case Phase::Hidden:
        if (appears) Enter(Phase::Slow, nowMillis);
        break;
      case Phase::Slow:
        if (depth == 0 && kDwellStayMillis <= nowMillis - since_) Enter(Phase::Recovered, nowMillis);
        break;
      case Phase::Recovered:
        if (appears) {
          Enter(Phase::Slow, nowMillis);
        } else if (kRecoveredMillis <= nowMillis - since_) {
          Enter(Phase::Hidden, nowMillis);
        }
        break;
    }
    if (0 < depth) depth_ = depth;

    Presentation presentation;
    presentation.phase = phase_;
    if (phase_ == Phase::Slow) presentation.depth = depth_;
    switch (phase_) {
      case Phase::Hidden:
        // the earliest the appear rule can hold if the depth stays above 0:
        // the history only grows that fast, and ageing can only delay it
        if (0 < depth) presentation.nextMillis = nowMillis + (kDwellAppearMillis - slowMillis);
        break;
      case Phase::Slow:
        if (depth == 0) presentation.nextMillis = since_ + kDwellStayMillis;
        break;
      case Phase::Recovered:
        presentation.nextMillis = since_ + kRecoveredMillis;
        if (0 < depth) {
          presentation.nextMillis = std::min(presentation.nextMillis,
                                             nowMillis + (kDwellAppearMillis - slowMillis));
        }
        break;
    }
    return presentation;
  }

  // The reading is unavailable: nothing shows, and the history starts over.
  void Reset() {
    spans_.clear();
    phase_ = Phase::Hidden;
    since_ = 0;
    depth_ = 0;
  }

  Phase phase() const { return phase_; }

 private:
  // [start, end) above 0; end -1 while still above 0
  struct Span {
    int64_t start = 0;
    int64_t end = -1;
  };

  void Enter(Phase phase, int64_t nowMillis) {
    phase_ = phase;
    since_ = nowMillis;
  }

  // How long the depth was above 0 in the window ending now.
  int64_t SlowMillis(int64_t nowMillis) const {
    const int64_t windowStart = nowMillis - kDwellWindowMillis;
    int64_t total = 0;
    for (const Span& span : spans_) {
      const int64_t start = std::max(span.start, windowStart);
      const int64_t end = span.end < 0 ? nowMillis : std::min(span.end, nowMillis);
      if (start < end) total += end - start;
    }
    return total;
  }

  std::vector<Span> spans_;
  Phase phase_ = Phase::Hidden;
  // when the phase was entered
  int64_t since_ = 0;
  // the last depth above 0
  int depth_ = 0;
};

// ---- the tray ---------------------------------------------------------------

struct TrayFacts {
  // the dwell's presentation
  Presentation presentation;
  // the tray otherwise reads Connected (TrayConnectionState.h). Every other
  // status says something more urgent about the connection, so the slow state
  // waits behind it with the tray's own icon and words.
  bool connected = false;
  // Settings > General: "Notify me when my internet is slow"
  bool notify = true;
  // this episode's balloon moment has passed (posted, or the setting was off)
  bool notified = false;
};

// what the tooltip says after the app's name
enum class Tip {
  // the connection status, as TrayConnectionState.h decides it
  Status,
  // "Slow internet", or "Using a slower fallback connection", with the numbers
  Slow,
  // "Internet back to normal"
  Recovered,
};

struct TrayView {
  // 1 to 4: the tunnel icon of that row of the depth table (icon 4 is the
  // full tunnel); 0: the tray's own icon
  int icon = 0;
  Tip tip = Tip::Status;
  // the episode's balloon moment: the first time this episode reads slow on
  // the tray. The caller notes it whether or not the setting posts one, so a
  // setting turned on later in the episode does not post a late one.
  bool balloonDue = false;
  // post the balloon now
  bool postBalloon = false;

  bool operator==(const TrayView&) const = default;
};

inline TrayView DecideTray(const TrayFacts& facts) {
  TrayView view;
  if (!facts.connected) return view;
  switch (facts.presentation.phase) {
    case Phase::Hidden:
      break;
    case Phase::Slow:
      view.icon = tunnel::IconLevelForDepth(facts.presentation.depth);
      view.tip = Tip::Slow;
      view.balloonDue = !facts.notified;
      view.postBalloon = view.balloonDue && facts.notify;
      break;
    case Phase::Recovered:
      // the tunnel has cleared: the tray's own icon, with the words
      view.tip = Tip::Recovered;
      break;
  }
  return view;
}

// The tray's slow state over time, as AppController holds it: the dwell, and
// whether this episode's balloon moment has passed.
class TrayEpisode {
 public:
  // A reading's depth at `nowMillis`, or nullopt while the reading is
  // unavailable (no session, the remote down, no tunnel up): then nothing
  // shows and the dwell starts over. Observe the same depth again at the
  // returned nextMillis when no reading comes first.
  Presentation Observe(std::optional<int> depth, int64_t nowMillis) {
    if (depth) {
      presentation_ = dwell_.Observe(*depth, nowMillis);
    } else {
      dwell_.Reset();
      presentation_ = Presentation{};
    }
    // the episode ends when the slow state is gone
    if (presentation_.phase == Phase::Hidden) notified_ = false;
    return presentation_;
  }

  // The tray now, over its connection status and the setting (TrayFacts).
  // The episode's balloon moment is spent here, whether the setting posts the
  // balloon or not.
  TrayView Decide(bool connected, bool notify) {
    TrayFacts facts;
    facts.presentation = presentation_;
    facts.connected = connected;
    facts.notify = notify;
    facts.notified = notified_;
    const TrayView view = DecideTray(facts);
    if (view.balloonDue) notified_ = true;
    return view;
  }

  const Presentation& presentation() const { return presentation_; }

 private:
  Dwell dwell_;
  Presentation presentation_;
  bool notified_ = false;
};

// NOTIFYICONDATAW::szTip's 128 characters, less the terminating null.
inline constexpr std::size_t kTooltipCharacters = 127;

// The tray's slow status: `words` ("Slow internet"), then the reading's numbers
// in parentheses when the tooltip has room for them after `prefixLength`
// characters (the app's name and the dash before the status). The shell cuts
// whatever is still too long.
inline std::wstring SlowStatus(std::wstring_view words, const Reading& reading,
                               std::size_t prefixLength) {
  std::wstring status(words);
  const std::wstring numbers = SummaryNumbers(reading);
  if (numbers.empty()) return status;
  const std::wstring withNumbers = status + L" (" + numbers + L")";
  if (prefixLength + withNumbers.size() <= kTooltipCharacters) return withNumbers;
  return status;
}

}  // namespace urnw::slow
