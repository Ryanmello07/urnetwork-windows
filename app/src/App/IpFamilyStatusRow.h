// The IP-family status row (connect/IPV6.md D2): the window's providers by
// address family as one row of three columns, Dualstack / IPv4 / IPv6, directly
// under the transport distribution bar in the activity pane, so "which carrier
// moved the bytes" is followed by "which families the exits can carry, and how
// well". Each column is its label in the pixel face over its status: how many
// providers of that family are connected and how many are still connecting,
// each line only when its count is not zero, or "disconnected" alone. The
// best-ranked family with a connected provider is bright (shared on a tie),
// the other live columns are muted, and an empty column is faint; the whole
// column takes its tier, so the row reads as three units. The row always keeps
// the height of a label and two status lines, top aligned, so it never
// reflows as lines come and go.
//
// All the counting and ranking is IpFamilyStatus.h (pure, tested
// off-Windows); this class converts the SDK's grid points, keeps the last
// statuses so an unchanged push costs nothing, and draws. Built into a host
// Grid like TransportBar and ExtenderPanel; no per-frame path — the row
// changes only when the window does. UI thread only.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>

#include "IpFamilyStatus.h"
#include "Sdk.h"

namespace urnw {

// The localized tag for a provider-row family token ("both" / "v4" / "v6"):
// "Both" is a word and comes from the store; v4 / v6 are protocol names and are
// not translated. Shared with the provider-locations rows.
winrt::hstring IpFamilyLabelText(std::string const& token);

class IpFamilyStatusRow {
 public:
  // `host` receives the whole component.
  explicit IpFamilyStatusRow(winrt::Microsoft::UI::Xaml::Controls::Grid const& host);

  // Replace the grid (from LiveStats::gridPoints, every stats push). Rebuilds
  // only when a column's counts actually changed.
  void SetGrid(std::vector<urnet::ProviderGridPoint> const& points);

  // Re-render the labels and the status lines after a language change.
  void ApplyStrings();

  // The current columns, for the page's own bookkeeping and the preview build.
  const std::vector<IpFamilyColumnStatus>& Statuses() const { return statuses_; }

 private:
  struct Column {
    IpFamilyColumn column = IpFamilyColumn::Dualstack;
    winrt::Microsoft::UI::Xaml::Controls::StackPanel panel{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock label{nullptr};
    // the two status slots, in line order (connected then connecting, or
    // disconnected alone in the first); a slot a column does not use is
    // collapsed
    winrt::Microsoft::UI::Xaml::Controls::TextBlock line0{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock line1{nullptr};
  };

  void BuildVisuals(winrt::Microsoft::UI::Xaml::Controls::Grid const& host);
  Column BuildColumn(IpFamilyColumn column) const;
  void Rebuild();
  winrt::hstring LabelText(IpFamilyColumn column) const;
  winrt::hstring LineText(IpFamilyStatusLine const& line) const;
  winrt::Microsoft::UI::Xaml::Media::Brush BrushFor(IpFamilyTier tier) const;

  std::vector<IpFamilyColumnStatus> statuses_;
  bool built_ = false;

  winrt::Microsoft::UI::Xaml::Controls::Grid root_{nullptr};
  std::vector<Column> columns_;
  winrt::Microsoft::UI::Xaml::Media::FontFamily labelFont_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::Brush textBrush_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::Brush mutedBrush_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::Brush faintBrush_{nullptr};
};

}  // namespace urnw
