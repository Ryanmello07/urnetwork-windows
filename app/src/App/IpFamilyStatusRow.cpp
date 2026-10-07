// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "IpFamilyStatusRow.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>

#include <format>
#include <string>

#include "Localization.h"  // urnw::Plural
#include "PageContext.h"   // pages::Loc
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;

namespace urnw {
namespace {

namespace automation = winrt::Microsoft::UI::Xaml::Automation;
namespace peers = winrt::Microsoft::UI::Xaml::Automation::Peers;

// The pixel face (PP NeueBit) at the smallest size any URnetwork app sets it:
// the plan, referral and onboarding leads on this platform are 22, apple's
// column labels are 16, and the three platforms agree on 16 here.
constexpr double kLabelFontSize = 16.0;
// the status lines, the transport bar legend's and the extender panel's 11
constexpr double kLineFontSize = 11.0;
// the gap between the three columns, the pane's own
constexpr double kColumnSpacing = 8.0;
// the gap between a label and its lines, and between lines
constexpr double kLineSpacing = 2.0;

// A font family from App.xaml by key, or the default when it is missing (the
// same lookup the onboarding, plan and referral cards use for this face).
FontFamily FontResource(std::wstring_view key) {
  try {
    auto resources = Application::Current().Resources();
    auto boxed = winrt::box_value(hstring{key});
    if (resources.HasKey(boxed)) return resources.Lookup(boxed).as<FontFamily>();
  } catch (...) {
  }
  return FontFamily{L"Segoe UI"};
}

// a status line, in the weight the transport bar legend and the extender
// panel set their text
TextBlock MakeLine() {
  TextBlock tb;
  tb.FontSize(kLineFontSize);
  tb.FontWeight(winrt::Windows::UI::Text::FontWeight{500});
  tb.TextWrapping(TextWrapping::NoWrap);
  tb.TextTrimming(TextTrimming::CharacterEllipsis);
  tb.HorizontalAlignment(HorizontalAlignment::Left);
  return tb;
}

// keeps an element out of the accessibility tree; the row is read from its
// root's name, and the invisible template must never be read at all
void HideFromAutomation(UIElement const& element) {
  automation::AutomationProperties::SetAccessibilityView(element, peers::AccessibilityView::Raw);
}

}  // namespace

hstring IpFamilyLabelText(std::string const& token) {
  switch (IpFamilyColumnForToken(token)) {
    case IpFamilyColumn::Dualstack: return pages::Loc("ip_family_both");
    case IpFamilyColumn::V6: return hstring{L"v6"};
    case IpFamilyColumn::V4: break;
  }
  return hstring{L"v4"};
}

IpFamilyStatusRow::IpFamilyStatusRow(Grid const& host) { BuildVisuals(host); }

void IpFamilyStatusRow::BuildVisuals(Grid const& host) {
  labelFont_ = FontResource(L"UrWordmarkFontFamily");
  textBrush_ = colors::TextBrush();
  mutedBrush_ = colors::MutedBrush();
  faintBrush_ = colors::FaintBrush();

  // A plain pane row on the same inset as the transport bar and the extender
  // panel, and NOT a Button: there is nothing to open from here, so there must
  // be nothing that looks pressable. Three equal columns.
  root_ = Grid();
  root_.Padding(Thickness{12, 8, 12, 8});
  root_.HorizontalAlignment(HorizontalAlignment::Stretch);
  root_.ColumnSpacing(kColumnSpacing);
  for (int i = 0; i < 3; ++i) {
    ColumnDefinition column;
    column.Width(GridLength{1, GridUnitType::Star});
    root_.ColumnDefinitions().Append(column);
  }

  // The tallest column a status can produce -- a label over two lines --
  // invisible, in the first cell. It is what gives the row its height, so the
  // row keeps the height of a label and two status lines whatever the live
  // columns show (mmm/DESIGNSTYLE.md "Placeholders, not pop-in"). Its text is
  // never rendered, hit or read, so it is not a user-facing string.
  Column probe = BuildColumn(IpFamilyColumn::Dualstack);
  probe.label.Text(L"Dualstack");
  probe.line0.Text(L"0 connected");
  probe.line1.Text(L"0 connecting");
  probe.panel.Opacity(0);
  probe.panel.IsHitTestVisible(false);
  for (UIElement const& element :
       {UIElement(probe.panel), UIElement(probe.label), UIElement(probe.line0),
        UIElement(probe.line1)}) {
    HideFromAutomation(element);
  }
  Grid::SetColumn(probe.panel, 0);
  root_.Children().Append(probe.panel);

  int index = 0;
  for (IpFamilyColumn column : kIpFamilyColumns) {
    Column built = BuildColumn(column);
    Grid::SetColumn(built.panel, index++);
    root_.Children().Append(built.panel);
    columns_.push_back(std::move(built));
  }

  host.Children().Append(root_);
  statuses_ = IpFamilyColumnStatuses({});
  built_ = true;
  ApplyStrings();  // which also runs the first Rebuild
}

IpFamilyStatusRow::Column IpFamilyStatusRow::BuildColumn(IpFamilyColumn column) const {
  Column built;
  built.column = column;

  built.panel = StackPanel();
  built.panel.Spacing(kLineSpacing);
  built.panel.VerticalAlignment(VerticalAlignment::Top);
  built.panel.HorizontalAlignment(HorizontalAlignment::Stretch);

  built.label = TextBlock();
  built.label.FontFamily(labelFont_);
  built.label.FontSize(kLabelFontSize);
  built.label.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
  built.label.TextWrapping(TextWrapping::NoWrap);
  built.label.TextTrimming(TextTrimming::CharacterEllipsis);
  built.label.HorizontalAlignment(HorizontalAlignment::Left);
  built.label.Foreground(faintBrush_);
  built.panel.Children().Append(built.label);

  built.line0 = MakeLine();
  built.line1 = MakeLine();
  built.panel.Children().Append(built.line0);
  built.panel.Children().Append(built.line1);
  return built;
}

void IpFamilyStatusRow::SetGrid(std::vector<urnet::ProviderGridPoint> const& points) {
  std::vector<IpFamilyPoint> input;
  input.reserve(points.size());
  for (const auto& point : points) {
    input.push_back(IpFamilyPoint{point.ClientId.value_or(std::string()), point.State,
                                  point.IpFamily});
  }
  std::vector<IpFamilyColumnStatus> statuses = IpFamilyColumnStatuses(input);
  if (statuses == statuses_) return;
  statuses_ = std::move(statuses);
  Rebuild();
}

void IpFamilyStatusRow::ApplyStrings() {
  if (!built_) return;
  for (Column& column : columns_) column.label.Text(LabelText(column.column));
  Rebuild();
}

void IpFamilyStatusRow::Rebuild() {
  if (!built_) return;
  const std::vector<IpFamilyTier> tiers = IpFamilyColumnTiers(statuses_);
  // "Dualstack: 3 connected, 1 connecting. IPv4: disconnected. IPv6: 2
  // connected." for a screen reader, from the same texts the columns show
  std::wstring name;
  for (std::size_t i = 0; i < columns_.size() && i < statuses_.size(); ++i) {
    Column& column = columns_[i];
    const Brush brush = BrushFor(i < tiers.size() ? tiers[i] : IpFamilyTier::Unavailable);
    column.label.Foreground(brush);

    const std::vector<IpFamilyStatusLine> lines = IpFamilyStatusLines(statuses_[i]);
    TextBlock slots[2] = {column.line0, column.line1};
    std::wstring lineText;
    for (std::size_t s = 0; s < 2; ++s) {
      if (s < lines.size()) {
        const hstring text = LineText(lines[s]);
        slots[s].Text(text);
        slots[s].Foreground(brush);
        slots[s].Visibility(Visibility::Visible);
        if (!lineText.empty()) lineText += L", ";
        lineText += std::wstring(text.c_str());
      } else {
        slots[s].Visibility(Visibility::Collapsed);
      }
    }
    if (!name.empty()) name += L". ";
    name += std::format(L"{}: {}", std::wstring(column.label.Text().c_str()), lineText);
  }
  automation::AutomationProperties::SetName(root_, hstring{name});
}

hstring IpFamilyStatusRow::LabelText(IpFamilyColumn column) const {
  switch (column) {
    case IpFamilyColumn::Dualstack: return pages::Loc("ip_family_dualstack");
    case IpFamilyColumn::V4: return pages::Loc("ipv4");
    case IpFamilyColumn::V6: break;
  }
  return pages::Loc("ipv6");
}

hstring IpFamilyStatusRow::LineText(IpFamilyStatusLine const& line) const {
  switch (line.kind) {
    case IpFamilyLineKind::Connected:
      return hstring{urnw::Plural("ip_family_connected_count", line.count)};
    case IpFamilyLineKind::Connecting:
      return hstring{urnw::Plural("ip_family_connecting_count", line.count)};
    case IpFamilyLineKind::Disconnected: break;
  }
  return pages::Loc("ip_family_disconnected");
}

Brush IpFamilyStatusRow::BrushFor(IpFamilyTier tier) const {
  switch (tier) {
    case IpFamilyTier::Best: return textBrush_;
    case IpFamilyTier::Active: return mutedBrush_;
    case IpFamilyTier::Unavailable: break;
  }
  return faintBrush_;
}

}  // namespace urnw
