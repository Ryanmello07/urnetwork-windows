// Everything the extender surfaces decide BEFORE they touch a XAML object
// (connect/EXTENDER.md K4, K6, K7): the gossip status dot's colour and word,
// the panel's "N of M" and event rate, the settings form's text and
// placeholders, the share screen's QR placement, and whether an import may
// proceed.
//
// It is all here, and it is all pure, for the reason IpFamilyGroups.h gives:
// the windows solution has no test project and a WinUI 3 app cannot even be
// built on the machine this was written on, so anything expressed as a
// function of plain values is verified by tools/extender-tests.cpp on any
// host, and only the drawing is unverified. The SDK structs are mirrored here
// as plain views rather than included, so the tests need neither
// urnetwork_sdk.hpp nor the SDK dll; the WinUI layer copies the three or four
// fields across at its boundary (ExtenderPanel.cpp, ExtenderSheets.cpp).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ExtenderRingGeometry.h"

namespace urnw {

// ---- the gossip status dot (K4) --------------------------------------------

// green connected, yellow connecting, red disconnected. The SDK derives the
// state for both roles (feed and member) so every app draws one value; this
// only chooses the paint and the word.
enum class GossipTone { Green, Yellow, Red };

// The SDK's GossipState tokens. Mirrors URNET_EXTENDER_GOSSIP_STATE_* so a
// build that has not seen the SDK header still agrees with it.
inline constexpr const char* kGossipStateConnected = "connected";
inline constexpr const char* kGossipStateConnecting = "connecting";
inline constexpr const char* kGossipStateDisconnected = "disconnected";

// An unknown token is RED, never green: "I do not recognise this state" and
// "the network is up" are different answers, and only one of them is safe to
// show as reassurance.
GossipTone ExtenderGossipToneFor(std::string_view gossipState);

// The localization key for the state word. Note the store spells the middle
// one `gossip_connecting`, not `connecting`: `connected` and `disconnected`
// are the shared connection words, the yellow one is the gossip network's own.
const char* ExtenderGossipStateKey(std::string_view gossipState);

// ---- the panel's model (K4) -------------------------------------------------

// As much of the SDK's ExtenderInfo as the panel reads.
struct ExtenderInfoView {
  std::string ip;
  std::string colorHex;
  // addresses carrying at least one live connection right now
  std::int64_t inUse = 0;

  bool operator==(const ExtenderInfoView& o) const {
    return ip == o.ip && colorHex == o.colorHex && inUse == o.inUse;
  }
  bool operator!=(const ExtenderInfoView& o) const { return !(*this == o); }
};

// As much of the SDK's ExtenderStatus as the panel reads.
struct ExtenderStatusView {
  std::string gossipState;
  // K5: N is the addresses carrying a live connection, M every usable entry
  // (known, key active, not on hold) -- what the strategy would dial.
  std::int64_t activeCount = 0;
  std::int64_t reserveCount = 0;
  std::int64_t eventCountLastMinute = 0;
  std::vector<ExtenderInfoView> extenders;

  // The SDK fires once a second whether or not anything moved, so the feed
  // dedups on this before it reaches the UI thread.
  bool operator==(const ExtenderStatusView& o) const {
    return gossipState == o.gossipState && activeCount == o.activeCount &&
           reserveCount == o.reserveCount &&
           eventCountLastMinute == o.eventCountLastMinute && extenders == o.extenders;
  }
  bool operator!=(const ExtenderStatusView& o) const { return !(*this == o); }
};

// What the panel draws, with nothing left to decide.
struct ExtenderPanelModel {
  GossipTone tone = GossipTone::Red;
  // a key for Loc(), never a rendered word
  std::string stateKey = kGossipStateDisconnected;
  std::int64_t activeCount = 0;
  std::int64_t reserveCount = 0;
  std::int64_t eventCountLastMinute = 0;
  // one hollow ring per ACTIVE extender, in status order, in its colour
  std::vector<ExtenderMark> activeMarks;

