// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "VlessSheet.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>

#include "Localization.h"
#include "Log.h"
#include "SettingsSheets.h"  // the row kit: MakeSheet / Row / Supporting / Lookup / clipboard
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Windows::Foundation;
using namespace urnw::rows;

// NOTE on captures, as in ExtenderSheets.cpp: control handlers capture the
// sheet weakly; the coroutines take the dialog's DispatcherQueue, a weak
// reference and a pointer to the host (which outlives every sheet) before they
// leave the UI thread, never touch `this` off it, and come back through the
// queue.

namespace urnw {
namespace {

namespace automation = winrt::Microsoft::UI::Xaml::Automation;
namespace datatransfer = winrt::Windows::ApplicationModel::DataTransfer;
namespace input = winrt::Microsoft::UI::Xaml::Input;

hstring H(std::string const& s) { return winrt::to_hstring(s); }
hstring Loc(std::string_view key) { return hstring{Localized(key)}; }

Visibility Shown(bool shown) { return shown ? Visibility::Visible : Visibility::Collapsed; }

// A labelled text field: its label as the header and as its automation name,
// on the brand input style the other sheets' fields wear.
TextBox MakeTextField(Panel const& host, std::string_view labelKey) {
  TextBox box;
  box.Style(Lookup(L"UrTextInputStyle"));
  box.Header(winrt::box_value(Loc(labelKey)));
  automation::AutomationProperties::SetName(box, Loc(labelKey));
  host.Children().Append(box);
  return box;
}

// A labelled picker, the width of the fields around it.
ComboBox MakePicker(Panel const& host, std::string_view labelKey) {
  ComboBox picker;
  picker.Header(winrt::box_value(Loc(labelKey)));
  picker.HorizontalAlignment(HorizontalAlignment::Stretch);
  automation::AutomationProperties::SetName(picker, Loc(labelKey));
  host.Children().Append(picker);
  return picker;
}

// The picker's items for `options`, `value` selected. Labels come from the
// store; an entry without a label key shows its value (a TLS fingerprint, or a
// stored value this build does not offer).
void FillPicker(ComboBox const& picker, std::vector<vless::Option> const& options,
                std::string const& value) {
  picker.Items().Clear();
  for (vless::Option const& option : options) {
    picker.Items().Append(
        winrt::box_value(option.labelKey.empty() ? H(option.value) : Loc(option.labelKey)));
  }
  picker.SelectedIndex(vless::IndexOf(options, value));
}

// A muted help line under a field, as the network sheet's are.
TextBlock MakeNote(Panel const& host, hstring const& text) {
  TextBlock note;
  note.Text(text);
  note.FontSize(11);
  note.TextWrapping(TextWrapping::Wrap);
  note.Foreground(colors::MutedBrush());
  host.Children().Append(note);
  return note;
}

ToggleSwitch MakeSwitch() {
  ToggleSwitch toggle;
  toggle.Style(Lookup(L"UrSwitchToggleStyle"));
  return toggle;
}

}  // namespace

std::shared_ptr<VlessSheet> VlessSheet::Create(XamlRoot const& root, SdkHost& sdk) {
  auto sheet = std::shared_ptr<VlessSheet>(new VlessSheet(sdk));
  sheet->Build(root);
  sheet->Load();
  return sheet;
}

void VlessSheet::Build(XamlRoot const& root) {
  dialog_ = MakeSheet(root, Loc("vless"));
  dialog_.PrimaryButtonText(Loc("save"));
  // Save stays on the sheet: its verdict and the next-connect note are read
  // here, and the share link may be copied after it.
  dialog_.PrimaryButtonClick(
      [weak = weak_from_this()](auto const&, ContentDialogButtonClickEventArgs const& args) {
        args.Cancel(true);
        if (auto self = weak.lock()) self->Save();
      });

  StackPanel form;
  form.Spacing(12);
  // room for the scroll bar beside the fields
  form.Padding(ThicknessHelper::FromLengths(0, 0, 12, 0));

  Supporting(form, Loc("vless_settings_description"));

  enabledToggle_ = MakeSwitch();
  Row(form, Loc("vless_enabled"), hstring{}, enabledToggle_);

  // The share link: paste one to fill the whole form, or copy the form as one.
  linkBox_ = MakeTextField(form, "vless_link");
  MakeNote(form, Loc("vless_link_hint"));
  StackPanel linkActions;
  linkActions.Orientation(Orientation::Horizontal);
  linkActions.Spacing(8);
  pasteButton_ = Button();
  pasteButton_.Content(winrt::box_value(Loc("vless_paste_link")));
  pasteButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->PasteLink();
  });
  linkActions.Children().Append(pasteButton_);
  copyButton_ = Button();
  copyButton_.Content(winrt::box_value(Loc("vless_copy_link")));
  copyButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->CopyLink();
  });
  linkActions.Children().Append(copyButton_);
  form.Children().Append(linkActions);

  nameBox_ = MakeTextField(form, "name_label");
  addressBox_ = MakeTextField(form, "vless_server_address");
  portBox_ = MakeTextField(form, "vless_port");
  // Digits only, five at most (VlessPresentation.h IsPortInput), with the
  // number layout on a touch keyboard.
  portBox_.MaxLength(5);
  {
    input::InputScope scope;
    input::InputScopeName number;
    number.NameValue(input::InputScopeNameValue::Number);
    scope.Names().Append(number);
    portBox_.InputScope(scope);
  }
  portBox_.BeforeTextChanging(
      [](TextBox const&, TextBoxBeforeTextChangingEventArgs const& args) {
        if (!vless::IsPortInput(winrt::to_string(args.NewText()))) args.Cancel(true);
      });
  idBox_ = MakeTextField(form, "vless_user_id");

  networkPicker_ = MakePicker(form, "transport");
  securityPicker_ = MakePicker(form, "vless_security");
  flowPicker_ = MakePicker(form, "vless_flow");
  // The transport and the security decide which fields below exist at all.
  auto onPick = [weak = weak_from_this()](IInspectable const&, SelectionChangedEventArgs const&) {
    auto self = weak.lock();
    if (!self || self->applyingForm_) return;
    self->ApplyVisibility();
  };
  networkPicker_.SelectionChanged(onPick);
  securityPicker_.SelectionChanged(onPick);

  // tls and REALITY
  serverNameBox_ = MakeTextField(form, "vless_server_name");
  fingerprintPicker_ = MakePicker(form, "vless_fingerprint");
  // tls
  alpnBox_ = MakeTextField(form, "vless_alpn");
  insecureToggle_ = MakeSwitch();
  insecureRow_ = Row(form, Loc("vless_allow_insecure"), hstring{}, insecureToggle_);
  // REALITY
  publicKeyBox_ = MakeTextField(form, "vless_public_key");
  shortIdBox_ = MakeTextField(form, "vless_short_id");
  // ws and httpupgrade
  pathBox_ = MakeTextField(form, "vless_path");
  hostBox_ = MakeTextField(form, "vless_host_header");

  // One control around the whole form, so the form is disabled in one place
  // until the space's settings are read (a disabled parent disables its
  // children).
  formHost_ = ContentControl();
  formHost_.HorizontalContentAlignment(HorizontalAlignment::Stretch);
  formHost_.IsTabStop(false);
  formHost_.Content(form);

  ScrollViewer scroll;
  scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
  scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
  scroll.MaxHeight(460);
  scroll.Content(formHost_);

  statusText_ = TextBlock();
  statusText_.FontSize(12);
  statusText_.TextWrapping(TextWrapping::Wrap);
  statusText_.Visibility(Visibility::Collapsed);

  nextConnectText_ = TextBlock();
  nextConnectText_.Text(Loc("vless_settings_next_connect"));
  nextConnectText_.FontSize(12);
  nextConnectText_.TextWrapping(TextWrapping::Wrap);
  nextConnectText_.Foreground(colors::MutedBrush());
  nextConnectText_.Visibility(Visibility::Collapsed);

  StackPanel content;
  content.MinWidth(420);
  content.Spacing(12);
  content.Children().Append(scroll);
  content.Children().Append(statusText_);
  content.Children().Append(nextConnectText_);
  dialog_.Content(content);

  // The new form's shape until the read lands, with nothing editable: a save
  // before the read would write the defaults over the space's real settings.
  ApplyForm(vless::NewForm());
  formHost_.IsEnabled(false);
  dialog_.IsPrimaryButtonEnabled(false);
  ShowStatus(Loc("loading"), kit::ValidationState::Validating);
}

