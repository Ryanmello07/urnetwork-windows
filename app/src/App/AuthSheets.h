// Sign-in sheets, as ContentDialogs (macOS Authenticate/LoginInitial parity).
// Plain C++ helpers like BalanceSheets/StatsSheets: all methods run on the UI
// thread, and the window holds the sheet's shared_ptr while it is showing.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <memory>
#include <string>

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "SdkHost.h"

namespace urnw {

// ---- Seedphrase display -----------------------------------------------------
// macOS SeedphraseDisplayView: the one and only showing of a newly minted
// seedphrase — numbered word grid, a warning that this is the only time, copy
// to clipboard, and a confirmation that gates whatever comes next.
//
// The security shape is deliberate and matches macOS:
//   * the sheet has no close button AND cancels its own Closing event for any
//     result but Primary. The missing close button is not what does it —
//     ContentDialog closes on Esc either way, and it did, orphaning a network
//     the server had already created. Confirming is the only way out because
//     the Closing handler makes it so.
//   * `onConfirmed` is what actually registers the device (SdkHost::
//     ConfirmInstantAccount). Until then no session exists, so an account
//     nobody can recover is never left signed in.
//   * the phrase lives in this object for the life of the sheet, is never
//     logged, and is never written anywhere but the clipboard the user asked
//     for — and that copy is excluded from Clipboard History and from the
//     cloud clipboard, so it does not follow the user to another machine.
class SeedphraseDisplaySheet : public std::enable_shared_from_this<SeedphraseDisplaySheet> {
 public:
  // `onCopied` raises the app's own "copied" acknowledgement (the owner has the
  // snackbar); `onConfirmed` runs when the user says they have saved it.
  static std::shared_ptr<SeedphraseDisplaySheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, std::string const& seedphrase,
      std::function<void()> onCopied, std::function<void()> onConfirmed);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  SeedphraseDisplaySheet(std::string seedphrase, std::function<void()> onCopied,
                         std::function<void()> onConfirmed)
      : seedphrase_(std::move(seedphrase)),
        onCopied_(std::move(onCopied)),
        onConfirmed_(std::move(onConfirmed)) {}

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  void CopyToClipboard();

  std::string seedphrase_;
  std::function<void()> onCopied_;
  std::function<void()> onConfirmed_;
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  // set by the primary button; the Closing handler refuses every close until
  // it is true
  bool confirmed_ = false;
};

// ---- Network server ---------------------------------------------------------
// iOS Shared/Views/NetworkServerSheet: point the client at a different network
// API — a self-hosted or forked deployment — instead of the official one, with
// optional explicit api/connect url overrides.
//
// Offered from the SIGNED-OUT screen only, as on iOS: switching servers swaps
// the LocalState and therefore the stored jwt, so it cannot be done underneath
// a live session.
//
// Its VLESS button edits the ACTIVE space's VLESS server on the VLESS sheet
// (VlessSheet.h), the one Settings opens. Only one ContentDialog shows at a
// time, so the button closes this sheet with VlessRequested() set and the
// caller opens that one in its place (LoginPage::OnChangeNetworkServer).
class NetworkServerSheet : public std::enable_shared_from_this<NetworkServerSheet> {
 public:
  static std::shared_ptr<NetworkServerSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }
  // the sheet closed through its VLESS button
  bool VlessRequested() const { return vlessRequested_; }

 private:
  explicit NetworkServerSheet(SdkHost& sdk) : sdk_(sdk) {}

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  void ApplyDerivedPlaceholders();   // preview the urls the host would produce
  void UpdateInsecureWarning();      // http:// / ws:// override -> danger line
  void Apply(std::string const& host, std::string const& apiUrl,
             std::string const& connectUrl);
  void UseDefault();
  // what THIS process calls the default network (SdkHost resolves the
  // URNETWORK_NETWORK_HOST override); never the compiled-in constant directly
  std::string DefaultHost() const;

  SdkHost& sdk_;
  SdkHost::NetworkServer current_;
  bool vlessRequested_ = false;
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox hostBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox apiBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox connectBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock insecureText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock statusText_{nullptr};
};

// The url/host normalization the sheet applies before anything is sent to the
// SDK. Free functions, ported one-for-one from iOS NetworkServerUtils.swift
// (itself a port of android's NetworkServerSelector.kt), so the three clients
// agree on what "bringyour.com", "https://api.example.com/" and "[2001:db8::1]:8080"
// each mean. Declared here so they are testable and reviewable on their own.
namespace netserver {
std::string NormalizeHost(std::string const& raw);
std::string NormalizeApiUrl(std::string const& raw);
std::string NormalizeConnectUrl(std::string const& raw);
bool HasInsecureScheme(std::string const& raw, std::string const& secureScheme);
// The SDK's own host/env -> "api."/"connect." subdomain derivation, so the sheet
// can preview accurately before anything is applied.
std::string DerivedServiceUrl(std::string const& hostName, std::string const& migrationHostName,
                              std::string const& envName, std::string const& scheme,
                              std::string const& service);
}  // namespace netserver

// ---- Account menu -----------------------------------------------------------
// iOS Shared/Views/AccountMenu.swift, as the native idiom: a MenuFlyout hung off
// the title-bar avatar. Network name, "Create account" for a guest, sign out,
// and the referral share.
//
// The referral share is iOS's ReferralShareLink. Windows has no ShareLink, and
// the honest desktop equivalent of "share this text" is the clipboard: the
// message (store key referral_share_message, with the code substituted) is
// copied and the caller raises an acknowledgement. The code is fetched when the
// menu opens — like iOS's poller, but on demand rather than every minute.
struct AccountMenuActions {
  std::function<void()> onCreateAccount;  // guests only; null hides the item
  std::function<void()> onSignOut;
  // called with the message that was put on the clipboard, so the owner can
  // raise its snackbar
  std::function<void()> onShared;
};
void ShowAccountMenu(winrt::Microsoft::UI::Xaml::FrameworkElement const& anchor, SdkHost& sdk,
                     std::string const& networkName, bool guest,
                     AccountMenuActions actions);

}  // namespace urnw
