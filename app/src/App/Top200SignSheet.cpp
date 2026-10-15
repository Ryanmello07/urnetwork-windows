// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "Top200SignSheet.h"

#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>

#include <stdexcept>

#include "Localization.h"
#include "Log.h"
#include "PageContext.h"
#include "SdkHost.h"
#include "SheetFit.h"  // sheetfit: sheets clamp to the window at open time
#include "Strings.h"
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace urnw::pages;

namespace urnw {
namespace {

namespace dt = winrt::Windows::ApplicationModel::DataTransfer;

TextBlock MakeText(hstring const& text, double fontSize, Brush const& brush = nullptr,
                   bool wrap = false) {
  TextBlock tb;
  tb.Text(text);
  tb.FontSize(fontSize);
  if (brush) tb.Foreground(brush);
  if (wrap) tb.TextWrapping(TextWrapping::Wrap);
  return tb;
}

// the 12sp muted caption above a value
TextBlock MakeCaption(hstring const& text) { return MakeText(text, 12, colors::MutedBrush(), true); }

// a hex value, shortened, selectable, with the whole value in its tooltip
TextBlock MakeMono() {
  TextBlock tb = MakeText(hstring{}, 13, colors::TextBrush());
  tb.FontFamily(FontFamily(L"Consolas"));
  tb.IsTextSelectionEnabled(true);
  return tb;
}

void ShowMono(TextBlock const& tb, std::string const& full) {
  tb.Text(H(top200sign::Shorten(full)));
  if (full.empty()) {
    ToolTipService::SetToolTip(tb, nullptr);
  } else {
    ToolTipService::SetToolTip(tb, winrt::box_value(H(full)));
  }
}

Button MakeSmallButton(hstring const& text) {
  Button button;
  button.Content(winrt::box_value(text));
  button.Padding(Thickness{10, 2, 10, 2});
  return button;
}

// a caption, its value and a Copy button on one row
Grid MakeCopyRow(hstring const& caption, TextBlock const& mono, Button const& copy) {
  Grid row;
  row.ColumnSpacing(8);
  ColumnDefinition star;
  star.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
  ColumnDefinition automatic;
  automatic.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Auto));
  row.ColumnDefinitions().Append(star);
  row.ColumnDefinitions().Append(automatic);
  StackPanel texts;
  texts.Spacing(2);
  texts.Children().Append(MakeCaption(caption));
  texts.Children().Append(mono);
  row.Children().Append(texts);
  Grid::SetColumn(copy, 1);
  copy.VerticalAlignment(VerticalAlignment::Bottom);
  row.Children().Append(copy);
  return row;
}

}  // namespace

std::shared_ptr<Top200SignSheet> Top200SignSheet::Create(
    XamlRoot const& root, winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue,
    std::string prefill, bool linkInvalid) {
  auto sheet = std::shared_ptr<Top200SignSheet>(new Top200SignSheet(queue));
  sheet->Build(root);
  sheet->Prefill(prefill, linkInvalid);
  sheet->LoadDevice();
  return sheet;
}

Top200SignSheet::Top200SignSheet(winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue)
    : queue_(queue) {}