winrt::fire_and_forget VlessSheet::Load() {
  auto weak = weak_from_this();
  auto queue = dialog_.DispatcherQueue();
  SdkHost* const sdk = &sdk_;

  co_await winrt::resume_background();
  // nullopt is a read that failed, which is not a space without a server: the
  // sdk answers its new-form defaults for that
  std::optional<vless::Form> form;
  if (const std::optional<urnet::VlessSettings> settings = sdk->CurrentVlessSettings()) {
    form = vless::FormFrom(settings);
  }

  queue.TryEnqueue([weak, form] {
    if (auto self = weak.lock()) self->ApplyLoaded(form);
  });
}

void VlessSheet::ApplyLoaded(std::optional<vless::Form> const& form) {
  if (!form) {
    // The read-back after a save failing does not undo the save, whose verdict
    // stands (SdkHost logged the read); the first read failing leaves nothing
    // to edit.
    if (loaded_) return;
    ShowStatus(Loc("something_went_wrong"), kit::ValidationState::Invalid);
    return;
  }
  if (!loaded_) {
    loaded_ = true;
    formHost_.IsEnabled(true);
    ShowStatus({}, kit::ValidationState::NotChecked);
  }
  ApplyForm(*form);
  dialog_.IsPrimaryButtonEnabled(!busy_);
}

