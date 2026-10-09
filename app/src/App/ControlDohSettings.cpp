// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "ControlDohSettings.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>

#include "Localization.h"
#include "Log.h"
#include "SettingsSheets.h"  // the row kit: MakeSheet / Lookup
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Windows::Foundation;
using namespace urnw::rows;

// Note on captures, as in VlessSheet.cpp: control handlers capture the block
// weakly; the coroutines take the root's DispatcherQueue, a weak reference and
// a pointer to the host (which outlives every page and sheet) before they leave
// the UI thread, never touch `this` off it, and come back through the queue.

namespace urnw {
namespace {

namespace automation = winrt::Microsoft::UI::Xaml::Automation;

hstring H(std::string const& s) { return winrt::to_hstring(s); }
hstring Loc(std::string_view key) { return hstring{Localized(key)}; }

// Muted prose at the block's inset, wrapping. The inset is the Account pane's
// row inset, which the pane's action buttons carry as their margin too.
TextBlock MakeProse(Panel const& host, hstring const& text, double size) {
  TextBlock prose;
  prose.Text(text);
  prose.FontSize(size);
  prose.TextWrapping(TextWrapping::Wrap);
  prose.Foreground(colors::MutedBrush());
  prose.Margin(ThicknessHelper::FromLengths(12, 0, 12, 0));
  host.Children().Append(prose);
  return prose;
}

// A full-width command on the Account pane's bar style, which is also what the
// block wears in its sheet.
Button MakeAction(Panel const& host, std::string_view key, bool primary) {
  Button button;
  button.Content(winrt::box_value(Loc(key)));
  button.Style(Lookup(primary ? L"UrPaneActionPrimaryStyle" : L"UrPaneActionSecondaryStyle"));
  button.HorizontalAlignment(HorizontalAlignment::Stretch);
  automation::AutomationProperties::SetName(button, Loc(key));
  host.Children().Append(button);
  return button;
}

}  // namespace

std::shared_ptr<ControlDohBlock> ControlDohBlock::Create(SdkHost& sdk) {
  auto block = std::shared_ptr<ControlDohBlock>(new ControlDohBlock(sdk));
  block->Build();
  return block;
}

void ControlDohBlock::Build() {
  root_ = StackPanel();
  root_.Spacing(8);
  root_.Padding(ThicknessHelper::FromLengths(0, 10, 0, 0));

  // The privacy disclosure is part of the description, so it is always shown,
  // not only on focus.
  description_ = MakeProse(root_, Loc("control_doh_urls_description"), 12);

  // One server per line. No placeholder: an empty box means the built-in
  // servers alone, and the hint under it already shows the shape of a line.
  urlsBox_ = TextBox();
  urlsBox_.Style(Lookup(L"UrTextInputStyle"));
  urlsBox_.AcceptsReturn(true);
  urlsBox_.TextWrapping(TextWrapping::Wrap);
  urlsBox_.IsSpellCheckEnabled(false);
  urlsBox_.Height(108);
  urlsBox_.Margin(ThicknessHelper::FromLengths(12, 0, 12, 0));
  automation::AutomationProperties::SetName(urlsBox_, Loc("control_doh_urls"));
  root_.Children().Append(urlsBox_);
  hint_ = MakeProse(root_, Loc("control_doh_urls_hint"), 11);

  chinaButton_ = MakeAction(root_, "control_doh_use_china", /*primary=*/false);
  buttons_.emplace_back(chinaButton_, "control_doh_use_china");
  chinaButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->UseChinaResolvers();
  });
  chinaHint_ = MakeProse(root_, Loc("control_doh_use_china_hint"), 11);

  resetButton_ = MakeAction(root_, "control_doh_urls_reset", /*primary=*/false);
  buttons_.emplace_back(resetButton_, "control_doh_urls_reset");
  resetButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    auto self = weak.lock();
    if (!self || self->busy_ || !self->loaded_) return;
    // the built-in servers alone, saved at once: an empty list clears
    self->urlsBox_.Text(hstring{});
    self->Save({});
  });

  saveButton_ = MakeAction(root_, "save", /*primary=*/true);
  buttons_.emplace_back(saveButton_, "save");
  saveButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      self->Save(ParseControlDohLines(winrt::to_string(self->urlsBox_.Text())));
    }
  });

  statusText_ = MakeProse(root_, {}, 12);
  statusText_.Visibility(Visibility::Collapsed);
  // the tunnel runs in the service, which imports the space at its next
  // tunnel start (TunnelController step 3/8)
  nextConnectText_ = MakeProse(root_, Loc("control_doh_urls_next_connect"), 12);
  nextConnectText_.Visibility(Visibility::Collapsed);

  // Nothing is editable until the read lands: a save before it would write an
  // empty list over the space's real servers.
  SetBusy(false);
  ShowStatus(Loc("loading"), kit::ValidationState::Validating);
}

winrt::fire_and_forget ControlDohBlock::Load() {
  auto weak = weak_from_this();
  auto queue = root_.DispatcherQueue();
  SdkHost* const sdk = &sdk_;

  co_await winrt::resume_background();
  const std::optional<std::vector<std::string>> urls = sdk->CurrentControlDohUrls();

  queue.TryEnqueue([weak, urls] {
    if (auto self = weak.lock()) self->ApplyLoaded(urls);
  });
}