  bool operator==(const ExtenderPanelModel& o) const {
    return tone == o.tone && stateKey == o.stateKey && activeCount == o.activeCount &&
           reserveCount == o.reserveCount &&
           eventCountLastMinute == o.eventCountLastMinute && activeMarks == o.activeMarks;
  }
  bool operator!=(const ExtenderPanelModel& o) const { return !(*this == o); }
};

ExtenderPanelModel ExtenderPanelModelFor(const ExtenderStatusView& status);

// ---- the settings form (K6) -------------------------------------------------

// As much of the SDK's ExtenderSettings as the form reads. `dnsName` and
// `gossipUrl` are the EFFECTIVE values; the `...Default` flags say whether
// that value is the derived default rather than something configured.
struct ExtenderSettingsView {
  std::string dnsName;
  bool dnsNameDefault = false;
  std::string gossipUrl;
  bool gossipUrlDefault = false;
  std::vector<std::string> hosts;
  std::string networkHost;
  std::vector<std::string> rootPublicKeys;
  bool rootPublicKeysDefault = false;
};

// The three fields, as text. An empty box MEANS the default (that is the SDK's
// contract for SetSettings), so a value that IS the default is shown as an
// empty box with the default named in the placeholder -- clearing a box is how
// a user goes back to the default, and a box pre-filled with the default would
// turn every save into an explicit override of it.
struct ExtenderSettingsForm {
  std::string dnsNameText;
  // the argument for `extender_default_value`; empty when there is no default
  // to name (the value is configured, so the placeholder is never seen)
  std::string dnsNameDefaultValue;
  std::string gossipUrlText;
  std::string gossipUrlDefaultValue;
  // one host per line, in the configured order
  std::string hostsText;

  bool operator==(const ExtenderSettingsForm& o) const {
    return dnsNameText == o.dnsNameText && dnsNameDefaultValue == o.dnsNameDefaultValue &&
           gossipUrlText == o.gossipUrlText &&
           gossipUrlDefaultValue == o.gossipUrlDefaultValue && hostsText == o.hostsText;
  }
  bool operator!=(const ExtenderSettingsForm& o) const { return !(*this == o); }
};

ExtenderSettingsForm ExtenderSettingsFormFor(const ExtenderSettingsView& settings);

// The hosts box back into the list SetSettings takes: split on newlines (and
// commas, because a user who has a comma-separated list in hand will paste
// it), trimmed, empties dropped, duplicates kept in order -- the SDK owns the
// policy, this owns only the typing.
std::vector<std::string> ParseExtenderHostLines(std::string_view text);

// ---- share (K7) -------------------------------------------------------------

inline constexpr const char* kExtenderSharePrefix = "ur-ext:1:";

// Trim a pasted payload: a share arrives out of a chat window or an email and
// carries whatever whitespace came with it.
std::string TrimShareText(std::string_view text);

// A cheap shape check for enabling the import button, NOT a decoder -- the SDK
// is the only thing that decides whether a payload is real (DecodeShare).
bool LooksLikeExtenderShare(std::string_view text);

// Where the connector glyph sits on the rendered code (K7: "the QR renders at
// error level H with the black and white connector glyph centered and a 4 px
// outline of the connector shape around it").
//
// Level H recovers up to ~30% of the codewords, so a centred occlusion of a
// few percent of the area is well inside the budget; kExtenderQrGlyphFraction
// is the glyph's side as a fraction of the code's side, and the OUTLINE is
// counted into the cleared area too -- clearing only the glyph would leave the
// outline painted over live modules, where its 4 px would not read as an
// outline at all.
struct ExtenderQrLayout {
  int moduleCount = 0;     // the code's side, in modules
  double moduleSize = 0;   // px per module
  double side = 0;         // the drawn code's side in px (moduleCount * moduleSize)
  double glyphSide = 0;    // the connector glyph's box
  double glyphLeft = 0;    // relative to the code's top-left
  double glyphTop = 0;
  double outlineThickness = 0;
  // The module index range blanked under the glyph, [clearFrom, clearTo). Both
  // axes, since the cleared area is square and centred.
  int clearFrom = 0;
  int clearTo = 0;
};

inline constexpr double kExtenderQrGlyphFraction = 0.22;
inline constexpr double kExtenderQrOutline = 4.0;

// `pixelSide` is the space the code is drawn into. A moduleCount of zero (no
// code) or a non-positive side yields a zeroed layout, which draws nothing.
ExtenderQrLayout ExtenderQrLayoutFor(int moduleCount, double pixelSide);

// One horizontal run of dark modules, in module coordinates. The code is drawn
// as rectangles, and a 37-module code is ~680 dark modules: merging each row's
// runs turns that into ~200 shapes, which is the difference between a dialog
// that opens and one that hitches while it does.
struct ExtenderQrRun {
  int x = 0;
  int y = 0;
  int length = 0;

