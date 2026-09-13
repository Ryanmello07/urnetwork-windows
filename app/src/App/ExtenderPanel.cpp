// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "ExtenderPanel.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include "ExtenderRingGeometry.h"
#include "PageContext.h"  // pages::Loc
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;

namespace urnw {
namespace {

// wingdi.h declares ::Ellipse; alias the XAML shape so unqualified lookup under
// the using-directives stays unambiguous (IpFamilyHistogram.cpp does the same)
using ShapeEllipse = winrt::Microsoft::UI::Xaml::Shapes::Ellipse;
namespace automation = winrt::Microsoft::UI::Xaml::Automation;

// The panel's rings are NOT the hero's dots: there is no grid cell here to
// inherit a size from, and a ring at the hero's ~18px cell would be a smudge in
// a text row. 12px with K2's own 2px stroke is one text line tall, which is what
// lets a row of them sit beside the count without changing the row's height.
constexpr double kRingDiameter = 12.0;
constexpr double kRingGap = 4.0;
// the gossip status dot, the same 8px as the connect status line's
constexpr double kStatusDot = 8.0;

// The connect status line's own triple (ConnectPage::ApplyStatus), so a green
// dot means the same thing everywhere in this window.
winrt::Windows::UI::Color ColorForTone(GossipTone tone) {
  switch (tone) {
    case GossipTone::Green: return colors::kUrGreen;
    case GossipTone::Yellow: return colors::kStatusConnecting;
    case GossipTone::Red: break;
  }
  return colors::kUrCoral;
}

TextBlock MakeLabel(double fontSize, Brush const& brush) {
  TextBlock tb;
  tb.FontSize(fontSize);
  tb.FontWeight(winrt::Windows::UI::Text::FontWeight{500});
  if (brush) tb.Foreground(brush);
  tb.VerticalAlignment(VerticalAlignment::Center);
  return tb;
}

}  // namespace

ExtenderPanel::ExtenderPanel(Grid const& host) { BuildVisuals(host); }

void ExtenderPanel::BuildVisuals(Grid const& host) {
  mutedBrush_ = colors::MutedBrush();
  faintBrush_ = colors::FaintBrush();

  // A plain pane row on the same inset as the transport bar and the histogram,
  // and NOT a Button: K4 says tapping does nothing and there is no details
  // panel, so there must be nothing here that looks pressable.
  root_ = StackPanel();
  root_.Spacing(4);
  root_.Padding(Thickness{12, 8, 12, 8});
  root_.HorizontalAlignment(HorizontalAlignment::Stretch);

  title_ = MakeLabel(11, mutedBrush_);
  title_.HorizontalAlignment(HorizontalAlignment::Left);
  title_.Margin(Thickness{0, 0, 0, 2});
  root_.Children().Append(title_);

  // K4's order is left to right: the rings, then "N of M", then the gossip dot
  // with its word and rate. The gossip group is pushed to the right edge, which
  // is this pane's own key/value rhythm (every row above it puts its figure
  // there) and keeps the reading order K4 asks for.
  Grid line;
  ColumnDefinition c0, c1, c2;
  c0.Width(GridLength{0, GridUnitType::Auto});
  c1.Width(GridLength{0, GridUnitType::Auto});
  c2.Width(GridLength{1, GridUnitType::Star});
  line.ColumnDefinitions().Append(c0);
  line.ColumnDefinitions().Append(c1);
  line.ColumnDefinitions().Append(c2);
  line.ColumnSpacing(8);

  ringRow_ = StackPanel();
  ringRow_.Orientation(Orientation::Horizontal);
  ringRow_.Spacing(kRingGap);
  ringRow_.VerticalAlignment(VerticalAlignment::Center);
  ringRow_.IsHitTestVisible(false);
  Grid::SetColumn(ringRow_, 0);
  line.Children().Append(ringRow_);

  countText_ = MakeLabel(12, mutedBrush_);
  Grid::SetColumn(countText_, 1);
  line.Children().Append(countText_);

  gossipRow_ = StackPanel();
  gossipRow_.Orientation(Orientation::Horizontal);
  gossipRow_.Spacing(6);
  gossipRow_.HorizontalAlignment(HorizontalAlignment::Right);
  gossipRow_.VerticalAlignment(VerticalAlignment::Center);
  gossipDot_ = ShapeEllipse();
  gossipDot_.Width(kStatusDot);
  gossipDot_.Height(kStatusDot);
  gossipDot_.VerticalAlignment(VerticalAlignment::Center);
  gossipDot_.IsHitTestVisible(false);
  gossipBrush_ = SolidColorBrush(colors::kUrCoral);
  gossipDot_.Fill(gossipBrush_);
  gossipRow_.Children().Append(gossipDot_);
  gossipText_ = MakeLabel(12, mutedBrush_);
  gossipRow_.Children().Append(gossipText_);
  eventsText_ = MakeLabel(11, faintBrush_);
  gossipRow_.Children().Append(eventsText_);
  Grid::SetColumn(gossipRow_, 2);
  line.Children().Append(gossipRow_);

  root_.Children().Append(line);

  host.Children().Append(root_);
  built_ = true;
  ApplyStrings();  // which also runs the first Rebuild
}

void ExtenderPanel::ApplyStrings() {
  if (!built_) return;
  const hstring title = pages::Loc("extenders");
  title_.Text(title);
  automation::AutomationProperties::SetName(root_, title);
  // the rings and the dot say nothing on their own; these are what a screen
  // reader reads in their place
  automation::AutomationProperties::SetName(ringRow_, pages::Loc("active_extenders"));
  automation::AutomationProperties::SetName(gossipRow_, pages::Loc("gossip_network"));
  Rebuild();
}

void ExtenderPanel::SetStatus(ExtenderStatusView const& status) {
  ExtenderPanelModel model = ExtenderPanelModelFor(status);
  if (model == model_) return;
  model_ = std::move(model);
  Rebuild();
}

void ExtenderPanel::Rebuild() {
  if (!built_) return;
  RebuildRings();

  // "{0} of {1}": N is the addresses carrying a live connection right now, M is
  // every usable directory entry (K4, K5). It is faint rather than muted when
  // there is nothing at all, so "0 of 0" does not read as a figure to act on.
  countText_.Text(hstring{urnw::Format("extenders_active_of_reserve", model_.activeCount,
                                       model_.reserveCount)});
  countText_.Foreground(0 < model_.reserveCount ? mutedBrush_ : faintBrush_);

  gossipBrush_.Color(ColorForTone(model_.tone));
  gossipText_.Text(pages::Loc(model_.stateKey));
  eventsText_.Text(
      hstring{urnw::Plural("gossip_events_per_minute", model_.eventCountLastMinute)});
}

void ExtenderPanel::RebuildRings() {
  ringRow_.Children().Clear();
  for (ExtenderMark const& mark : model_.activeMarks) {
    // one ring, K2's stroke, K3's colour drawn as the SDK gave it; an address
    // the SDK sent no colour for is muted rather than painted a hue this app
    // invented
    ShapeEllipse ring;
    ring.Width(kRingDiameter - kExtenderRingStroke);
    ring.Height(kRingDiameter - kExtenderRingStroke);
    ring.StrokeThickness(kExtenderRingStroke);
    ExtenderRgb rgb{};
    Brush stroke = mutedBrush_;
    if (ParseExtenderColorHex(mark.colorHex, rgb)) {
      stroke = SolidColorBrush(winrt::Windows::UI::Color{255, rgb.r, rgb.g, rgb.b});
    }
    ring.Stroke(stroke);
    ring.VerticalAlignment(VerticalAlignment::Center);
    ring.IsHitTestVisible(false);
    // the address itself, for the one reader who wants to know WHICH extender
    automation::AutomationProperties::SetName(ring, pages::H(mark.ip));
    ringRow_.Children().Append(ring);
  }
  // an empty ring row still occupies its column's spacing; collapse it so the
  // count sits where it would with no extenders at all
  ringRow_.Visibility(model_.activeMarks.empty() ? Visibility::Collapsed
                                                 : Visibility::Visible);
}

}  // namespace urnw
