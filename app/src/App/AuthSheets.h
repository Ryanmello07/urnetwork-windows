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

// ---- Try guest mode -----------------------------------------------------------
// macOS GuestModeSheet: a brief explainer, the terms consent (the same
// terms/privacy links the create step uses), and one button that creates a
// throwaway guest network (SdkHost::LoginAsGuest). On success the dialog hides
// itself and the auth-state relay swaps the login panel for the home view;
// errors show inline and leave the sheet open for a retry.
class GuestModeSheet : public std::enable_shared_from_this<GuestModeSheet> {
 public:
  static std::shared_ptr<GuestModeSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  explicit GuestModeSheet(SdkHost& sdk) : sdk_(sdk) {}

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  void Submit();
  void ApplyResult(bool ok, std::string const& error);

  SdkHost& sdk_;
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::CheckBox termsCheck_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock errorText_{nullptr};
  // optional referral code: guests can be referred too (android/apple instant
  // account parity). Validated before the create; a bad code keeps the sheet
  // open rather than silently dropping the bonus.
  winrt::Microsoft::UI::Xaml::Controls::TextBox codeBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock codeStatus_{nullptr};
  bool creating_ = false;
};

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
class NetworkServerSheet : public std::enable_shared_from_this<NetworkServerSheet> {
 public:
  static std::shared_ptr<NetworkServerSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

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
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox hostBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox apiBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox connectBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock insecureText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock statusText_{nullptr};
};

// ---- SSO sheet (embedded Google / Apple sign-in) ---------------------------
// The provider's own sign-in page in a WebView2 inside the app, signed in the
// way the ur.io website does it (mmm ur.io ConnectDialog), with the answer
// handed to SdkHost::HandleDeepLink as exactly the urnetwork://oauth/<provider>
// deep link the api's callback would have delivered in the browser flow — the
// armed attempt's state check, nonce check and authLogin all run unchanged.
//
//   google -> the sheet's webview navigates to the authorize url
//     (response_mode=fragment); the provider's redirect to the registered
//     https://ur.io/auth/google/callback carries the result in the FRAGMENT,
//     which NavigationStarting sees and cancels — the ur.io page never loads,
//     and the token never leaves the machine except to the provider.
//   apple -> Apple requires response_mode=form_post the moment the name or
//     email scope is requested, and the one redirect registered on the
//     Services ID (https://ur.io) answers a POST with 405 — so the website's
//     popup transport (AppleID.auth with usePopup:true) is the only one that
//     works: a hidden driver page at the ur.io origin (served from disk
//     through a virtual host mapping, so it does not depend on the real site)
//     window.open()s the web_message authorize url into the sheet's visible
//     webview, and Apple's completion page postMessages the result to the
//     driver, which forwards it over the webview bridge (WebMessageReceived).
//
// This exists because the api-callback browser flow (SdkHost::SignInWithSso)
// is rejected by both providers' current configurations; these flows work with
// zero server changes.
class SsoSheet : public std::enable_shared_from_this<SsoSheet> {
 public:
  // `provider` is "apple" or "google"; `authorizeUrl` is the armed attempt's
  // authorize page (SdkHost::SignInWithSsoEmbedded). onCancel fires when the
  // sheet closes without an answer — closing our own sheet is a CERTAIN
  // cancel, unlike a closed browser tab — and onFailed when the webview could
  // not bring up the authorize page at all (the caller falls back to the
  // browser flow, which re-arms and supersedes this attempt).
  static std::shared_ptr<SsoSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk, std::string provider,
      std::string authorizeUrl, std::function<void()> onCancel,
      std::function<void()> onFailed);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  SsoSheet(SdkHost& sdk, std::string provider, std::string authorizeUrl,
           std::function<void()> onCancel, std::function<void()> onFailed)
      : sdk_(sdk),
        provider_(std::move(provider)),
        authorizeUrl_(std::move(authorizeUrl)),
        onCancel_(std::move(onCancel)),
        onFailed_(std::move(onFailed)) {}

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  // Kicked when the slot enters the live tree: Google navigates its webview to
  // the authorize url; Apple brings up the driver page and its popup. Async
  // because WebView2 init is (the UpgradeSheet::OpenEmbedded pattern).
  winrt::fire_and_forget OpenProviderPage();
  winrt::fire_and_forget OpenApplePopup();
  // The sign-in's answer (a fragment callback intercepted in the webview, or
  // Apple's oauthDone forwarded by the driver page): deliver the synthesized
  // deep link and close. Runs deferred, on the UI thread.
  void HandleReturn(std::string const& deepLink);
  void HandleAppleMessage(std::string const& json);
  // The webview failed before the provider's page rendered: nothing was shown,
  // so no sign-in can be lost by switching — close and let the caller's
  // browser flow try.
  void Fail();
  void TeardownWebView();

  SdkHost& sdk_;
  std::string provider_;
  std::string authorizeUrl_;
  std::function<void()> onCancel_;
  std::function<void()> onFailed_;
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  // the webview (created once the slot loads) sits under a loading ring
  winrt::Microsoft::UI::Xaml::Controls::Grid webviewSlot_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::WebView2 webview_{nullptr};
  // apple only: the hidden ur.io-origin page that window.open()s the authorize
  // url and receives the result's web message (the website's SDK opener)
  winrt::Microsoft::UI::Xaml::Controls::WebView2 driverWebview_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ProgressRing ring_{nullptr};
  bool pageLoaded_ = false;  // the provider's page rendered at least once
  bool completed_ = false;   // the sign-in's answer went to HandleDeepLink
  bool failed_ = false;      // onFailed already ran (suppresses onCancel)
  bool closed_ = false;      // the dialog was dismissed; drop in-flight init
  uint32_t webviewGeneration_ = 0;  // drops async results of a torn-down webview
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
