// The connection region picker (ConnectionRegion.h): where the user says they
// connect from, which only picks how the app reaches URnetwork, never the
// tunnel's DNS. One block, three doors, one stored value:
//  - the DNS editor shows it inline, at the top, over the "Traffic DNS"
//    controls (StatsSheets.cpp DnsEditorSheet);
//  - the login screen's optional "Connecting from" control opens it in a sheet
//    of its own (ConnectionRegionSheet), before sign-in;
//  - the "Can't reach URnetwork" prompt opens that same sheet, on the login
//    screen or signed in.
// A selection saves at once through SdkHost (NetworkSpace::setIngressCountryCode)
// and applies from the next lookup; nothing ever selects a region by itself.
// Under the list: the resolvers the selection uses (sdk ingressDohUrls /
// ingressPlainDnsServers), or "Default resolvers".
//
// Every sdk call runs off the UI thread: the space calls take SdkHost's lock.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "ConnectionRegion.h"
#include "SdkHost.h"

namespace urnw {

// The platform's localized name for a lower-case ISO code
// (Windows.Globalization.GeographicRegion); "" when it has none.
std::wstring RegionDisplayName(const std::string& code);

// The label a stored value shows as: its localized name, or Other / Undisclosed.
winrt::hstring RegionLabel(const std::string& stored);

class ConnectionRegionBlock : public std::enable_shared_from_this<ConnectionRegionBlock> {
 public:
  // `onSaved` runs on the UI thread with the stored value after each save.
  static std::shared_ptr<ConnectionRegionBlock> Create(
      SdkHost& sdk, std::function<void(std::string const&)> onSaved = {});

  winrt::Microsoft::UI::Xaml::Controls::StackPanel Root() const { return root_; }

  // Reads the regions and the stored value off the UI thread into the list.
  winrt::fire_and_forget Load();

 private:
  explicit ConnectionRegionBlock(SdkHost& sdk) : sdk_(sdk), selection_(sdk) {}
  void Build();
  void ApplyLoaded(std::vector<connection_region::Row> rows, std::optional<std::string> stored);
  winrt::fire_and_forget Save(connection_region::Row row);
  winrt::fire_and_forget ShowResolvers(std::string code);

  SdkHost& sdk_;
  connection_region::RegionSelection<SdkHost> selection_;
  std::function<void(std::string const&)> onSaved_;
  std::vector<connection_region::Row> rows_;
  bool updating_ = false;

  winrt::Microsoft::UI::Xaml::Controls::StackPanel root_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ComboBox picker_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock resolvers_{nullptr};
};

// The login screen's and the prompt's door: the block in a sheet titled
// "Where are you connecting from?".
class ConnectionRegionSheet {
 public:
  static std::shared_ptr<ConnectionRegionSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk,
      std::function<void(std::string const&)> onSaved = {});

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  ConnectionRegionSheet() = default;

  std::shared_ptr<ConnectionRegionBlock> block_;
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
};

}  // namespace urnw
