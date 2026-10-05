// The bootstrap DNS-over-HTTPS servers of the active network space (sdk
// control_doh_ui.go): `https://<ip literal>/<path>` servers the space's own
// names (api, connect, extender) are looked up through ahead of the built-in
// ones, for a network that blocks those -- mainland China blocks all four. The
// servers see those lookups, which the description says, always on screen.
//
// Two doors show the same block. Account > Extenders shows it inline, under the
// extender settings. The login screen's Change Network API sheet opens it in a
// sheet of its own (ControlDohSheet), before sign-in: a fresh install behind
// such a network cannot resolve the api to sign in at all, so that door is the
// one it needs. Either way it edits whichever space is active, through SdkHost,
// and needs no session: the servers are a value of the app's own space.
//
// Top to bottom, as on every platform: the description, one server per line,
// the hint, "Use China resolvers" (fills the box from the sdk's preset without
// saving) with its hint, "Use built-in servers only" (clears and saves), and
// Save. A save goes through NetworkSpace::setControlDohUrls, which validates
// every line and saves nothing when one fails, its error shown in the store's
// words (an id this build does not know, or the sdk's internal_error, as
// something went wrong; only "" is a save); it applies in place, and the
// service takes the space at its next tunnel start, so a save also says the
// VPN uses the servers the next time it connects. The host draws the title:
// Account's group header, or the sheet's.
//
// Every decision short of drawing -- the box's lines, the save outcome and the
// error keys, the preset's country -- is ExtenderPresentation.h, which is pure
// and tested off-Windows. Every sdk call runs off the UI thread: the space
// calls take SdkHost's lock, which a session bootstrap holds for its whole
// length.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "ExtenderPresentation.h"
#include "SdkHost.h"
#include "UrComponents.h"

namespace urnw {

class ControlDohBlock : public std::enable_shared_from_this<ControlDohBlock> {
 public:
  // The block, built and disabled until Load() lands.
  static std::shared_ptr<ControlDohBlock> Create(SdkHost& sdk);

  // The block's root, for the host to place under its title.
  winrt::Microsoft::UI::Xaml::Controls::StackPanel Root() const { return root_; }

  // Reads the space's servers off the UI thread into the box. After a save it
  // is the read-back: the sdk stores the list normalized, repeats dropped and
  // v4 before v6, and the box shows what was stored.
  winrt::fire_and_forget Load();

  // Re-texts the block after a language change; a verdict on the status line
  // is left as it was written.
  void ApplyStrings();

 private:
  explicit ControlDohBlock(SdkHost& sdk) : sdk_(sdk) {}
  void Build();
  // `urls` is nullopt when the read failed.
  void ApplyLoaded(std::optional<std::vector<std::string>> const& urls);
  // The preset into the box, read off the UI thread; nothing is saved.
  winrt::fire_and_forget UseChinaResolvers();
  // Saves `urls` off the UI thread: the box's lines, or none for "Use built-in
  // servers only".
  winrt::fire_and_forget Save(std::vector<std::string> urls);
  // One sdk call at a time, with the box and the three commands off meanwhile.
  void SetBusy(bool busy);
  void ShowStatus(winrt::hstring const& text, kit::ValidationState state);

  SdkHost& sdk_;
  // the servers could be read, so there is something to edit and save
  bool loaded_ = false;
  bool busy_ = false;

  winrt::Microsoft::UI::Xaml::Controls::StackPanel root_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock description_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox urlsBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock hint_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button chinaButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock chinaHint_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button resetButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button saveButton_{nullptr};
  // the verdict line (loading, an error, saved) and the next-connect note a
  // save adds
  winrt::Microsoft::UI::Xaml::Controls::TextBlock statusText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock nextConnectText_{nullptr};
  // the three buttons with the store id each came from, for ApplyStrings
  std::vector<std::pair<winrt::Microsoft::UI::Xaml::Controls::Button, std::string>> buttons_;
};

// The login screen's door: the block in a sheet titled with the setting's name.
// The established sheet pattern (SettingsSheets.h): a static Create(root, ...),
// a ContentDialog on the brand sheet surface, the page holding the shared_ptr
// while it shows.
class ControlDohSheet {
 public:
  static std::shared_ptr<ControlDohSheet> Create(winrt::Microsoft::UI::Xaml::XamlRoot const& root,
                                                 SdkHost& sdk);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  ControlDohSheet() = default;

  std::shared_ptr<ControlDohBlock> block_;
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
};

}  // namespace urnw
