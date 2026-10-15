// Earnings › Top 200 › Sign fleet binding (support inbox 7934; factury
// support/plans/top200-in-app-signing.md): the sheet where this device signs
// the Top 200 fleet binding its operator built on the ur.io page.
//
//   This device   the client ID and client key (Device.getSnClientKey, read
//                 over the DeviceRemote's rpc into the service, so "" while the
//                 service is not connected), each copyable, and Copy member
//                 entry for the operator's fleet manifest
//   Binding       the box (paste, type, or pre-filled by a
//                 urnetwork://top200/sign link), checked on every edit by
//                 top200sign::Validate; a binding of this device shows its
//                 review (fleet ID, hotkey, chain and subnet, epochs)
//   Sign          only for a Valid binding, on the user's tap: the service
//                 signs it with the client key (Device.signSnFleetBinding over
//                 the existing local device rpc); the sheet then shows the
//                 {binding, client_signature} payload with Copy payload
//
// Nothing is sent over the network by this sheet, and nothing it holds is
// secret: the client key shown is the public key, the private key stays in the
// service. The binding and the payload are never logged. A link only pre-fills
// the box: signing always needs the tap after the review.
//
// Plain C++ like the other Earnings sheets: every method runs on the UI thread;
// the two device reads and the signing run on a background thread and come back
// through the dispatcher queue to a WEAK reference, so a late result after the
// sheet closed finds nothing.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "Top200Binding.h"

namespace urnw {

class Top200SignSheet : public std::enable_shared_from_this<Top200SignSheet> {
 public:
  // `prefill` goes into the box as if pasted (a link's decoded binding, or "").
  // `linkInvalid` is a link whose binding did not decode: the box starts empty
  // and shows top200_sign_error_invalid until something is typed or pasted.
  static std::shared_ptr<Top200SignSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root,
      winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue, std::string prefill,
      bool linkInvalid);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

  // A link that arrives while the sheet is open: replaces the box the same way.
  void Prefill(std::string const& binding, bool linkInvalid);

 private:
  explicit Top200SignSheet(winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue);

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  winrt::fire_and_forget LoadDevice();
  void ApplyDevice(std::string clientId, std::string clientKey);
  winrt::fire_and_forget PasteFromClipboard();
  winrt::fire_and_forget Sign();
  void ApplySigned(top200sign::Signed result, uint64_t edit);
  // Re-checks the box and renders the error line, the review, the payload and
  // the buttons from the current state. Called on every change.
  void ApplyState();
  void Copy(std::string const& text, winrt::Microsoft::UI::Xaml::Controls::Button const& button);

  winrt::Microsoft::UI::Dispatching::DispatcherQueue queue_;

  bool deviceLoaded_ = false;
  std::string clientId_;
  std::string clientKey_;
  bool linkInvalid_ = false;
  bool signing_ = false;
  // bumped on every edit of the box: a signing started before the last edit
  // shows nothing
  uint64_t edit_ = 0;
  std::string payload_;
  std::string failure_;

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock clientIdValue_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock clientKeyValue_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button copyClientIdButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button copyClientKeyButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button copyMemberButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock noKeyText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox bindingBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock errorText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel reviewPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock fleetIdValue_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock hotkeyValue_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock chainValue_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock epochsValue_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel progressRow_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel donePanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox payloadBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button copyPayloadButton_{nullptr};
};

}  // namespace urnw