void Top200SignSheet::Build(XamlRoot const& root) {
  dialog_ = ContentDialog();
  dialog_.XamlRoot(root);
  dialog_.Title(winrt::box_value(Loc("top200_sign_binding")));
  dialog_.Background(colors::SheetBrush());
  dialog_.PrimaryButtonText(Loc("top200_sign_action"));
  dialog_.CloseButtonText(Loc("close"));
  dialog_.DefaultButton(ContentDialogButton::None);
  dialog_.IsPrimaryButtonEnabled(false);

  StackPanel content;
  content.Spacing(8);
  content.MinWidth(sheetfit::Width(root, 480));
  content.MaxWidth(sheetfit::Width(root, 560));

  content.Children().Append(MakeText(Loc("top200_sign_intro"), 13, colors::MutedBrush(), true));

  // ---- this device
  content.Children().Append(MakeText(Loc("top200_sign_this_device"), 14, colors::TextBrush()));
  clientIdValue_ = MakeMono();
  copyClientIdButton_ = MakeSmallButton(Loc("copy"));
  copyClientIdButton_.IsEnabled(false);
  copyClientIdButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->Copy(self->clientId_, self->copyClientIdButton_);
  });
  content.Children().Append(MakeCopyRow(Loc("client_id"), clientIdValue_, copyClientIdButton_));
  clientKeyValue_ = MakeMono();
  copyClientKeyButton_ = MakeSmallButton(Loc("copy"));
  copyClientKeyButton_.IsEnabled(false);
  copyClientKeyButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->Copy(self->clientKey_, self->copyClientKeyButton_);
  });
  content.Children().Append(
      MakeCopyRow(Loc("top200_sign_client_key"), clientKeyValue_, copyClientKeyButton_));
  noKeyText_ = MakeText(Loc("top200_sign_error_no_key"), 12, colors::DangerBrush(), true);
  noKeyText_.Visibility(Visibility::Collapsed);
  content.Children().Append(noKeyText_);
  content.Children().Append(MakeCaption(Loc("top200_sign_member_hint")));
  copyMemberButton_ = MakeSmallButton(Loc("top200_sign_copy_member"));
  copyMemberButton_.IsEnabled(false);
  copyMemberButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      self->Copy(top200sign::MemberEntry(self->clientId_, self->clientKey_), self->copyMemberButton_);
    }
  });
  content.Children().Append(copyMemberButton_);

  // ---- the binding
  Grid boxHeader;
  boxHeader.ColumnSpacing(8);
  {
    ColumnDefinition star;
    star.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
    ColumnDefinition automatic;
    automatic.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Auto));
    boxHeader.ColumnDefinitions().Append(star);
    boxHeader.ColumnDefinitions().Append(automatic);
  }
  TextBlock boxLabel = MakeText(Loc("top200_sign_binding_label"), 14, colors::TextBrush());
  boxLabel.VerticalAlignment(VerticalAlignment::Bottom);
  boxHeader.Children().Append(boxLabel);
  Button paste = MakeSmallButton(Loc("top200_sign_paste"));
  Grid::SetColumn(paste, 1);
  paste.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->PasteFromClipboard();
  });
  boxHeader.Children().Append(paste);
  boxHeader.Margin(Thickness{0, 8, 0, 0});
  content.Children().Append(boxHeader);

  bindingBox_ = TextBox();
  bindingBox_.AcceptsReturn(true);
  bindingBox_.TextWrapping(TextWrapping::Wrap);
  bindingBox_.IsSpellCheckEnabled(false);
  bindingBox_.IsTextPredictionEnabled(false);
  bindingBox_.FontFamily(FontFamily(L"Consolas"));
  bindingBox_.FontSize(12);
  bindingBox_.MinHeight(96);
  bindingBox_.MaxHeight(160);
  bindingBox_.MaxLength(static_cast<int32_t>(top200sign::kMaxBindingTextLength));
  bindingBox_.TextChanged([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      // any edit ends the last signing's result
      ++self->edit_;
      self->payload_.clear();
      self->failure_.clear();
      self->ApplyState();
    }
  });
  content.Children().Append(bindingBox_);

  errorText_ = MakeText(hstring{}, 12, colors::DangerBrush(), true);
  errorText_.Visibility(Visibility::Collapsed);
  content.Children().Append(errorText_);

  // ---- the review
  reviewPanel_ = StackPanel();
  reviewPanel_.Spacing(4);
  reviewPanel_.Visibility(Visibility::Collapsed);
  reviewPanel_.Children().Append(MakeText(Loc("top200_sign_review"), 12, colors::MutedBrush(), true));
  fleetIdValue_ = MakeMono();
  reviewPanel_.Children().Append(MakeCaption(Loc("top200_sign_fleet_id")));
  reviewPanel_.Children().Append(fleetIdValue_);
  hotkeyValue_ = MakeMono();
  hotkeyValue_.TextWrapping(TextWrapping::Wrap);
  reviewPanel_.Children().Append(MakeCaption(Loc("top200_sign_hotkey")));
  reviewPanel_.Children().Append(hotkeyValue_);
  chainValue_ = MakeText(hstring{}, 13, colors::TextBrush(), true);
  reviewPanel_.Children().Append(chainValue_);
  epochsValue_ = MakeText(hstring{}, 13, colors::TextBrush(), true);
  reviewPanel_.Children().Append(epochsValue_);
  content.Children().Append(reviewPanel_);

  // ---- signing, then the payload
  progressRow_ = StackPanel();
  progressRow_.Orientation(Orientation::Horizontal);
  progressRow_.Spacing(8);
  ProgressRing ring;
  ring.Width(16);
  ring.Height(16);
  ring.IsActive(true);
  progressRow_.Children().Append(ring);
  progressRow_.Visibility(Visibility::Collapsed);
  content.Children().Append(progressRow_);

  donePanel_ = StackPanel();
  donePanel_.Spacing(6);
  donePanel_.Visibility(Visibility::Collapsed);
  donePanel_.Children().Append(MakeText(Loc("top200_sign_done"), 13, colors::AccentBrush(), true));
  payloadBox_ = TextBox();
  payloadBox_.IsReadOnly(true);
  payloadBox_.AcceptsReturn(true);
  payloadBox_.TextWrapping(TextWrapping::Wrap);
  payloadBox_.FontFamily(FontFamily(L"Consolas"));
  payloadBox_.FontSize(12);
  payloadBox_.MaxHeight(160);
  donePanel_.Children().Append(payloadBox_);
  copyPayloadButton_ = MakeSmallButton(Loc("top200_sign_copy_payload"));
  copyPayloadButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->Copy(self->payload_, self->copyPayloadButton_);
  });
  donePanel_.Children().Append(copyPayloadButton_);
  content.Children().Append(donePanel_);

  ScrollViewer scroll;
  scroll.Content(content);
  scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
  scroll.MaxHeight(sheetfit::Height(root, 640));
  dialog_.Content(scroll);

  dialog_.PrimaryButtonClick(
      [weak = weak_from_this()](auto const&, ContentDialogButtonClickEventArgs const& args) {
        args.Cancel(true);  // the sheet stays open to show the payload
        if (auto self = weak.lock()) self->Sign();
      });

  ApplyState();
}

