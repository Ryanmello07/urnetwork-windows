// The VLESS settings sheet: one VLESS server in the ACTIVE network space, which
// the space's client strategy dials through while it is on (sdk
// vless_settings.go). Two doors open the same sheet: Settings > Connections >
// VLESS, and the VLESS button of the login screen's Change Network API sheet,
// before sign-in. Either way it edits whichever space is active.
//
// Top to bottom, as on every platform: the description, Use VLESS, the VLESS
// link with Paste link and Copy link, name, server address, port, user id, the
// transport / security / flow pickers, and the fields the transport and
// security call for. Paste link reads the clipboard into the link box and
// parses it; a link that parses replaces the whole form. Save writes the form
// through NetworkSpace::setVlessSettings: the sdk validates enabled settings
// and saves nothing when they do not validate, and its error shows under the
// form in the store's words. The tunnel runs in the service, which imports the
// space at its next tunnel start, so a save also says the VPN takes the
// settings the next time it connects.
//
// Every decision short of drawing -- the fields' visibility, the form <->
// settings mapping, the error keys, the picker options -- is
// VlessPresentation.h, which is pure and tested off-Windows. Every sdk call
// runs off the UI thread: the space calls take SdkHost's lock, which a session
// bootstrap holds for its whole length.
//
// The established sheet pattern (SettingsSheets.h): enable_shared_from_this, a
// static Create(root, ...), a ContentDialog on the brand sheet surface, weak
// captures in handlers, and the page holding the shared_ptr while it shows.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "SdkHost.h"
#include "UrComponents.h"
#include "VlessPresentation.h"

namespace urnw {

class VlessSheet : public std::enable_shared_from_this<VlessSheet> {
 public:
  static std::shared_ptr<VlessSheet> Create(winrt::Microsoft::UI::Xaml::XamlRoot const& root,
                                            SdkHost& sdk);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  explicit VlessSheet(SdkHost& sdk) : sdk_(sdk) {}
  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  // Reads the space's settings off the UI thread and fills the form. After a
  // save it is the read-back: the sdk stores the settings normalized (and a
  // server that is off and names nothing as no server at all), and the form
  // shows what was stored.
  winrt::fire_and_forget Load();
  // `form` is nullopt when the read failed.
  void ApplyLoaded(std::optional<vless::Form> const& form);
  // The form into the controls, and back.
  void ApplyForm(vless::Form const& form);
  vless::Form ReadForm() const;
  // Shows the fields the chosen transport and security call for.
  void ApplyVisibility();
  winrt::fire_and_forget PasteLink();
  winrt::fire_and_forget CopyLink();
  winrt::fire_and_forget Save();
  // Paste, copy and save each run one sdk call off the UI thread; one at a
  // time, with the three commands off meanwhile.
  void SetBusy(bool busy);
  void ShowStatus(winrt::hstring const& text, kit::ValidationState state);
  // The store's words for an sdk error id (an unknown id: an invalid link).
  void ShowError(std::string const& errorId);

  SdkHost& sdk_;
  // the form could be read, so there is something to edit and save
  bool loaded_ = false;
  bool busy_ = false;
  // set while ApplyForm fills the pickers, whose SelectionChanged it raises
  bool applyingForm_ = false;
  // what the form carries without showing it (REALITY's spiderX)
  std::string spiderX_;

  // each picker's entries, in the order its items were appended
  std::vector<vless::Option> networkOptions_;
  std::vector<vless::Option> securityOptions_;
  std::vector<vless::Option> flowOptions_;
  std::vector<vless::Option> fingerprintOptions_;

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  // around the whole form, so one IsEnabled holds it until the read lands
  winrt::Microsoft::UI::Xaml::Controls::ContentControl formHost_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch enabledToggle_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox linkBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button pasteButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button copyButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox nameBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox addressBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox portBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox idBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ComboBox networkPicker_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ComboBox securityPicker_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ComboBox flowPicker_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox serverNameBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ComboBox fingerprintPicker_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox alpnBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch insecureToggle_{nullptr};
  // the row the insecure switch sits in, which is what its rule collapses
  winrt::Microsoft::UI::Xaml::FrameworkElement insecureRow_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox publicKeyBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox shortIdBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox pathBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox hostBox_{nullptr};
  // the verdict line (loading, a parse or save error, saved, copied) and the
  // next-connect note a save adds; both sit under the scrolling form, beside
  // the dialog's Save, so a verdict is never scrolled out of sight
  winrt::Microsoft::UI::Xaml::Controls::TextBlock statusText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock nextConnectText_{nullptr};
};

}  // namespace urnw
