// SPDX-License-Identifier: MPL-2.0
//
// No pch.h on purpose -- see ExtenderPresentation.h. App.vcxproj compiles this
// with PrecompiledHeader=NotUsing, like IpFamilyGroups.cpp.
#include "ExtenderPresentation.h"

#include <cmath>

namespace urnw {
namespace {

constexpr bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

std::string_view Trim(std::string_view value) {
  while (!value.empty() && IsSpace(value.front())) value.remove_prefix(1);
  while (!value.empty() && IsSpace(value.back())) value.remove_suffix(1);
  return value;
}

}  // namespace

// ---- the gossip status dot --------------------------------------------------

GossipTone ExtenderGossipToneFor(std::string_view gossipState) {
  if (gossipState == kGossipStateConnected) return GossipTone::Green;
  if (gossipState == kGossipStateConnecting) return GossipTone::Yellow;
  // kGossipStateDisconnected, empty (no status yet) and anything this build
  // does not know
  return GossipTone::Red;
}

const char* ExtenderGossipStateKey(std::string_view gossipState) {
  switch (ExtenderGossipToneFor(gossipState)) {
    case GossipTone::Green: return "connected";
    case GossipTone::Yellow: return "gossip_connecting";
    case GossipTone::Red: break;
  }
  return "disconnected";
}

// ---- the panel's model ------------------------------------------------------

ExtenderPanelModel ExtenderPanelModelFor(const ExtenderStatusView& status) {
  ExtenderPanelModel model;
  model.tone = ExtenderGossipToneFor(status.gossipState);
  model.stateKey = ExtenderGossipStateKey(status.gossipState);
  // Counts are never rendered negative: a count is a cardinality, and a
  // negative one is a bug upstream, not a number to put in front of a user.
  model.activeCount = 0 < status.activeCount ? status.activeCount : 0;
  model.reserveCount = 0 < status.reserveCount ? status.reserveCount : 0;
  model.eventCountLastMinute =
      0 < status.eventCountLastMinute ? status.eventCountLastMinute : 0;
  for (const ExtenderInfoView& extenderInfo : status.extenders) {
    // "active means carrying at least one live connection right now" (K4)
    if (extenderInfo.inUse <= 0) continue;
    if (extenderInfo.ip.empty()) continue;
    ExtenderMark mark;
    mark.ip = extenderInfo.ip;
    mark.colorHex = extenderInfo.colorHex;
    model.activeMarks.push_back(std::move(mark));
  }
  return model;
}

// ---- the settings form ------------------------------------------------------

ExtenderSettingsForm ExtenderSettingsFormFor(const ExtenderSettingsView& settings) {
  ExtenderSettingsForm form;
  if (settings.dnsNameDefault) {
    form.dnsNameDefaultValue = settings.dnsName;
  } else {
    form.dnsNameText = settings.dnsName;
  }
  if (settings.gossipUrlDefault) {
    form.gossipUrlDefaultValue = settings.gossipUrl;
  } else {
    form.gossipUrlText = settings.gossipUrl;
  }
  for (const std::string& host : settings.hosts) {
    const std::string_view trimmed = Trim(host);
    if (trimmed.empty()) continue;
    if (!form.hostsText.empty()) form.hostsText += "\n";
    form.hostsText.append(trimmed);
  }
  return form;
}

std::vector<std::string> ParseExtenderHostLines(std::string_view text) {
  std::vector<std::string> hosts;
  std::size_t start = 0;
  while (start <= text.size()) {
    std::size_t end = text.size();
    for (std::size_t i = start; i < text.size(); ++i) {
      if (text[i] == '\n' || text[i] == '\r' || text[i] == ',') {
        end = i;
        break;
      }
    }
    const std::string_view item = Trim(text.substr(start, end - start));
    if (!item.empty()) hosts.emplace_back(item);
    if (end == text.size()) break;
    start = end + 1;
  }
  return hosts;
}

// ---- share ------------------------------------------------------------------

std::string TrimShareText(std::string_view text) { return std::string(Trim(text)); }

bool LooksLikeExtenderShare(std::string_view text) {
  const std::string_view trimmed = Trim(text);
  const std::string_view prefix = kExtenderSharePrefix;
  if (trimmed.size() <= prefix.size()) return false;
  return trimmed.substr(0, prefix.size()) == prefix;
}

ExtenderQrLayout ExtenderQrLayoutFor(int moduleCount, double pixelSide) {
  ExtenderQrLayout layout;
  if (moduleCount <= 0 || pixelSide <= 0) return layout;
  layout.moduleCount = moduleCount;
  // A whole number of pixels per module: a fractional one leaves seams between
  // rectangles at some scales and a decoder reading a photo of the screen has
  // to work harder than it should.
  layout.moduleSize = std::floor(pixelSide / moduleCount);
  if (layout.moduleSize < 1) layout.moduleSize = 1;
  layout.side = layout.moduleSize * moduleCount;
  layout.outlineThickness = kExtenderQrOutline;

  // The glyph plus its outline has to come out of whole modules, or the
  // cleared patch and the drawn glyph disagree at the edges.
  const double wanted = layout.side * kExtenderQrGlyphFraction;
  const double cleared = wanted + 2 * layout.outlineThickness;
  int clearedModules = static_cast<int>(std::ceil(cleared / layout.moduleSize));
  // centred: the cleared run has to have the same parity as the code, and
  // every QR version has an odd module count
  if (((clearedModules ^ moduleCount) & 1) != 0) ++clearedModules;
  if (moduleCount < clearedModules) clearedModules = moduleCount;
  if (clearedModules < 1) clearedModules = 1;

  layout.clearFrom = (moduleCount - clearedModules) / 2;
  layout.clearTo = layout.clearFrom + clearedModules;

  const double clearedSide = clearedModules * layout.moduleSize;
  layout.glyphSide = clearedSide - 2 * layout.outlineThickness;
  if (layout.glyphSide < 0) {
    // an absurdly small render: draw the glyph, drop the outline rather than
    // invert it
    layout.glyphSide = clearedSide;
    layout.outlineThickness = 0;
  }
  layout.glyphLeft = (layout.side - layout.glyphSide) / 2;
  layout.glyphTop = layout.glyphLeft;
  return layout;
}

// ---- import -----------------------------------------------------------------

ExtenderImportDecision DecideExtenderImport(const ExtenderShareDecodeView& decoded,
                                            bool useSettings) {
  ExtenderImportDecision decision;
  if (!decoded.ok) {
    // The SDK already named the reason, as a key id. An empty one still has to
    // say something, and "this is not an extender share" is the only thing a
    // decode that failed without a reason can honestly claim.
    decision.messageKey =
        decoded.error.empty() ? std::string("import_extenders_invalid") : decoded.error;
    if (decision.messageKey == "import_extenders_foreign_host") {
      decision.showForeignHost = true;
      decision.messageArg =
          decoded.networkHost.empty() ? decoded.settingsHost : decoded.networkHost;
    }
    return decision;
  }

  decision.showSettingsToggle = decoded.hasSettings;
  decision.showForeignHost = decoded.foreignHost;
  if (decoded.foreignHost) {
    decision.messageKey = "import_extenders_foreign_host";
    decision.messageArg = decoded.networkHost;
  }

  const bool applyingSettings = useSettings && decoded.hasSettings;
  // A foreign payload is importable only WITH its settings: without them the
  // addresses would be judged against an operator that never signed them.
  decision.canImport = !decoded.foreignHost || applyingSettings;
  decision.needsConfirm = applyingSettings;
  if (applyingSettings) {
    decision.confirmArg =
        decoded.settingsHost.empty() ? decoded.networkHost : decoded.settingsHost;
  }
  return decision;
}

ExtenderImportOutcome ExtenderImportOutcomeFor(const ExtenderImportResultView& result) {
  ExtenderImportOutcome outcome;
  if (!result.ok) {
    outcome.messageKey =
        result.error.empty() ? std::string("import_extenders_invalid") : result.error;
    return outcome;
  }
  outcome.ok = true;
  outcome.isPlural = true;
  outcome.messageKey = "import_extenders_imported";
  outcome.count = 0 < result.importedCount ? result.importedCount : 0;
  return outcome;
}

}  // namespace urnw
