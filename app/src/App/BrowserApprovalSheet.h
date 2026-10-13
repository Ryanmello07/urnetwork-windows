// The "Sign in with browser" sheet: one ContentDialog whose pages carry a sign-in the
// person approves on a web page (UrIoApproval.h), and the paste of a one-time auth code
// that this sheet has always held for when the browser cannot be used. A second dialog
// cannot open over the first, so every page is this one's.
//
// Pages:
//   Start      "Continue in browser" with the host it goes to beside it, and the paste field.
//              Without the approval (AppController::ApprovalOffered says no) the button opens
//              the site to sign in at and the field is the whole of the flow, as it was.
//   Waiting    the browser was sent to the page and the app waits for its answer. If the
//              launcher said no, the person is told, Copy puts the link on the clipboard to open
//              by hand, and the attempt stays. The link is never shown as text.
//   SigningIn  the page's answer is being redeemed, or the person's yes is being registered
//   Confirm    "Sign in" to the network the answer would join, by its name, before anything
//              is registered or stored
//   TimedOut   the attempt's five minutes are up
//   Failed     the answer could not be used, or the registration was refused
//
// What the sheet holds, and does not:
//   * The attempt is AppController's, not the sheet's. A hide to the tray closes every sheet
//     (MainWindow::CloseSheetsForHide) and the browser may still answer, so the sheet takes
//     AppController::WindowHides when it is built and asks again when it closes: the person
//     closing it cancels the attempt, a hide keeps it, and LoginPage::OnApprovedCode opens the
//     sheet again on the answer. The count is moved by the hide itself, so the answer does not
//     depend on when the dialog's Closed event arrives (after its popup's closing transition,
//     which a hidden window may not run until it is shown), and a sheet that a hide has swept
//     is no longer Open() even before it has closed.
//   * The one-time code is passed through (BeginRedeem) and never kept by the sheet (the
//     page that hosts it holds it for up to two seconds while another sheet closes:
//     LoginPage::ContinueApprovedCode). The credential it redeems is held by SdkHost until
//     the person confirms, and is discarded if the sheet closes on the Confirm page, or
//     while the redeem was out.
//   * The request link is a secret until it is opened. It is kept only while the Waiting page
//     is up, never shown as text (any program of the session can read a window's text through
//     UI Automation), and copied only with the clipboard's history and roaming off.
//   * There is no check code: the contract reserves one and this app neither computes nor
//     shows it.
//
// All methods run on the UI thread. The page that hosts the sheet holds its shared_ptr while
// it shows, under the single-sheet gate (LoginPage::ShowApprovalSheet).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "SdkHost.h"

namespace urnw {

class BrowserApprovalSheet : public std::enable_shared_from_this<BrowserApprovalSheet> {
 public:
  enum class Page { Start, Waiting, SigningIn, Confirm, TimedOut, Failed };

  // Asks AppController once, as the sheet is built, whether the approval is offered.
  static std::shared_ptr<BrowserApprovalSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

  // False once the sheet has closed, or once a hide to the tray has swept it (it is closing,
  // although its Closed event has not come): whatever is handed to it then is dropped, and
  // LoginPage::OnApprovedCode opens a sheet of its own for an answer instead of spending a
  // one-time code into one that is going away.
  bool Open() const;
  // The page's approved answer, already opened and consumed by the gate: redeem it, then ask
  // the person to confirm which network it would join. Takes the code over and wipes it.
  void BeginRedeem(std::string code);
  // The attempt's five minutes are up (a sheet that is waiting says so).
  void ShowTimedOut();
  // The person cancelled in the browser: back to the start, with nothing signed in.
  void ShowDenied();
  // The code pasted into the fallback field, when Sign in closed the sheet; "" otherwise.
  // The page that hosts the sheet signs in with it once the sheet is gone.
  std::string TakePastedCode();

 private:
  BrowserApprovalSheet() = default;

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  void BuildStart();
  void BuildWaiting();
  void BuildSigningIn();
  void BuildConfirm();
  void BuildMessage();

  void ShowPage(Page page);
  void ShowStartNote(winrt::hstring const& message);
  void ShowFailed(winrt::hstring const& message);
  void ShowConfirm(std::string const& networkName);

  // The one button of the Start page and of the Waiting page's "again".
  void OnAction();
  // The attempt: BeginApproval, the request, the browser. A second click while the attempt is
  // live, and its request young enough for the page, opens the same request again.
  winrt::fire_and_forget StartApproval();
  // The paste flow's button: the site to sign in at, in the browser, with the launcher's answer
  // observed.
  winrt::fire_and_forget OpenManualSignIn();
  void CopyLink();

  void OnPrimary();
  void OnRedeemed(SdkHost::ApprovedNetwork answer);
  void Confirm();
  void OnConfirmed(AuthResult const& result);
  void SubmitPastedCode();
  void OnClosed();
  // Closes the sheet from inside; nothing outside hides it but the hide-to-tray sweep.
  void Finish();

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel startPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel waitingPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel signingInPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel confirmPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel messagePanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button actionButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button againButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button copyButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::PasswordBox pasteBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock startNote_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock launchError_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock networkText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock messageText_{nullptr};

  Page page_ = Page::Start;
  // AppController said the approval is offered: the Start page leads with it
  bool approvalOffered_ = false;
  // the dialog has closed: drop every answer that is still on its way
  bool closed_ = false;
  // AppController::WindowHides when this sheet was built: a hide to the tray since then
  // is what swept it, and not the person closing it
  std::uint64_t hides_ = 0;
  // the person said yes on the Confirm page: the credential has left the hold, so closing the
  // sheet from here discards nothing
  bool confirmed_ = false;
  // the browser is being asked, so a second click waits
  bool starting_ = false;
  // the request, while the Waiting page is up
  std::string link_;
  // the code Sign in closed the sheet on
  std::string pasted_;
};

}  // namespace urnw
