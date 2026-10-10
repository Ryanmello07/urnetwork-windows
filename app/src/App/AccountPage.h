// The Account destination: profile (network name + auth + password), the
// Sessions row that opens the Sessions page (SessionsPage), the redeemed
// balance-code list, and the Referrals row that opens the "Refer and earn"
// page (ReferralsPage). macOS AccountRootView and ProfileView parity.
//
// The plan + usage card that sits above these is NOT here: it is written by the
// SubscriptionBalanceStore relay in MainWindow, which paints the account panel
// and the connect drawer from one snapshot.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "ControlDohSettings.h"  // the bootstrap DNS-over-HTTPS servers block
#include "ExtenderSheets.h"   // the share + import sheets (EXTENDER.md K7)
#include "ReferralTotalsState.h"
#include "SettingsSheets.h"  // rows::FieldState + the row kit
#include "UrComponents.h"
#include "VerifySendNotice.h"

namespace winrt::URnetwork::implementation {
struct MainWindow;
}

namespace urnw {

class AccountPage {
 public:
  explicit AccountPage(winrt::URnetwork::implementation::MainWindow& window);

  void ApplyStrings();

  void LoadAccount();
  void LoadReferralInfo();   // referral code + totals (pane A's usage-bar rows)
  void RetryReferralInfo();  // the referral rows' Try again: back to Loading, read again
  void LoadBalanceCodes();   // redeemed-codes list (account panel)
  // Pane D (EXTENDER.md K6). Reads the effective settings through the SDK's
  // ExtenderViewController and the legacy private extender off the network
  // space -- BOTH off the UI thread, because the controller lives on the
  // DeviceRemote and its read is an rpc to the service. Safe to call with no
  // session: it renders the NoDevice state. The bootstrap DNS-over-HTTPS
  // servers are read too, session or not (ControlDohSettings.h).
  winrt::fire_and_forget LoadExtenderSettings();

  // read by MainWindow::ApplyBalance for the "Total Referrals" / bonus rows on
  // both plan cards: the count, or Loading / an error until a read lands
  ReferralTotalsFetch const& referralTotals() const { return referralTotals_; }

