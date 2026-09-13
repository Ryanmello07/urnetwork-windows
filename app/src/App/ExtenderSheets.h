// The two extender sheets the account section opens (connect/EXTENDER.md K7,
// K8): SHARE, which renders this space's addresses as a QR code with the
// connector glyph at its centre and the payload as copyable text, and IMPORT,
// which reads a code back out of a chosen image file or out of pasted text.
//
// There is NO CAMERA path here, and that is K8, not an omission: windows and
// linux "render through a vendored single-file encoder, import reads an image
// file through zxing-cpp plus pasted text, no camera".
//
// Encoding, decoding and applying all live in the SDK's ExtenderViewController
// -- one implementation of the payload rules for every app. These two classes
// only draw what it returns and ask it to act; every decision between those two
// things is ExtenderPresentation.h, which is pure and tested off-Windows.
//
// Both follow the established sheet pattern (SettingsSheets.h): enable_shared_
// from_this, a static Create(root, ...), a ContentDialog on the brand sheet
// surface, weak captures in handlers, and the page holding the shared_ptr for
// as long as the dialog shows.
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
#include <winrt/Microsoft.UI.Xaml.Media.h>

#include "ExtenderPresentation.h"
#include "SdkHost.h"

namespace urnw {

// ---- share (K7) -------------------------------------------------------------
//
// "The share payload is `ur-ext:1:` followed by base64url of ExtenderShare{...}
// ... The QR renders at error level H with the black and white connector glyph
// centered and a 4 px outline of the connector shape around it; the share
// screen also shows the payload as copyable text."
class ExtenderShareSheet : public std::enable_shared_from_this<ExtenderShareSheet> {
 public:
  static std::shared_ptr<ExtenderShareSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  explicit ExtenderShareSheet(SdkHost& sdk) : sdk_(sdk) {}
  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  // Ask the SDK for the payload at the current toggle state and draw it. The
  // controller is a DeviceRemote object, so the call is an rpc to the service
  // and runs off the UI thread like every other one in these sheets.
  winrt::fire_and_forget Rebuild();
  void ApplyShare(std::string const& text, std::int64_t count, bool failed);
  // Draw `text` as a level-H code inside the fixed canvas, glyph and outline
  // included. An empty text clears the canvas.
  void RenderCode(std::string const& text);

  SdkHost& sdk_;
  std::string text_;
  bool includeSettings_ = false;  // K7: off by default

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Canvas canvas_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Border codeFrame_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock countText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock payloadText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock statusText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch settingsToggle_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button copyButton_{nullptr};
};

// ---- import (K7, K8) ---------------------------------------------------------
//
// A chosen image file (decoded with zxing-cpp) or pasted text. On a decoded
// payload: the count, the "use extender settings" toggle when the code carries
// settings, the foreign-host line when it belongs to another operator, and a
// confirmation before any import that would replace the operator settings.
class ExtenderImportSheet : public std::enable_shared_from_this<ExtenderImportSheet> {
 public:
  // `onImported` runs on the UI thread after a successful import, so the
  // account section can re-read the settings the import may have changed.
  // `owner` is the window handle a desktop FileOpenPicker has to be
  // initialised with; without it PickSingleFileAsync throws E_ACCESSDENIED
  // rather than opening (SettingsPage::SaveLogsToFile found this the hard way).
  static std::shared_ptr<ExtenderImportSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, HWND owner, SdkHost& sdk,
      std::function<void()> onImported);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  ExtenderImportSheet(HWND owner, SdkHost& sdk, std::function<void()> onImported)
      : owner_(owner), sdk_(sdk), onImported_(std::move(onImported)) {}
  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  // Pick an image, decode a QR out of it, and put the payload in the box.
  // Off the UI thread from the file read on: a photo is megabytes and the
  // decode is a real scan.
  winrt::fire_and_forget ChooseImage();
  // Hand `text` to the SDK's decoder and apply the decision.
  void Decode(std::string const& text);
  void ApplyDecision();
  winrt::fire_and_forget Import();
  void ApplyImportResult(ExtenderImportResultView const& result);
  void ShowMessage(winrt::hstring const& message, bool danger);

  HWND owner_{};
  SdkHost& sdk_;
  std::function<void()> onImported_;

  std::string text_;
  ExtenderShareDecodeView decoded_;
  bool useSettings_ = false;
  // K7's confirmation: the first press of Import on a payload that would
  // replace the operator settings arms, the second commits. Two presses on ONE
  // control rather than a second dialog, which is the pattern the referral
  // unlink already uses here.
  bool confirmArmed_ = false;
  bool busy_ = false;

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button chooseButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox pasteBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock countText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch settingsToggle_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock foreignText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock statusText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button importButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ProgressRing ring_{nullptr};
};

}  // namespace urnw
