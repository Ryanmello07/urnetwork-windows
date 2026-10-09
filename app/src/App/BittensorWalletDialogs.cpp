// SPDX-License-Identifier: MPL-2.0
// the project compiles with /Yu"pch.h" (App.vcxproj), so every translation unit
// must include it first
#include "pch.h"

#include "BittensorWalletDialogs.h"

#include <memory>

#include <winrt/Windows.ApplicationModel.DataTransfer.h>

#include "BittensorWalletFlow.h"
#include "Localization.h"
#include "Log.h"
#include "MainWindow.xaml.h"
#include "PageContext.h"
#include "Strings.h"
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

namespace urnw::pages {

winrt::fire_and_forget ChooseBittensorWallet(
    winrt::com_ptr<winrt::URnetwork::implementation::MainWindow> window,
    std::function<void(std::string walletId)> next) {
  if (!window || window->sheetOpen()) co_return;  // one ContentDialog at a time

  // the supported wallets, by their product names, each with the line that
  // says what it is (manual entry; which WalletConnect wallets)
  ContentDialog dialog;
  dialog.XamlRoot(window->Content().XamlRoot());
  dialog.Title(winrt::box_value(Loc("bittensor_choose_wallet")));
  dialog.CloseButtonText(Loc("cancel"));
  dialog.Background(colors::SheetBrush());

  auto choice = std::make_shared<int>(-1);
  StackPanel list;
  list.MinWidth(320);
  list.Spacing(8);
  for (int i = 0; i < bittensor::kChooserWalletCount; ++i) {
    const std::string walletId(bittensor::kChooserWallets[i]);
    StackPanel label;
    TextBlock name;
    name.Text(H(urnet::bittensorWalletDisplayName(walletId)));
    name.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    label.Children().Append(name);
    const std::string hintKey = bittensor::ChooserHintKey(walletId);
    if (!hintKey.empty()) {
      TextBlock hint;
      hint.Text(Loc(hintKey));
      hint.Opacity(0.7);
      hint.TextWrapping(TextWrapping::Wrap);
      label.Children().Append(hint);
    }
    Button button;
    button.HorizontalAlignment(HorizontalAlignment::Stretch);
    button.HorizontalContentAlignment(HorizontalAlignment::Left);
    button.Content(label);
    button.Click([dialog, choice, i](auto const&, auto const&) {
      *choice = i;
      dialog.Hide();
    });
    list.Children().Append(button);
  }
  dialog.Content(list);

  window->SetSheetOpen(true);
  try {
    co_await dialog.ShowAsync();
  } catch (std::exception const& e) {
    urnw::LogError("bittensor wallet chooser: {}", e.what());
  } catch (...) {
    urnw::LogError("bittensor wallet chooser failed to open");
  }
  window->SetSheetOpen(false);

  if (next) next(bittensor::WalletForChoice(*choice));
}

winrt::fire_and_forget ShowBittensorManualForm(
    winrt::com_ptr<winrt::URnetwork::implementation::MainWindow> window,
    SdkHost::BittensorManualRequest request) {
  if (!window || window->sheetOpen()) {
    // nowhere to show it: the proof is answered rather than left waiting
    Sdk().CancelBittensorProof();
    co_return;
  }

  StackPanel content;
  content.MinWidth(380);
  content.Spacing(8);

  TextBlock instructions;
  instructions.TextWrapping(TextWrapping::Wrap);
  instructions.Text(winrt::hstring{
      urnw::Format("bittensor_manual_sign_instructions", Widen(request.walletName))});
  content.Children().Append(instructions);

  // the exact challenge: read-only, selectable, and one click to copy
  TextBox messageBox;
  messageBox.Header(winrt::box_value(Loc("bittensor_message_to_sign")));
  messageBox.Text(H(request.message));
  messageBox.IsReadOnly(true);
  messageBox.AcceptsReturn(true);
  messageBox.TextWrapping(TextWrapping::Wrap);
  content.Children().Append(messageBox);

  Button copyButton;
  copyButton.Content(winrt::box_value(Loc("copy")));
  const std::string message = request.message;
  copyButton.Click([message](auto const&, auto const&) {
    namespace dt = winrt::Windows::ApplicationModel::DataTransfer;
    try {
      dt::DataPackage package;
      package.SetText(H(message));
      dt::Clipboard::SetContent(package);
    } catch (...) {
      urnw::LogWarn("bittensor manual form: the clipboard refused the message");
    }
  });
  content.Children().Append(copyButton);

  TextBox addressBox;
  addressBox.PlaceholderText(Loc("earnings_address_placeholder"));
  addressBox.Text(H(request.address));
  content.Children().Append(addressBox);

  TextBox signatureBox;
  signatureBox.Header(winrt::box_value(Loc("bittensor_signature_label")));
  signatureBox.PlaceholderText(Loc("bittensor_signature_placeholder"));
  content.Children().Append(signatureBox);

  TextBlock errorText;
  errorText.FontSize(12);
  errorText.TextWrapping(TextWrapping::Wrap);
  errorText.Foreground(colors::DangerBrush());
  errorText.Visibility(Visibility::Collapsed);
  content.Children().Append(errorText);

  ContentDialog dialog;
  dialog.XamlRoot(window->Content().XamlRoot());
  dialog.Title(winrt::box_value(H(request.walletName)));
  dialog.Content(content);
  dialog.PrimaryButtonText(Loc("continue_txt"));
  dialog.CloseButtonText(Loc("cancel"));
  dialog.DefaultButton(ContentDialogButton::Primary);
  dialog.Background(colors::SheetBrush());

  // whether the session took the proof or ended it; the close then needs no cancel
  auto settled = std::make_shared<bool>(false);
  dialog.PrimaryButtonClick([addressBox, signatureBox, errorText, settled](
                                auto const&, ContentDialogButtonClickEventArgs const& args) {
    const auto answer = Sdk().SubmitBittensorManual(Narrow(addressBox.Text().c_str()),
                                                    Narrow(signatureBox.Text().c_str()));
    if (answer.closed) {
      *settled = true;
      return;  // the dialog closes; the flow has its answer
    }
    args.Cancel(true);  // a typo: keep the form, say what is wrong
    errorText.Text(H(answer.error));
    errorText.Visibility(Visibility::Visible);
  });

  window->SetSheetOpen(true);
  try {
    co_await dialog.ShowAsync();
  } catch (...) {
    urnw::LogError("bittensor manual form failed to open");
  }
  window->SetSheetOpen(false);
  if (!*settled) Sdk().CancelBittensorProof();
}

}  // namespace urnw::pages