void VlessSheet::ApplyForm(vless::Form const& form) {
  // Refilling a picker raises its SelectionChanged; the visibility is applied
  // once at the end instead.
  applyingForm_ = true;
  enabledToggle_.IsOn(form.enabled);
  nameBox_.Text(H(form.name));
  addressBox_.Text(H(form.address));
  portBox_.Text(H(form.port));
  idBox_.Text(H(form.id));
  networkOptions_ = vless::OptionsWith(vless::NetworkOptions(), form.network);
  FillPicker(networkPicker_, networkOptions_, form.network);
  securityOptions_ = vless::OptionsWith(vless::SecurityOptions(), form.security);
  FillPicker(securityPicker_, securityOptions_, form.security);
  flowOptions_ = vless::OptionsWith(vless::FlowOptions(), form.flow);
  FillPicker(flowPicker_, flowOptions_, form.flow);
  serverNameBox_.Text(H(form.serverName));
  fingerprintOptions_ = vless::OptionsWith(vless::FingerprintOptions(), form.fingerprint);
  FillPicker(fingerprintPicker_, fingerprintOptions_, form.fingerprint);
  alpnBox_.Text(H(form.alpn));
  insecureToggle_.IsOn(form.allowInsecure);
  publicKeyBox_.Text(H(form.publicKey));
  shortIdBox_.Text(H(form.shortId));
  pathBox_.Text(H(form.path));
  hostBox_.Text(H(form.host));
  spiderX_ = form.spiderX;
  applyingForm_ = false;
  ApplyVisibility();
}

