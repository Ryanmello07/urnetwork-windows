// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "IpFamilyHistogram.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Documents.h>  // RichTextBlock inline flow (wrapping rows)
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include <cmath>
#include <format>

#include "PageContext.h"  // pages::Adv
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;

namespace urnw {
namespace {

// wingdi.h declares ::Ellipse; alias the XAML shape so unqualified lookup under
// the using-directives stays unambiguous
using ShapeEllipse = winrt::Microsoft::UI::Xaml::Shapes::Ellipse;
namespace documents = winrt::Microsoft::UI::Xaml::Documents;
namespace automation = winrt::Microsoft::UI::Xaml::Automation;

// the label column, wide enough for the widest of the three labels in every
// script the store carries, so the three dot rows start on one vertical line
constexpr double kLabelMinWidth = 40.0;
// the gap between neighbouring dots: the widget's dots touch (cell = diameter);
// in a wrapping row a hair of air keeps a long row countable
constexpr double kDotGap = 2.0;

// ---- the not-yet-in-store strings ------------------------------------------
// The histogram is new on every platform at once (IPV6.md D2); its ids reach
// the shared store with the next localization sync. Until then each call
// carries the id the store should have AND the English it renders
// (PageContext.h Adv: the store wins the moment a key appears). The ids extract
// with the other pending families:
//   grep -ohE '"ip_famil[a-z0-9_]+"' app/src/App/*.cpp | sort -u
hstring FamilyText(std::string_view key, const wchar_t* english) {
  return pages::Adv(key, english);
}

TextBlock MakeLabel(hstring const& text, double fontSize, Brush const& brush) {
  TextBlock tb;
  tb.Text(text);
  tb.FontSize(fontSize);
  tb.FontWeight(winrt::Windows::UI::Text::FontWeight{500});
  if (brush) tb.Foreground(brush);
  tb.VerticalAlignment(VerticalAlignment::Center);
  return tb;
}

// Core WinUI has no WrapPanel, so the dots flow inline in a RichTextBlock,
// which wraps to the next line when the next dot would overflow the pane -- the
// same flow TransportBar's legend and the sheets' chips use, and what apple's
// FlowRow does for this exact row.
RichTextBlock MakeFlow() {
  RichTextBlock flow;
  flow.TextWrapping(TextWrapping::Wrap);
  flow.IsTextSelectionEnabled(false);
  flow.Blocks().Append(documents::Paragraph());
  return flow;
}

documents::Paragraph FlowParagraph(RichTextBlock const& flow) {
  return flow.Blocks().GetAt(0).as<documents::Paragraph>();
}

void ClearFlow(RichTextBlock const& flow) { FlowParagraph(flow).Inlines().Clear(); }

void AppendInline(RichTextBlock const& flow, FrameworkElement const& item) {
  // right margin = the inter-dot gap, bottom = the wrap-line gap
  item.Margin(Thickness{0, 0, kDotGap, kDotGap});
  documents::InlineUIContainer container;
  container.Child(item);
  FlowParagraph(flow).Inlines().Append(container);
}

}  // namespace

hstring IpFamilyLabelText(std::string const& token) {
  switch (IpFamilyGroupForToken(token)) {
    case IpFamilyGroup::Both: return FamilyText("ip_family_both", L"Both");
    case IpFamilyGroup::V6: return hstring{L"v6"};
    case IpFamilyGroup::V4: break;
  }
  return hstring{L"v4"};
}

IpFamilyHistogram::IpFamilyHistogram(Grid const& host) { BuildVisuals(host); }

void IpFamilyHistogram::BuildVisuals(Grid const& host) {
  mutedBrush_ = colors::MutedBrush();
  faintBrush_ = colors::FaintBrush();
  // the connect widget's Added dot (ConnectCanvas::ColorForPointState)
  dotBrush_ = SolidColorBrush(colors::kUrGreen);

  // A plain pane row (the same inset as the transport bar's Button padding),
  // not a Button: there is nothing to open from here yet.
  root_ = StackPanel();
  root_.Spacing(4);
  root_.Padding(Thickness{12, 8, 12, 8});
  root_.HorizontalAlignment(HorizontalAlignment::Stretch);

  // title, styled like the transport bar's
  const hstring title = FamilyText("ip_families", L"IP families");
  TextBlock titleLabel = MakeLabel(title, 11, mutedBrush_);
  titleLabel.HorizontalAlignment(HorizontalAlignment::Left);
  titleLabel.Margin(Thickness{0, 0, 0, 2});
  root_.Children().Append(titleLabel);

  for (IpFamilyGroup group : {IpFamilyGroup::Both, IpFamilyGroup::V4, IpFamilyGroup::V6}) {
    Row row;
    row.group = group;

    Grid line;
    ColumnDefinition c0, c1;
    c0.Width(GridLength{0, GridUnitType::Auto});
    c1.Width(GridLength{1, GridUnitType::Star});
    line.ColumnDefinitions().Append(c0);
    line.ColumnDefinitions().Append(c1);
    line.ColumnSpacing(8);

    row.label = MakeLabel(IpFamilyLabelText(IpFamilyGroupToken(group)), 11, mutedBrush_);
    row.label.MinWidth(kLabelMinWidth);
    row.label.VerticalAlignment(VerticalAlignment::Top);
    Grid::SetColumn(row.label, 0);
    line.Children().Append(row.label);

    row.flow = MakeFlow();
    row.flow.VerticalAlignment(VerticalAlignment::Top);
    Grid::SetColumn(row.flow, 1);
    line.Children().Append(row.flow);

    root_.Children().Append(line);
    rows_.push_back(row);
  }

  automation::AutomationProperties::SetName(root_, title);
  host.Children().Append(root_);
  built_ = true;
  Rebuild();
}

void IpFamilyHistogram::SetGrid(std::vector<urnet::ProviderGridPoint> const& points,
                                double dotDiameter) {
  std::vector<IpFamilyDot> dots;
  dots.reserve(points.size());
  // EXTENDER.md K1/K2: the drawer draws the SAME dots and the same rings as
  // the hero, at its own dot size, so the extenders ride along with the
  // grouping. Only providers actually reached through one get an entry, which
  // is nearly always none of them.
  std::unordered_map<std::string, std::vector<ExtenderMark>> marks;
  for (const auto& point : points) {
    IpFamilyDot dot;
    dot.clientId = point.ClientId.value_or(std::string());
    dot.state = point.State;
    dot.ipFamily = point.IpFamily;
    if (!dot.clientId.empty()) {
      std::vector<ExtenderMark> pointMarks =
          PairExtenderMarks(point.ExtenderIps, point.ExtenderColorHexes);
      if (!pointMarks.empty()) marks.emplace(dot.clientId, std::move(pointMarks));
    }
    dots.push_back(std::move(dot));
  }
  IpFamilyGroups groups = GroupAddedProvidersByIpFamily(dots);
  // a diameter the canvas has not computed yet (0) keeps the last one, or the
  // pure fallback when there was none, so a dot is never drawn at size zero
  const double diameter = 0 < dotDiameter
                              ? dotDiameter
                              : (0 < dotDiameter_ ? dotDiameter_
                                                  : IpFamilyDotDiameter(0, 0, 0));
  const bool changed = groups != groups_ || marks != marks_ ||
                       std::fabs(diameter - dotDiameter_) > 0.01;
  groups_ = std::move(groups);
  marks_ = std::move(marks);
  dotDiameter_ = diameter;
  if (changed) Rebuild();
}

void IpFamilyHistogram::Rebuild() {
  if (!built_) return;
  for (Row& row : rows_) RebuildRow(row);
}

// One provider's dot, at the row's dot size. With no extenders this is the
// bare ellipse the row has always drawn; with them it is that ellipse, shrunk
// by K2's rule, centred inside one hollow ring per extender in the colour the
// SDK paired with it -- the hero's construction at this surface's size, which
// is what makes "the same set of providers" read as the same set.
FrameworkElement IpFamilyHistogram::MakeDot(std::vector<ExtenderMark> const& marks) const {
  const double diameter = 0 < dotDiameter_ ? dotDiameter_ : IpFamilyDotDiameter(0, 0, 0);
  ShapeEllipse dot;
  dot.Fill(dotBrush_);
  dot.IsHitTestVisible(false);
  if (marks.empty()) {
    dot.Width(diameter);
    dot.Height(diameter);
    return dot;
  }

  const ExtenderRingLayout layout = ExtenderRingsFor(diameter, marks);
  dot.Width(layout.dotDiameter);
  dot.Height(layout.dotDiameter);

  // A fixed-size Grid, so the whole assembly occupies exactly one dot's worth
  // of the row and its children centre on one another for free.
  Grid cell;
  cell.Width(diameter);
  cell.Height(diameter);
  cell.IsHitTestVisible(false);
  for (const ExtenderRing& spec : layout.rings) {
    ShapeEllipse ring;
    ring.Width(spec.diameter);
    ring.Height(spec.diameter);
    ring.StrokeThickness(spec.stroke);
    // K3: the SDK's colour, drawn as given; muted when it sent none, never a
    // hue this app invented.
    Brush stroke = mutedBrush_;
    if (spec.hasColor) {
      stroke = SolidColorBrush(
          winrt::Windows::UI::Color{255, spec.color.r, spec.color.g, spec.color.b});
    }
    ring.Stroke(stroke);
    if (spec.dashed) {
      DoubleCollection dashes;
      dashes.Append(kExtenderRingDashOn);
      dashes.Append(kExtenderRingDashOff);
      ring.StrokeDashArray(dashes);
      ring.StrokeDashCap(PenLineCap::Round);
    }
    ring.HorizontalAlignment(HorizontalAlignment::Center);
    ring.VerticalAlignment(VerticalAlignment::Center);
    cell.Children().Append(ring);
  }
  dot.HorizontalAlignment(HorizontalAlignment::Center);
  dot.VerticalAlignment(VerticalAlignment::Center);
  cell.Children().Append(dot);
  return cell;
}

void IpFamilyHistogram::RebuildRow(Row& row) {
  ClearFlow(row.flow);
  const std::vector<std::string>& ids = groups_.For(row.group);
  static const std::vector<ExtenderMark> kNoMarks;
  for (const std::string& id : ids) {
    const auto found = marks_.find(id);
    AppendInline(row.flow, MakeDot(found == marks_.end() ? kNoMarks : found->second));
  }
  // an empty row is just its label: the label stays, the flow collapses so the
  // row keeps a single line's height rather than an empty paragraph's
  row.flow.Visibility(ids.empty() ? Visibility::Collapsed : Visibility::Visible);
  row.label.Foreground(ids.empty() ? faintBrush_ : mutedBrush_);
  // "Both: 3 providers" for a screen reader; the dots themselves are decorative
  automation::AutomationProperties::SetName(
      row.flow, hstring{std::format(L"{}: {}", std::wstring(row.label.Text().c_str()),
                                    ids.size())});
}

}  // namespace urnw