  void OnSaveNetworkName(winrt::Windows::Foundation::IInspectable const&,
                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // R4: the profile name's explicit edit mode (spec, Profile). The row shows the
  // saved name; the pencil opens a field; Save and Cancel exist only while it is
  // open, and Cancel restores the saved name rather than whatever was typed.
  void OnEditNetworkName(winrt::Windows::Foundation::IInspectable const&,
                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnCancelNetworkName(winrt::Windows::Foundation::IInspectable const&,
                           winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

  // The redeemed-code table and every state it can be in, in one place: three
  // scattered writes to a panel and an empty line used to disagree about which
  // was showing. Called by LoadBalanceCodes, ApplyStrings and ResetForSignOut.
  void RenderBalanceCodes(urnet::RedeemedBalanceCodeList const& codes,
                          rows::FieldState state);

  // Keyboard focus onto the Sessions row, as the Sessions page closes: back
  // where the user left the Account list.
  void FocusSessionsNav();

  // Drop the signed-out account's identity. userAuth_ is the dangerous one:
  // SendPasswordReset mails a link to it, so a leftover value mails the
  // PREVIOUS account's owner. needsNameClaim_ would likewise pick the previous
  // account's branch on the next save.
  void ResetForSignOut();

 private:
  // The controls the markup cannot carry: the name status line and the
  // password-reset affordance, built into the AccountProfileExtra host panel.
  void BuildProfileExtra();
  // The Referrals row (pane B) that opens the Refer and earn page.
  void BuildReferralsNav();
  // The Sessions row (pane B), right after the profile, that opens the
  // Sessions page (SessionsPage.h).
  void BuildSessionsNav();
  // ---- pane D: extenders (EXTENDER.md K6, K7) ------------------------------
  // The whole section is built here rather than in markup, for the reason
  // SettingsSheets.h gives about the settings sections: MainWindow.xaml is the
  // file every parallel phase touches and a uniform stack of label/field rows
  // is more compact as a builder than as markup.
  void BuildExtenderPane();
  void ApplyExtenderForm(rows::FieldState state, ExtenderSettingsForm const& form,
                         std::string const& privateIp, std::string const& privateSecret,
                         bool hasController);
  // The three edited values, through the view controller: an empty box means
  // the derived default, and the SDK restarts the space's network client and
  // node in place.
  winrt::fire_and_forget SaveExtenderSettings();
  // The legacy single private extender, which is a network-space VALUE rather
  // than a view-controller setting. A change confined to the extender values
  // is applied in place by the space manager, so this does not disturb the
  // live session; the service reads it at its next start, which is what the
  // note under the fields says.
  winrt::fire_and_forget SavePrivateExtender();
  // Re-text pane D's built labels and buttons after a language change.
  // BuildExtenderPane is one-shot, so unlike the other two builders on this
  // page it cannot simply be re-run.
  void ApplyExtenderStrings();
  winrt::fire_and_forget ShowExtenderShareSheet();
  winrt::fire_and_forget ShowExtenderImportSheet();
  // "Reset extenders" (connect EXTENDER.md E7): the confirmation, then the
  // reset itself off the UI thread (SdkHost::ResetExtenders: the app's own
  // space, then the service's by its control verb), which reloads the form it
  // leaves before it says "Extenders reset". Needs no session.
  winrt::fire_and_forget ConfirmResetExtenders();
  winrt::fire_and_forget ResetExtenders();
  void SendPasswordReset();
  // Counts a password reset rate limit down and turns Send back on once the
  // retry time has passed (resetRateLimitTimer_ tick).
  void RefreshResetRateLimit();
  // Every async field on this surface reaches one of these, for the same reason
  // the settings page does: before it, a 401 and an empty account looked
  // identical (the redeemed-codes list rendered "No balance codes found" for
  // both).
  void ApplyAccountState(rows::FieldState state);

  winrt::URnetwork::implementation::MainWindow& w_;

  ReferralTotalsFetch referralTotals_;
  std::string referralCode_;
  // the auth this account signs in with, needed by the password-reset call
  std::string userAuth_;
  // apple AccountNavStackView: a name must be CLAIMED (no reclaim cooldown on
  // the auto-generated one) rather than CHANGED (24h cooldown protects the old
  // name) exactly when the account carries none of these identity methods.
  // Seedphrase deliberately does not count - a seedphrase-only account still
  // has an auto-generated name to claim.
  bool needsNameClaim_ = false;

  void SetEditingName(bool editing);
  // The auth line and its ROW: an empty fixed-height row is still a hole.
  void SetAuthText(winrt::hstring const& text);
  // Write the saved name into the view row AND keep the copy the editor seeds
  // from. The TextBox is never the source of truth for the name.
  void ApplyNetworkName(std::string const& name);

  winrt::Microsoft::UI::Xaml::Controls::TextBlock nameStatus_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button changePasswordButton_{nullptr};
  bool built_ = false;
  bool referralsNavBuilt_ = false;
  winrt::Microsoft::UI::Xaml::Controls::TextBlock referralsNavValue_{nullptr};
  bool sessionsNavBuilt_ = false;
  winrt::Microsoft::UI::Xaml::Controls::Button sessionsNavButton_{nullptr};
  // one-shot: the "nothing has been requested yet" states, applied by
  // ApplyStrings so a load is not needed to make the card readable
  bool initialStatesApplied_ = false;
  bool savingName_ = false;
  bool editingName_ = false;
  // the last name the SERVER acknowledged; what the editor seeds from and what
  // Cancel restores
  std::string networkName_;
  bool sendingReset_ = false;
  // after the server refused a reset link for too many attempts: Send stays
  // off until the retry time, with a 1s tick counting the status line down
  urnw::ResendCooldown resetRateLimit_;
  winrt::hstring resetRateLimitText_;
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer resetRateLimitTimer_{nullptr};

  // ---- pane D: extenders ---------------------------------------------------
  bool extenderBuilt_ = false;
  // a save or a reset of pane D is running; the pane's other writes wait
  bool savingExtender_ = false;
  bool advancedOpen_ = false;
  winrt::Microsoft::UI::Xaml::Controls::TextBox extenderDnsBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox extenderGossipBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox extenderHostsBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox privateIpBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox privateSecretBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button extenderSaveButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button privateSaveButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button shareExtendersButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button importExtendersButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button resetExtendersButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button advancedButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel advancedPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock extenderStatus_{nullptr};
  // The standing fact about this platform, not a per-save message: the tunnel
  // runs in the service process, which took this space's values at its last
  // start and reads the new ones at its next one.
  winrt::Microsoft::UI::Xaml::Controls::TextBlock extenderNote_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Border extenderNoteRow_{nullptr};
  // Every built label and button in pane D with the store id it came from, so
  // ApplyExtenderStrings can re-text them without rebuilding the pane.
  std::vector<std::pair<winrt::Microsoft::UI::Xaml::Controls::TextBlock, std::string>>
      extenderLabels_;
  std::vector<std::pair<winrt::Microsoft::UI::Xaml::Controls::Button, std::string>>
      extenderButtons_;
  // held for as long as its dialog is showing, like every other sheet here
  std::shared_ptr<urnw::ExtenderShareSheet> extenderShareSheet_;
  std::shared_ptr<urnw::ExtenderImportSheet> extenderImportSheet_;
  // The bootstrap DNS-over-HTTPS servers, under the extender settings. Not
  // gated on the view controller like the rows above it: the servers are a
  // value of the app's own space, read and saved through SdkHost, and a user
  // whose network blocks the built-in servers needs them before the app can
  // reach its own servers, let alone hold a session.
  std::shared_ptr<urnw::ControlDohBlock> controlDoh_;
};

}  // namespace urnw