void ControlDohBlock::ApplyLoaded(std::optional<std::vector<std::string>> const& urls) {
  if (!urls) {
    // The read-back after a save failing does not undo the save, whose verdict
    // stands (SdkHost logged the read); the first read failing leaves nothing
    // to edit.
    if (loaded_) return;
    ShowStatus(Loc("something_went_wrong"), kit::ValidationState::Invalid);
    return;
  }
  if (!loaded_) {
    loaded_ = true;
    ShowStatus({}, kit::ValidationState::NotChecked);
  }
  urlsBox_.Text(H(ControlDohText(*urls)));
  SetBusy(busy_);
}

winrt::fire_and_forget ControlDohBlock::UseChinaResolvers() {
  if (busy_ || !loaded_) co_return;
  auto weak = weak_from_this();
  auto queue = root_.DispatcherQueue();
  SdkHost* const sdk = &sdk_;
  SetBusy(true);

  co_await winrt::resume_background();
  // the sdk's preset is the one source of the servers (connect
  // RegionalControlDohUrls), so every app fills in the same four
  const std::vector<std::string> preset = sdk->RegionalControlDohUrls(kControlDohChinaCountryCode);

  queue.TryEnqueue([weak, preset] {
    auto self = weak.lock();
    if (!self) return;
    self->SetBusy(false);
    if (preset.empty()) {
      self->ShowStatus(Loc("something_went_wrong"), kit::ValidationState::Invalid);
      return;
    }
    // The box's contents are replaced and nothing is saved: the user reviews
    // the servers and saves.
    self->urlsBox_.Text(H(ControlDohText(preset)));
    self->ShowStatus({}, kit::ValidationState::NotChecked);
    self->nextConnectText_.Visibility(Visibility::Collapsed);
  });
}

winrt::fire_and_forget ControlDohBlock::Save(std::vector<std::string> urls) {
  if (busy_ || !loaded_) co_return;
  auto weak = weak_from_this();
  auto queue = root_.DispatcherQueue();
  SdkHost* const sdk = &sdk_;
  SetBusy(true);
  ShowStatus(Loc("loading"), kit::ValidationState::Validating);

  co_await winrt::resume_background();
  // "" saved; an error id saved nothing; nullopt never ran
  const std::optional<std::string> errorId = sdk->SetControlDohUrls(urls);

  queue.TryEnqueue([weak, errorId] {
    auto self = weak.lock();
    if (!self) return;
    self->SetBusy(false);
    if (!errorId) {
      self->ShowStatus(Loc("something_went_wrong"), kit::ValidationState::Invalid);
      return;
    }
    // Only "" is saved: any id, the sdk's internal_error included, saved nothing.
    const ControlDohSaveOutcome outcome = ControlDohSaveOutcomeFor(*errorId);
    if (!outcome.saved) {
      // the box keeps what was typed
      self->ShowStatus(Loc(outcome.messageKey), kit::ValidationState::Invalid);
      return;
    }
    // Read back first, as the VLESS sheet does: the box then shows the list the
    // sdk stored, and the read-back only fills the box, so the verdict written
    // after it stands.
    self->Load();
    self->ShowStatus(Loc(outcome.messageKey), kit::ValidationState::Valid);
    if (outcome.nextConnectNote) self->nextConnectText_.Visibility(Visibility::Visible);
  });
}

void ControlDohBlock::SetBusy(bool busy) {
  busy_ = busy;
  const bool live = loaded_ && !busy;
  urlsBox_.IsEnabled(live);
  for (auto const& entry : buttons_) entry.first.IsEnabled(live);
}

void ControlDohBlock::ShowStatus(hstring const& text, kit::ValidationState state) {
  kit::ApplySupportingText(statusText_, text, state);
  statusText_.Visibility(text.empty() ? Visibility::Collapsed : Visibility::Visible);
}

void ControlDohBlock::ApplyStrings() {
  description_.Text(Loc("control_doh_urls_description"));
  hint_.Text(Loc("control_doh_urls_hint"));
  chinaHint_.Text(Loc("control_doh_use_china_hint"));
  nextConnectText_.Text(Loc("control_doh_urls_next_connect"));
  automation::AutomationProperties::SetName(urlsBox_, Loc("control_doh_urls"));
  for (auto const& [button, key] : buttons_) {
    button.Content(winrt::box_value(Loc(key)));
    automation::AutomationProperties::SetName(button, Loc(key));
  }
}

std::shared_ptr<ControlDohSheet> ControlDohSheet::Create(XamlRoot const& root, SdkHost& sdk) {
  auto sheet = std::shared_ptr<ControlDohSheet>(new ControlDohSheet());
  sheet->block_ = ControlDohBlock::Create(sdk);
  sheet->dialog_ = MakeSheet(root, Loc("control_doh_urls"));

  StackPanel content;
  content.MinWidth(420);
  content.Children().Append(sheet->block_->Root());
  sheet->dialog_.Content(content);

  sheet->block_->Load();
  return sheet;
}

}  // namespace urnw
