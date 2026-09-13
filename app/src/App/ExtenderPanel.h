// The extender panel (connect/EXTENDER.md K4): in the connect drawer, directly
// under the IP-family histogram. One hollow ring per extender carrying a live
// connection right now, in that extender's colour; the count as "N of M" where
// M is every usable directory entry; and the gossip network's status dot --
// green connected, yellow connecting, red disconnected -- with its state word
// and the records and revocations the network delivered in the trailing 60 s.
//
// Tapping does nothing. K4 says so explicitly ("there is no details panel"), so
// this is a plain pane row rather than a Button, exactly like the IP-family
// histogram beside it and unlike the transport bar above it, which does open
// something.
//
// Every decision is ExtenderPresentation.h (pure, tested off-Windows) and every
// number in the rings is ExtenderRingGeometry.h (the same file the hero's dots
// use); this class converts, keeps the last model so an unchanged push costs
// nothing, and draws. Built into a host Grid like TransportBar and
// IpFamilyHistogram. UI thread only.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include "ExtenderPresentation.h"

namespace urnw {

class ExtenderPanel {
 public:
  // `host` receives the whole component.
  explicit ExtenderPanel(winrt::Microsoft::UI::Xaml::Controls::Grid const& host);

  // The status as SdkHost mapped it, from the device's once-a-second change
  // listener. A default-constructed view is "no session": a red dot, 0 of 0,
  // no rings -- which is the truth, not an empty state to hide.
  void SetStatus(ExtenderStatusView const& status);

  // Re-render the fixed labels after a language change.
  void ApplyStrings();

  // the current model, for the page's own bookkeeping and the preview build
  const ExtenderPanelModel& Model() const { return model_; }

 private:
  void BuildVisuals(winrt::Microsoft::UI::Xaml::Controls::Grid const& host);
  void Rebuild();
  void RebuildRings();

  ExtenderPanelModel model_;
  bool built_ = false;

  winrt::Microsoft::UI::Xaml::Controls::StackPanel root_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock title_{nullptr};
  // one hollow ring per active extender, left to right in status order
  winrt::Microsoft::UI::Xaml::Controls::StackPanel ringRow_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock countText_{nullptr};
  // the gossip group: the dot, its state word, and the event rate
  winrt::Microsoft::UI::Xaml::Controls::StackPanel gossipRow_{nullptr};
  winrt::Microsoft::UI::Xaml::Shapes::Ellipse gossipDot_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::SolidColorBrush gossipBrush_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock gossipText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock eventsText_{nullptr};

  winrt::Microsoft::UI::Xaml::Media::Brush mutedBrush_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::Brush faintBrush_{nullptr};
};

}  // namespace urnw