vless::Form VlessSheet::ReadForm() const {
  vless::Form form;
  form.enabled = enabledToggle_.IsOn();
  form.name = winrt::to_string(nameBox_.Text());
  form.address = winrt::to_string(addressBox_.Text());
  form.port = winrt::to_string(portBox_.Text());
  form.id = winrt::to_string(idBox_.Text());
  form.network = vless::ValueAt(networkOptions_, networkPicker_.SelectedIndex());
  form.security = vless::ValueAt(securityOptions_, securityPicker_.SelectedIndex());
  form.flow = vless::ValueAt(flowOptions_, flowPicker_.SelectedIndex());
  form.serverName = winrt::to_string(serverNameBox_.Text());
  form.fingerprint = vless::ValueAt(fingerprintOptions_, fingerprintPicker_.SelectedIndex());
  form.alpn = winrt::to_string(alpnBox_.Text());
  form.allowInsecure = insecureToggle_.IsOn();
  form.publicKey = winrt::to_string(publicKeyBox_.Text());
  form.shortId = winrt::to_string(shortIdBox_.Text());
  form.path = winrt::to_string(pathBox_.Text());
  form.host = winrt::to_string(hostBox_.Text());
  form.spiderX = spiderX_;
  return form;
}

void VlessSheet::ApplyVisibility() {
  const vless::FieldVisibility shown = vless::VisibilityFor(
      vless::ValueAt(networkOptions_, networkPicker_.SelectedIndex()),
      vless::ValueAt(securityOptions_, securityPicker_.SelectedIndex()));
  flowPicker_.Visibility(Shown(shown.flow));
  serverNameBox_.Visibility(Shown(shown.serverNameAndFingerprint));
  fingerprintPicker_.Visibility(Shown(shown.serverNameAndFingerprint));
  alpnBox_.Visibility(Shown(shown.alpnAndAllowInsecure));
  insecureRow_.Visibility(Shown(shown.alpnAndAllowInsecure));
  publicKeyBox_.Visibility(Shown(shown.publicKeyAndShortId));
  shortIdBox_.Visibility(Shown(shown.publicKeyAndShortId));
  pathBox_.Visibility(Shown(shown.pathAndHost));
  hostBox_.Visibility(Shown(shown.pathAndHost));
}

winrt::fire_and_forget VlessSheet::PasteLink() {
  if (busy_ || !loaded_) co_return;
  auto weak = weak_from_this();
  auto queue = dialog_.DispatcherQueue();
  SdkHost* const sdk = &sdk_;
  const std::string typed = winrt::to_string(linkBox_.Text());
  SetBusy(true);

  // The clipboard is read here, before the hop: Windows lets the foreground
  // app's UI thread read it. No text on it, or a clipboard another app holds
  // open, leaves the box's own text as the link.
  std::optional<std::string> clipboard;
  try {
    const auto view = datatransfer::Clipboard::GetContent();
    if (view.Contains(datatransfer::StandardDataFormats::Text())) {
      clipboard = winrt::to_string(co_await view.GetTextAsync());
    }
  } catch (...) {
    LogWarn("vless: the clipboard could not be read; parsing the link box");
  }
  const std::string link = vless::LinkToParse(clipboard, typed);
  const bool fromClipboard = link != typed;

  co_await winrt::resume_background();
  const std::optional<urnet::VlessLinkResult> result = sdk->ParseVlessLink(link);

  queue.TryEnqueue([weak, result, link, fromClipboard] {
    auto self = weak.lock();
    if (!self) return;
    self->SetBusy(false);
    // what was parsed stays in view, right or wrong
    if (fromClipboard) self->linkBox_.Text(H(link));
    if (!result) {
      self->ShowStatus(Loc("something_went_wrong"), kit::ValidationState::Invalid);
      return;
    }
    if (!result->Error.empty() || !result->Settings) {
      self->ShowError(result->Error);
      return;
    }
    // A link replaces the whole form, switched on (the sdk reads it enabled);
    // nothing is saved until Save.
    self->ApplyForm(vless::FormFrom(result->Settings));
    self->ShowStatus({}, kit::ValidationState::NotChecked);
  });
}

