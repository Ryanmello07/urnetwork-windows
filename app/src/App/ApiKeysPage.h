// The Account > API keys page (ApiKeysView in MainWindow.xaml): the network's
// API keys, creating one and deleting one (owner decision 2026-10-29: the
// screen on ur.io, apple, android, windows and linux). Reached from the
// Account list's API keys row, right after Sessions, and shown in Account's
// place as the Sessions page is; "‹ Account" and any rail navigation close it.
//
// The page calls the in-process Api, as the Sessions page opens its
// controller there: listApiKeys when it opens, on Refresh and Try again and
// after a create or a delete; createApiKey from the create sheet; and
// deleteApiKey after a confirmation that names the key. Each answers once, on
// an sdk thread, where ApiKeysPresentation.h's readers copy it into plain
// values; the value is marshalled to the UI thread through the
// DispatcherQueue, resolved through the window's weak reference, and dropped
// once the page is destroyed (alive_) or the open it went out under has closed
// (the model's fence). Everything else here runs on the UI thread, and no call
// it makes into the sdk blocks: each request runs on the sdk's own goroutine.
// ApiKeysPresentation.h decides everything before the XAML.
//
// The created key is shown once, in the create sheet (ApiKeyCreateSheet), and
// held only by that sheet's CreateFlow, which wipes it when the sheet closes
// by any route. Its copy is kept out of Clipboard History and off the cloud
// clipboard, and it is never logged or written anywhere.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "ApiKeysPresentation.h"

namespace winrt::URnetwork::implementation {
struct MainWindow;
}

namespace urnw {

// The create sheet: a name, then the key, once (ApiKeysPresentation.h
// CreateFlow). A ContentDialog on the brand sheet surface in the sheets'
// pattern (SettingsSheets.h): a static Create, weak captures in its handlers,
// and the page holding the shared_ptr while it shows.
//
// While the request is out the sheet cannot be closed (its close button is
// hidden and Esc is refused), so the key that comes back is the key shown; a
// sign-out or the page's teardown still closes it (Dismiss). When it closes,
// by any route, the key is wiped and taken off the screen.
class ApiKeyCreateSheet : public std::enable_shared_from_this<ApiKeyCreateSheet> {
 public:
  // `onCreated` runs on the UI thread once a key was created: the page's
  // reload, so the new key is listed.
  static std::shared_ptr<ApiKeyCreateSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, std::function<void()> onCreated);
  ~ApiKeyCreateSheet();

  ApiKeyCreateSheet(ApiKeyCreateSheet const&) = delete;
  ApiKeyCreateSheet& operator=(ApiKeyCreateSheet const&) = delete;

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }
  // A sign-out, or the page going away: the sheet closes whatever its step,
  // and the key goes with it.
  void Dismiss();

 private:
  explicit ApiKeyCreateSheet(std::function<void()> onCreated)
      : onCreated_(std::move(onCreated)) {}
  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  // the step's controls and the dialog's buttons
  void Render();
  // Create pressed
  void Submit();
  // the answer, on the UI thread
  void Apply(apikeys::CreateOutcome& outcome);
  // the key onto the clipboard, out of its history and off the cloud
  void Copy();
  // the copy button's word: "Copied" after a copy, else "Copy key"
  void ShowCopied(bool copied);
  // the sheet closed: the key wiped and off the screen
  void Clear();

  std::function<void()> onCreated_;
  apikeys::CreateFlow flow_;
  // a forced close: a sign-out or the page's teardown
  bool dismissed_ = false;
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel namePanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox nameBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ProgressRing ring_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock failedText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel createdPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock keyText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button copyButton_{nullptr};
  // the copy button's word: "Copied" for a moment after a copy, then "Copy key"
  winrt::Microsoft::UI::Xaml::Controls::TextBlock copyLabel_{nullptr};
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer copiedTimer_{nullptr};
};

class ApiKeysPage {
 public:
  explicit ApiKeysPage(winrt::URnetwork::implementation::MainWindow& window);
  // a create sheet still up closes, and its key goes; an answer still queued
  // for the page is dropped
  ~ApiKeysPage();

  ApiKeysPage(ApiKeysPage const&) = delete;
  ApiKeysPage& operator=(ApiKeysPage const&) = delete;

  // the pane title, the back affordance, Refresh, the intro and Create; the
  // rows' words follow
  void ApplyStrings();
  // MainWindow::OpenApiKeys: the page shows and loads the list. With no
  // session (signed out, --preview-ui) it asks nothing and says so.
  void Open();
  // MainWindow::CloseApiKeys, a rail navigation, a sign-out: every answer still
  // out is dropped, and the list goes.
  void Close();
  // a sign-out: nothing of the departed account stays, a create sheet up
  // closes and its key goes
  void ResetForSignOut();

 private:
  // the body's controls a rebuild puts keyboard focus back on
  enum class FocusKind { None, Delete, TryAgain };
  struct FocusTarget {
    FocusKind kind = FocusKind::None;
    std::string keyId;
  };
  // one row's control, by the key it deletes
  struct RowControls {
    std::string keyId;
    winrt::Microsoft::UI::Xaml::Controls::Button del{nullptr};
  };

  void Build();  // idempotent: the header's handlers, the intro and Create
  bool HasSession() const;
  // listApiKeys, for the open page
  void Load();
  // a load's answer, from the open `generation`, on the UI thread
  void ApplyList(uint64_t generation, uint64_t load, apikeys::ListOutcome outcome);
  // the header's indicator always; the body when what it shows changed (or
  // `force`), since a rebuild moves keyboard focus
  void Render(bool force);
  void RenderBody(apikeys::View const& view);
  void AppendRow(winrt::Microsoft::UI::Xaml::Controls::Panel const& host,
                 apikeys::Row const& row);
  FocusTarget FocusedTarget() const;
  void RestoreFocus(FocusTarget const& target);
  // the header's Refresh and the failed load's Try again
  void Refresh();
  winrt::fire_and_forget ShowCreateSheet();
  // a key was created through the sheet the open `generation` showed
  void OnCreated(uint64_t generation);
  // every delete asks first, naming the key; Cancel is the default
  winrt::fire_and_forget ConfirmDelete(std::string keyId);
  // the confirmed delete, for the list the dialog opened over
  void Delete(std::string const& keyId, uint64_t generation);
  void ApplyDelete(uint64_t generation, std::string const& keyId,
                   apikeys::DeleteOutcome const& outcome);

  winrt::URnetwork::implementation::MainWindow& w_;
  bool built_ = false;
  // the page is up in Account's place
  bool open_ = false;
  // it opened with a session to ask for the list
  bool session_ = false;

  // the list, the requests out, and the fence against an earlier open's answers
  apikeys::Model model_;
  // the body on screen
  std::optional<apikeys::View> rendered_;
  // false once the page is destroyed: a queued answer stops here
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  // the create sheet while it shows
  std::shared_ptr<ApiKeyCreateSheet> createSheet_;

  std::vector<RowControls> rowControls_;
  winrt::Microsoft::UI::Xaml::Controls::TextBlock introText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::HyperlinkButton docsLink_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button createButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button tryAgainButton_{nullptr};
};

}  // namespace urnw