void Top200SignSheet::Prefill(std::string const& binding, bool linkInvalid) {
  linkInvalid_ = linkInvalid;
  bindingBox_.Text(linkInvalid ? hstring{} : hstring{Widen(binding)});
  ++edit_;
  payload_.clear();
  failure_.clear();
  ApplyState();
}

// The client id and the client key, off the UI thread: on the DeviceRemote the
// key is an rpc into the service, which answers "" while it is not connected.
winrt::fire_and_forget Top200SignSheet::LoadDevice() {
  auto weak = weak_from_this();
  auto queue = queue_;
  co_await winrt::resume_background();
  std::string clientId;
  std::string clientKey;
  try {
    SdkHost& sdk = Sdk();
    if (sdk.hasDevice()) {
      clientId = sdk.device().getClientId();
      clientKey = sdk.device().getSnClientKey();
    }
  } catch (const std::exception& e) {
    LogWarn("top200 sign: the device read failed: {}", e.what());
  } catch (...) {
    LogWarn("top200 sign: the device read failed");
  }
  queue.TryEnqueue([weak, clientId, clientKey] {
    if (auto self = weak.lock()) self->ApplyDevice(clientId, clientKey);
  });
}

void Top200SignSheet::ApplyDevice(std::string clientId, std::string clientKey) {
  deviceLoaded_ = true;
  clientId_ = std::move(clientId);
  clientKey_ = std::move(clientKey);
  if (clientKey_.empty()) LogInfo("top200 sign: no client key (the service is not connected)");
  ApplyState();
}

winrt::fire_and_forget Top200SignSheet::PasteFromClipboard() {
  auto weak = weak_from_this();
  hstring text;
  try {
    const dt::DataPackageView view = dt::Clipboard::GetContent();
    if (!view.Contains(dt::StandardDataFormats::Text())) co_return;
    text = co_await view.GetTextAsync();
  } catch (...) {
    LogWarn("top200 sign: the clipboard could not be read");
    co_return;
  }
  // back on the UI thread (the awaiter resumes in the caller's apartment)
  auto self = weak.lock();
  if (!self) co_return;
  self->linkInvalid_ = false;
  self->bindingBox_.Text(text);
}