winrt::fire_and_forget VlessSheet::CopyLink() {
  if (busy_ || !loaded_) co_return;
  const auto built = vless::SettingsFrom<urnet::VlessSettings>(ReadForm());
  if (!built.errorKey.empty()) {
    ShowError(built.errorKey);
    co_return;
  }
  auto weak = weak_from_this();
  auto queue = dialog_.DispatcherQueue();
  SdkHost* const sdk = &sdk_;
  const urnet::VlessSettings settings = built.settings;
  SetBusy(true);

  co_await winrt::resume_background();
  // The link is "" for settings that do not validate; the validation names
  // the field, which is what the user can act on.
  const std::string link = sdk->VlessSettingsLink(settings);
  std::optional<std::string> errorId;
  if (link.empty()) errorId = sdk->ValidateVlessSettings(settings);

  queue.TryEnqueue([weak, link, errorId] {
    auto self = weak.lock();
    if (!self) return;
    self->SetBusy(false);
    if (link.empty()) {
      if (errorId && !errorId->empty()) {
        self->ShowError(*errorId);
      } else {
        self->ShowStatus(Loc("something_went_wrong"), kit::ValidationState::Invalid);
      }
      return;
    }
    try {
      CopyToClipboard(link);
    } catch (...) {
      LogWarn("vless: the clipboard could not be written");
      self->ShowStatus(Loc("something_went_wrong"), kit::ValidationState::Invalid);
      return;
    }
    self->ShowStatus(Loc("vless_link_copied"), kit::ValidationState::Valid);
  });
}

winrt::fire_and_forget VlessSheet::Save() {
  if (busy_ || !loaded_) co_return;
  const auto built = vless::SettingsFrom<urnet::VlessSettings>(ReadForm());
  if (!built.errorKey.empty()) {
    ShowError(built.errorKey);
    co_return;
  }
  auto weak = weak_from_this();
  auto queue = dialog_.DispatcherQueue();
  SdkHost* const sdk = &sdk_;
  const urnet::VlessSettings settings = built.settings;
  SetBusy(true);
  ShowStatus(Loc("loading"), kit::ValidationState::Validating);

  co_await winrt::resume_background();
  // "" saved; an error id saved nothing; nullopt never ran
  const std::optional<std::string> errorId = sdk->SetVlessSettings(settings);

  queue.TryEnqueue([weak, errorId] {
    auto self = weak.lock();
    if (!self) return;
    self->SetBusy(false);
    if (!errorId) {
      self->ShowStatus(Loc("something_went_wrong"), kit::ValidationState::Invalid);
      return;
    }
    if (!errorId->empty()) {
      self->ShowError(*errorId);
      return;
    }
    // Read back FIRST, as the extender settings do: the form then shows what
    // the sdk stored, and the read-back only fills fields, so the verdict
    // written after it stands.
    self->Load();
    self->ShowStatus(Loc("vless_settings_saved"), kit::ValidationState::Valid);
    // the tunnel runs in the service, which imports the space at its next
    // tunnel start (TunnelController step 3/8)
    self->nextConnectText_.Visibility(Visibility::Visible);
  });
}

void VlessSheet::SetBusy(bool busy) {
  busy_ = busy;
  pasteButton_.IsEnabled(!busy);
  copyButton_.IsEnabled(!busy);
  dialog_.IsPrimaryButtonEnabled(!busy && loaded_);
}

void VlessSheet::ShowStatus(hstring const& text, kit::ValidationState state) {
  kit::ApplySupportingText(statusText_, text, state);
  statusText_.Visibility(Shown(!text.empty()));
}

void VlessSheet::ShowError(std::string const& errorId) {
  ShowStatus(Loc(vless::ErrorKey(errorId)), kit::ValidationState::Invalid);
}

}  // namespace urnw