  bool operator==(const ExtenderQrRun& o) const {
    return x == o.x && y == o.y && length == o.length;
  }
};

// The dark runs of a code, with the modules under the glyph and its outline
// BLANKED (K7). `dark` is row-major, moduleCount * moduleCount; the clear range
// is [clearFrom, clearTo) on both axes, as ExtenderQrLayoutFor reports it.
// Blanking here rather than at draw time is what keeps the glyph's white box
// from being drawn over live modules, where the 4 px outline would not read as
// an outline at all.
std::vector<ExtenderQrRun> ExtenderQrRunsFor(int moduleCount, const std::vector<bool>& dark,
                                             int clearFrom, int clearTo);

// ---- import (K7, K8) ---------------------------------------------------------

// The SDK's ExtenderShareDecodeResult. `error` is a KEY ID
// (`import_extenders_invalid`, `import_extenders_foreign_host`), not a
// sentence, so every app renders the same words in its own language.
struct ExtenderShareDecodeView {
  bool ok = false;
  std::string error;
  std::string networkHost;
  bool foreignHost = false;
  std::int64_t count = 0;
  bool hasSettings = false;
  std::string settingsHost;
};

// What the import sheet may do with a decoded payload and the current state of
// its "use extender settings" toggle.
struct ExtenderImportDecision {
  // the Import button is live
  bool canImport = false;
  // the payload carries a settings block, so the toggle is shown at all
  bool showSettingsToggle = false;
  // the payload is for another operator: say so, whether or not it blocks
  bool showForeignHost = false;
  // importing would replace the dns name, gossip url and root keys, so it
  // takes a confirmation first
  bool needsConfirm = false;
  // "" when there is nothing to say; otherwise a store key
  std::string messageKey;
  // the argument of messageKey (a host), empty when it takes none
  std::string messageArg;
  // the argument of `import_extenders_confirm_settings`
  std::string confirmArg;
};

// K7: "An import whose network host differs from the space's is refused unless
// 'use extender settings' is chosen, which shows the operator host and asks to
// confirm before replacing the dns name, gossip url and root keys."
//
// So a foreign payload is NOT an error to be dismissed -- it is an import the
// user can still choose, by taking the settings with it. The message stays on
// screen either way, because a code that quietly imported nothing would be
// indistinguishable from one that worked.
ExtenderImportDecision DecideExtenderImport(const ExtenderShareDecodeView& decoded,
                                            bool useSettings);

// The SDK's ExtenderImportResult.
struct ExtenderImportResultView {
  bool ok = false;
  std::string error;  // a key id, as above
  std::int64_t importedCount = 0;
};

// What to say after an import ran: a plural key plus its count on success, the
// error key otherwise. `isPlural` says which of Loc / Plural to call.
struct ExtenderImportOutcome {
  bool ok = false;
  std::string messageKey;
  bool isPlural = false;
  std::int64_t count = 0;
};

ExtenderImportOutcome ExtenderImportOutcomeFor(const ExtenderImportResultView& result);

}  // namespace urnw