winrt::fire_and_forget Top200SignSheet::Sign() {
  if (signing_ || !deviceLoaded_) co_return;
  const top200sign::Check check =
      top200sign::Validate(Narrow(std::wstring{bindingBox_.Text()}), clientId_, clientKey_);
  if (check.verdict != top200sign::Verdict::Valid) co_return;
  LogInfo("top200 sign: sign tapped");
  signing_ = true;
  payload_.clear();
  failure_.clear();
  ApplyState();

  auto weak = weak_from_this();
  auto queue = queue_;
  const uint64_t edit = edit_;
  co_await winrt::resume_background();
  // the service holds the client key and signs (DeviceLocalRpc.SignSnFleetBinding
  // over the app's existing local device rpc); the text never leaves this machine
  const top200sign::Signed result = top200sign::SignChecked(check, [](std::string const& binding) {
    SdkHost& sdk = Sdk();
    if (!sdk.hasDevice()) throw std::runtime_error("device service not connected");
    return sdk.device().signSnFleetBinding(binding);
  });
  if (result.ok) {
    LogInfo("top200 sign: signed");
  } else {
    LogWarn("top200 sign: the binding was not signed");
  }
  queue.TryEnqueue([weak, result, edit] {
    if (auto self = weak.lock()) self->ApplySigned(result, edit);
  });
}

void Top200SignSheet::ApplySigned(top200sign::Signed result, uint64_t edit) {
  signing_ = false;
  // the box changed while the service signed: that result is not this text's
  if (edit == edit_) {
    if (result.ok) {
      payload_ = std::move(result.payload);
      copyPayloadButton_.Content(winrt::box_value(Loc("top200_sign_copy_payload")));
    } else {
      failure_ = result.detail.empty() ? std::string("error") : std::move(result.detail);
    }
  }
  ApplyState();
}

void Top200SignSheet::ApplyState() {
  // this device
  ShowMono(clientIdValue_, clientId_);
  ShowMono(clientKeyValue_, clientKey_);
  copyClientIdButton_.IsEnabled(!clientId_.empty());
  copyClientKeyButton_.IsEnabled(!clientKey_.empty());
  copyMemberButton_.IsEnabled(!top200sign::MemberEntry(clientId_, clientKey_).empty());
  noKeyText_.Visibility(deviceLoaded_ && clientKey_.empty() ? Visibility::Visible
                                                            : Visibility::Collapsed);

  // the box: until the device is read, only "not a binding" is known
  const top200sign::Check check =
      top200sign::Validate(Narrow(std::wstring{bindingBox_.Text()}), clientId_, clientKey_);
  top200sign::Verdict verdict = check.verdict;
  if (!deviceLoaded_ && verdict != top200sign::Verdict::Invalid) {
    verdict = top200sign::Verdict::Empty;
  }
  hstring error;
  if (!failure_.empty()) {
    error = hstring{Format("top200_sign_error_failed", Widen(failure_))};
  } else if (linkInvalid_ && verdict == top200sign::Verdict::Empty) {
    error = Loc("top200_sign_error_invalid");
  } else if (const std::string_view key = top200sign::ErrorKey(verdict); !key.empty()) {
    error = Loc(key);
  }
  errorText_.Text(error);
  errorText_.Visibility(error.empty() ? Visibility::Collapsed : Visibility::Visible);

  // the review: fleet ID, hotkey, chain and epochs are on screen before Sign
  // can be tapped
  const bool valid = verdict == top200sign::Verdict::Valid && check.binding.has_value();
  reviewPanel_.Visibility(valid ? Visibility::Visible : Visibility::Collapsed);
  if (valid) {
    ShowMono(fleetIdValue_, check.binding->fleetId);
    // in full: the user compares all 64 hex digits with the page
    hotkeyValue_.Text(H(check.binding->hotkey));
    chainValue_.Text(hstring{Format("top200_sign_chain", check.binding->chainId, check.binding->netuid)});
    epochsValue_.Text(hstring{
        Format("top200_sign_epochs", check.binding->validFromEpoch, check.binding->validToEpoch)});
  }

  progressRow_.Visibility(signing_ ? Visibility::Visible : Visibility::Collapsed);
  donePanel_.Visibility(payload_.empty() ? Visibility::Collapsed : Visibility::Visible);
  payloadBox_.Text(H(payload_));
  bindingBox_.IsReadOnly(signing_);
  dialog_.IsPrimaryButtonEnabled(valid && !signing_);
}

void Top200SignSheet::Copy(std::string const& text, Button const& button) {
  if (text.empty()) return;
  try {
    dt::DataPackage package;
    package.SetText(H(text));
    dt::Clipboard::SetContent(package);
  } catch (...) {
    LogWarn("top200 sign: the clipboard refused the copy");
    return;
  }
  button.Content(winrt::box_value(Loc("copied")));
}

}  // namespace urnw
