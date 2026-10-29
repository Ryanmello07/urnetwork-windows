// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "ApiKeysPage.h"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>

#include "Localization.h"
#include "Log.h"
#include "MainWindow.xaml.h"
#include "PageContext.h"
#include "Sdk.h"      // urnet::Api's API key calls and their results
#include "SdkHost.h"  // Sdk().api()
#include "SettingsSheets.h"  // rows::MakeSheet, rows::Lookup
#include "SheetFit.h"        // sheetfit: sheets clamp to the window at open time
#include "StatsFormat.h"     // the user's dates
#include "Strings.h"
#include "UrColors.h"
#include "UrComponents.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using urnw::pages::H;
using urnw::pages::Loc;
using urnw::pages::Sdk;

namespace urnw {

using winrt::Windows::Foundation::IInspectable;

namespace {

// two or three lines beside the key's mark; a minimum, so larger text grows
// the row rather than clipping it
constexpr double kRowMinHeight = 56;

constexpr wchar_t kKeyGlyph[] = L"\uE8D7";   // Permissions, a key: each key's row
constexpr wchar_t kCopyGlyph[] = L"\uE8C8";  // Copy
constexpr wchar_t kAddGlyph[] = L"\uE710";   // Add

// how long the copy button says "Copied"
constexpr std::chrono::milliseconds kCopiedFor{2000};

// The page's words: the store's strings and the user's dates (StatsFormat.h).
apikeys::Text ApiKeysText() {
  apikeys::Text text;
  text.localized = [](std::string_view key) { return Narrow(Localized(key)); };
  text.format = [](std::string_view key, std::string const& value) {
    return Narrow(Format(key, Widen(value)));
  };
  text.date = [](int64_t unixMillis) { return FormatLocalDate(unixMillis); };
  text.dateTime = [](int64_t unixMillis) { return FormatLocalDateTime(unixMillis); };
  return text;
}

// One line of a row: one line high whatever it says, trimmed with its whole
// text on hover, and read out as `spoken` when that differs (a date is read as
// the full date and time).
TextBlock MakeLine(std::string const& text, wchar_t const* style, std::string const& spoken = {}) {
  TextBlock line;
  if (auto found = rows::Lookup(style)) line.Style(found);
  line.Text(H(text));
  line.TextTrimming(TextTrimming::CharacterEllipsis);
  line.TextWrapping(TextWrapping::NoWrap);
  ToolTipService::SetToolTip(line, winrt::box_value(H(text)));
  if (!spoken.empty() && spoken != text) {
    Automation::AutomationProperties::SetName(line, H(spoken));
  }
  return line;
}

// Prose on the pane's rhythm: the row inset and hairline, wrapping allowed.
TextBlock AppendProse(Panel const& host, hstring const& text, Media::Brush const& brush) {
  Border box;
  box.Padding(ThicknessHelper::FromLengths(12, 10, 12, 10));
  box.BorderBrush(colors::BorderBrush());
  box.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));
  TextBlock prose;
  prose.Text(text);
  prose.FontSize(12);
  prose.TextWrapping(TextWrapping::Wrap);
  prose.Foreground(brush);
  box.Child(prose);
  host.Children().Append(box);
  return prose;
}

// A glyph and a label as a button's content, and the label for a caller that
// changes its word; the caller names the button, since a panel gives it no
// name of its own.
StackPanel MakeGlyphLabel(wchar_t const* glyph, hstring const& label, double glyphSize,
                          TextBlock* outLabel = nullptr) {
  StackPanel content;
  content.Orientation(Orientation::Horizontal);
  content.Spacing(8);
  FontIcon icon;
  icon.FontFamily(Media::FontFamily(kit::kIconFontFamily));
  icon.Glyph(glyph);
  icon.FontSize(glyphSize);
  icon.VerticalAlignment(VerticalAlignment::Center);
  // decoration beside a label that already carries the word
  Automation::AutomationProperties::SetAccessibilityView(
      icon, Automation::Peers::AccessibilityView::Raw);
  content.Children().Append(icon);
  TextBlock text;
  text.Text(label);
  text.VerticalAlignment(VerticalAlignment::Center);
  content.Children().Append(text);
  if (outLabel) *outLabel = text;
  return content;
}

}  // namespace

// ---- the create sheet ---------------------------------------------------------

std::shared_ptr<ApiKeyCreateSheet> ApiKeyCreateSheet::Create(XamlRoot const& root,
                                                             std::function<void()> onCreated) {
  auto sheet = std::shared_ptr<ApiKeyCreateSheet>(new ApiKeyCreateSheet(std::move(onCreated)));
  sheet->Build(root);
  return sheet;
}

ApiKeyCreateSheet::~ApiKeyCreateSheet() {
  // the XAML is gone or going; the key is not left in this object
  flow_.Close();
}

void ApiKeyCreateSheet::Build(XamlRoot const& root) {
  dialog_ = rows::MakeSheet(root, Loc("api_keys_create"));

  StackPanel content;
  content.MinWidth(sheetfit::Width(root, 400));
  content.Spacing(10);

  // the name step: the key's name, required here although a key made through
  // the API may have none
  namePanel_ = StackPanel();
  namePanel_.Spacing(8);
  nameBox_ = TextBox();
  nameBox_.Style(rows::Lookup(L"UrTextInputStyle"));
  nameBox_.Header(winrt::box_value(Loc("api_keys_name_label")));
  nameBox_.PlaceholderText(Loc("api_keys_name_placeholder"));
  // the server's limit; a UTF-16 count is never under the characters it counts
  nameBox_.MaxLength(static_cast<int32_t>(apikeys::kMaxNameLength));
  nameBox_.TextChanged([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->Render();
  });
  namePanel_.Children().Append(nameBox_);
  ring_ = ProgressRing();
  ring_.Width(16);
  ring_.Height(16);
  ring_.IsActive(false);
  ring_.Visibility(Visibility::Collapsed);
  ring_.HorizontalAlignment(HorizontalAlignment::Left);
  Automation::AutomationProperties::SetName(ring_, Loc("loading"));
  namePanel_.Children().Append(ring_);
  failedText_ = TextBlock();
  failedText_.Text(Loc("api_keys_create_failed"));
  failedText_.FontSize(12);
  failedText_.TextWrapping(TextWrapping::Wrap);
  failedText_.Foreground(colors::DangerBrush());
  failedText_.Visibility(Visibility::Collapsed);
  namePanel_.Children().Append(failedText_);
  content.Children().Append(namePanel_);

  // the created step: the warning, the key once, and its copy
  createdPanel_ = StackPanel();
  createdPanel_.Spacing(10);
  createdPanel_.Visibility(Visibility::Collapsed);
  // The warning is the point of the step, so it wears the one colour here
  // that is not text-muted (the seedphrase sheet's).
  TextBlock warning;
  warning.Text(Loc("api_keys_created_warning"));
  warning.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  warning.TextWrapping(TextWrapping::Wrap);
  warning.Foreground(colors::MakeBrush(colors::kUrAmber));
  createdPanel_.Children().Append(warning);
  Border keyFrame;
  keyFrame.BorderBrush(colors::BorderBrush());
  keyFrame.BorderThickness(ThicknessHelper::FromUniformLength(1));
  keyFrame.CornerRadius(CornerRadiusHelper::FromUniformRadius(8));
  keyFrame.Padding(ThicknessHelper::FromUniformLength(12));
  keyText_ = TextBlock();
  keyText_.FontFamily(Media::FontFamily(L"Consolas"));
  keyText_.FontSize(14);
  keyText_.TextWrapping(TextWrapping::Wrap);
  // Not selectable: a selection's Ctrl+C would put the key into Clipboard
  // History and the cloud clipboard, which the copy below keeps it out of.
  keyText_.IsTextSelectionEnabled(false);
  keyFrame.Child(keyText_);
  createdPanel_.Children().Append(keyFrame);
  copyButton_ = Button();
  copyButton_.HorizontalAlignment(HorizontalAlignment::Stretch);
  auto copyContent = MakeGlyphLabel(kCopyGlyph, Loc("api_keys_copy"), 16, &copyLabel_);
  copyContent.HorizontalAlignment(HorizontalAlignment::Center);
  // A copy with no visible or spoken result reads as one that did not happen:
  // the word becomes "Copied" for a moment, and is read out when it does.
  Automation::AutomationProperties::SetLiveSetting(
      copyLabel_, Automation::Peers::AutomationLiveSetting::Polite);
  copyButton_.Content(copyContent);
  Automation::AutomationProperties::SetName(copyButton_, Loc("api_keys_copy"));
  // copying is not closing: the sheet stays until Done
  copyButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->Copy();
  });
  createdPanel_.Children().Append(copyButton_);
  content.Children().Append(createdPanel_);
  copiedTimer_ = dialog_.DispatcherQueue().CreateTimer();
  copiedTimer_.Interval(kCopiedFor);
  copiedTimer_.IsRepeating(false);
  copiedTimer_.Tick([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->ShowCopied(false);
  });

  dialog_.Content(content);
  dialog_.PrimaryButtonClick(
      [weak = weak_from_this()](auto const&, ContentDialogButtonClickEventArgs const& args) {
        args.Cancel(true);  // the sheet stays: the answer decides the next step
        if (auto self = weak.lock()) self->Submit();
      });
  // While the request is out, Esc is refused (the close button is hidden then),
  // so the key that comes back is the key shown. A sign-out's or the page's
  // close (Dismiss) is never refused.
  dialog_.Closing([weak = weak_from_this()](auto const&,
                                            ContentDialogClosingEventArgs const& args) {
    auto self = weak.lock();
    if (!self) return;
    if (!self->dismissed_ && self->flow_.HoldsClose()) args.Cancel(true);
  });
  // closed by any route: the key goes
  dialog_.Closed([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->Clear();
  });
  Render();
}

void ApiKeyCreateSheet::Render() {
  if (flow_.closed()) return;
  const apikeys::CreateStep step = flow_.step();
  const bool creating = step == apikeys::CreateStep::Creating;
  const bool created = step == apikeys::CreateStep::Created;
  dialog_.Title(winrt::box_value(Loc(created ? "api_keys_created_title" : "api_keys_create")));
  namePanel_.Visibility(created ? Visibility::Collapsed : Visibility::Visible);
  nameBox_.IsEnabled(step == apikeys::CreateStep::Name);
  ring_.IsActive(creating);
  ring_.Visibility(creating ? Visibility::Visible : Visibility::Collapsed);
  failedText_.Visibility(step == apikeys::CreateStep::Name && flow_.failed()
                             ? Visibility::Visible
                             : Visibility::Collapsed);
  createdPanel_.Visibility(created ? Visibility::Visible : Visibility::Collapsed);
  if (created) {
    keyText_.Text(H(flow_.secret()));
    dialog_.PrimaryButtonText(hstring{});
    dialog_.CloseButtonText(Loc("done"));
    dialog_.DefaultButton(ContentDialogButton::Close);
    return;
  }
  dialog_.PrimaryButtonText(Loc("create"));
  dialog_.IsPrimaryButtonEnabled(flow_.CanCreate(Narrow(nameBox_.Text().c_str())));
  // no close button while the request is out
  dialog_.CloseButtonText(creating ? hstring{} : Loc("cancel"));
  dialog_.DefaultButton(ContentDialogButton::Primary);
}

void ApiKeyCreateSheet::Submit() {
  // the session may have ended under the sheet
  if (!Sdk().apiReady() || !Sdk().IsLoggedIn()) return;
  const std::optional<std::string> name = flow_.Begin(Narrow(nameBox_.Text().c_str()));
  if (!name) return;
  Render();
  urnet::CreateApiKeyArgs args;
  args.name = *name;
  auto queue = dialog_.DispatcherQueue();
  auto weak = weak_from_this();
  try {
    Sdk().api().createApiKey(args, [queue, weak](std::optional<urnet::CreateApiKeyResult> result,
                                                 std::optional<std::string> err) {
      // The key moves out of the sdk's result here, into the one value that
      // crosses to the UI thread, and is wiped with it whatever happens next:
      // shown, refused by a closed sheet, or never delivered.
      auto outcome =
          std::make_shared<apikeys::CreateOutcome>(apikeys::CreateOutcomeFrom(result, err));
      if (!outcome->ok) LogWarn("api keys: create failed: {}", outcome->error);
      queue.TryEnqueue([weak, outcome] {
        if (auto self = weak.lock()) self->Apply(*outcome);
      });
    });
  } catch (std::exception const& e) {
    LogError("api keys: could not create a key: {}", e.what());
    apikeys::CreateOutcome failed;
    failed.error = e.what();
    Apply(failed);
  }
}

void ApiKeyCreateSheet::Apply(apikeys::CreateOutcome& outcome) {
  const bool created = outcome.ok;
  // the flow takes the key to show it, or wipes it
  flow_.Apply(outcome);
  if (!flow_.closed()) {
    Render();
    if (flow_.step() == apikeys::CreateStep::Created) {
      copyButton_.Focus(FocusState::Programmatic);
    }
  }
  // the server has the key whether or not it was shown: the list says so
  if (created && onCreated_) onCreated_();
}

void ApiKeyCreateSheet::Copy() {
  if (flow_.step() != apikeys::CreateStep::Created || flow_.secret().empty()) return;
  namespace dt = winrt::Windows::ApplicationModel::DataTransfer;
  try {
    dt::DataPackage package;
    package.SetText(H(flow_.secret()));
    // A full account credential stays out of Clipboard History (Win+V,
    // readable by anything that asks) and off the cloud clipboard, which would
    // sync it to the Microsoft account and to other machines. Both are on for
    // a plain SetContent; the seedphrase's copy sets the same two
    // (SeedphraseDisplaySheet::CopyToClipboard).
    dt::ClipboardContentOptions options;
    options.IsAllowedInHistory(false);
    options.IsRoamable(false);
    if (!dt::Clipboard::SetContentWithOptions(package, options)) {
      // never the fallback to SetContent: that is the leak this avoids
      LogError("api keys: the clipboard refused a history-excluded copy");
      return;
    }
  } catch (winrt::hresult_error const&) {
    LogWarn("api keys: the clipboard could not be written");
    return;
  }
  ShowCopied(true);
}

void ApiKeyCreateSheet::ShowCopied(bool copied) {
  if (!copyLabel_) return;
  const hstring word = Loc(copied ? "api_keys_copied" : "api_keys_copy");
  copyLabel_.Text(word);
  Automation::AutomationProperties::SetName(copyButton_, word);
  if (!copiedTimer_) return;
  copiedTimer_.Stop();
  if (copied) copiedTimer_.Start();
}

void ApiKeyCreateSheet::Clear() {
  // the key is wiped, and so is an answer that comes later
  flow_.Close();
  if (keyText_) keyText_.Text(hstring{});
  if (copiedTimer_) copiedTimer_.Stop();
}

void ApiKeyCreateSheet::Dismiss() {
  dismissed_ = true;
  // at once: the dialog's Closed comes later, if at all
  Clear();
  if (!dialog_) return;
  try {
    dialog_.Hide();
  } catch (...) {
    // the dialog is already gone with its window
  }
}

// ---- the page -------------------------------------------------------------------

ApiKeysPage::ApiKeysPage(winrt::URnetwork::implementation::MainWindow& window) : w_(window) {}

ApiKeysPage::~ApiKeysPage() {
  *alive_ = false;  // the answers still queued stop here
  model_.Close();
  try {
    if (createSheet_) createSheet_->Dismiss();
  } catch (...) {
    // the window is shutting down; the sheet's key is wiped with it
  }
  createSheet_.reset();
}

void ApiKeysPage::Build() {
  if (built_) return;
  built_ = true;
  w_.ApiKeysBackButton().Click([this](auto const&, auto const&) { w_.CloseApiKeys(); });
  w_.ApiKeysRefreshButton().Click([this](auto const&, auto const&) { Refresh(); });

  // the intro and the API reference, then Create, above the list
  auto host = w_.ApiKeysIntroHost();
  Border box;
  box.Padding(ThicknessHelper::FromLengths(12, 10, 12, 10));
  box.BorderBrush(colors::BorderBrush());
  box.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));
  StackPanel intro;
  intro.Spacing(4);
  introText_ = TextBlock();
  introText_.FontSize(13);
  introText_.TextWrapping(TextWrapping::Wrap);
  introText_.Foreground(colors::MutedBrush());
  intro.Children().Append(introText_);
  docsLink_ = HyperlinkButton();
  docsLink_.NavigateUri(winrt::Windows::Foundation::Uri(apikeys::kDocsUrl));
  docsLink_.FontSize(13);
  docsLink_.Padding(ThicknessHelper::FromLengths(0, 0, 0, 0));
  intro.Children().Append(docsLink_);
  box.Child(intro);
  host.Children().Append(box);

  createButton_ = Button();
  createButton_.Style(rows::Lookup(L"UrPaneActionPrimaryStyle"));
  Automation::AutomationProperties::SetAutomationId(createButton_, L"acceptance.api-keys.create");
  createButton_.Click([this](auto const&, auto const&) { ShowCreateSheet(); });
  host.Children().Append(createButton_);
}

void ApiKeysPage::ApplyStrings() {
  Build();
  w_.ApiKeysPaneTitle().Text(Loc("api_keys_title"));
  Automation::AutomationProperties::SetName(w_.ApiKeysPane(), Loc("api_keys_title"));
  // "‹ Account": the page has no rail item; this is the way back
  w_.ApiKeysBackButton().Content(winrt::box_value(hstring{L"‹ " + std::wstring{Loc("account")}}));
  Automation::AutomationProperties::SetName(w_.ApiKeysBackButton(), Loc("account"));
  w_.ApiKeysRefreshText().Text(Loc("refresh"));
  // the button's content is a panel, which gives it no name of its own
  Automation::AutomationProperties::SetName(w_.ApiKeysRefreshButton(), Loc("refresh"));
  introText_.Text(Loc("api_keys_intro"));
  docsLink_.Content(winrt::box_value(Loc("api_keys_docs_link")));
  createButton_.Content(MakeGlyphLabel(kAddGlyph, Loc("api_keys_create"), 14));
  Automation::AutomationProperties::SetName(createButton_, Loc("api_keys_create"));
}

// ---- lifecycle ---------------------------------------------------------------

bool ApiKeysPage::HasSession() const {
  // IsLoggedIn(), not apiReady(): apiReady is api_.has_value(), set at SDK init
  // rather than at sign-in. --preview-ui has no session either.
  return !w_.previewUi() && Sdk().apiReady() && Sdk().IsLoggedIn();
}

void ApiKeysPage::Open() {
  Build();
  open_ = true;
  // a page opened again starts over, and an earlier open's answers are dropped
  model_.Open();
  rendered_.reset();
  session_ = HasSession();
  if (session_) Load();
  Render(/*force=*/true);
}

void ApiKeysPage::Close() {
  open_ = false;
  session_ = false;
  model_.Close();  // every answer still out is dropped
  // a sign-out closes a create sheet still up, and its key goes
  if (createSheet_) createSheet_->Dismiss();
  rendered_.reset();
  rowControls_.clear();
  tryAgainButton_ = nullptr;
  w_.ApiKeysHost().Children().Clear();
  w_.ApiKeysRefreshRing().IsActive(false);
  w_.ApiKeysRefreshRing().Visibility(Visibility::Collapsed);
}

void ApiKeysPage::ResetForSignOut() { Close(); }

void ApiKeysPage::Load() {
  if (!open_ || !session_) return;
  const uint64_t generation = model_.Current();
  const uint64_t load = model_.BeginLoad();
  Render(/*force=*/false);
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  auto alive = alive_;
  try {
    // The sdk answers on its own thread. The answer is read there, and only
    // the plain value crosses to the UI thread.
    Sdk().api().listApiKeys([queue, weak, alive, generation, load](
                                std::optional<urnet::ListApiKeysResult> result,
                                std::optional<std::string> err) {
      apikeys::ListOutcome outcome = apikeys::ListOutcomeFrom(result, err);
      queue.TryEnqueue([weak, alive, generation, load, outcome = std::move(outcome)]() mutable {
        if (!*alive) return;
        if (auto self = weak.get()) self->apiKeys().ApplyList(generation, load, std::move(outcome));
      });
    });
  } catch (std::exception const& e) {
    LogError("api keys: could not list the keys: {}", e.what());
    apikeys::ListOutcome failed;
    failed.error = e.what();
    ApplyList(generation, load, std::move(failed));
  }
}

void ApiKeysPage::ApplyList(uint64_t generation, uint64_t load, apikeys::ListOutcome outcome) {
  // for a page this one has closed: a sign-out, or another account's
  if (!model_.Admits(generation)) return;
  // the page shows its own words, never this message
  if (!outcome.ok) LogWarn("api keys: listing failed: {}", outcome.error);
  if (!model_.ApplyLoad(load, std::move(outcome))) return;
  Render(/*force=*/false);
}

// ---- rendering ----------------------------------------------------------------

void ApiKeysPage::Render(bool force) {
  if (!built_ || !open_) return;
  auto host = w_.ApiKeysHost();
  // nothing to create or refresh without a session
  createButton_.Visibility(session_ ? Visibility::Visible : Visibility::Collapsed);
  w_.ApiKeysRefreshButton().IsEnabled(session_);
  if (!session_) {
    // signed out or --preview-ui: nothing to list, and the page says why
    rendered_.reset();
    rowControls_.clear();
    tryAgainButton_ = nullptr;
    host.Children().Clear();
    AppendProse(host, Loc("please_login_to_urnetwork"), colors::FaintBrush());
    w_.ApiKeysRefreshRing().IsActive(false);
    w_.ApiKeysRefreshRing().Visibility(Visibility::Collapsed);
    return;
  }
  apikeys::View view = apikeys::ViewFor(model_.state(), ApiKeysText());
  // a reload over the rows: the rows stay and the header says it is running
  w_.ApiKeysRefreshRing().IsActive(view.refreshing);
  w_.ApiKeysRefreshRing().Visibility(view.refreshing ? Visibility::Visible
                                                     : Visibility::Collapsed);
  view.refreshing = false;  // the header's alone
  if (!force && rendered_ && *rendered_ == view) return;
  RenderBody(view);
  rendered_ = std::move(view);
}

void ApiKeysPage::RenderBody(apikeys::View const& view) {
  const FocusTarget focused = FocusedTarget();
  auto host = w_.ApiKeysHost();
  host.Children().Clear();
  rowControls_.clear();
  tryAgainButton_ = nullptr;

  switch (view.body) {
    case apikeys::Body::Progress: {
      // never loaded: progress, in the middle of where the list will be
      StackPanel progress;
      progress.HorizontalAlignment(HorizontalAlignment::Center);
      progress.Margin(ThicknessHelper::FromLengths(0, 32, 0, 32));
      ProgressRing ring;
      ring.IsActive(true);
      ring.Width(24);
      ring.Height(24);
      Automation::AutomationProperties::SetName(ring, Loc("loading"));
      progress.Children().Append(ring);
      host.Children().Append(progress);
      break;
    }
    case apikeys::Body::LoadFailed: {
      AppendProse(host, Loc("api_keys_load_failed"), colors::DangerBrush());
      tryAgainButton_ = Button();
      tryAgainButton_.Content(winrt::box_value(Loc("retry")));
      tryAgainButton_.Style(rows::Lookup(L"UrPaneActionSecondaryStyle"));
      tryAgainButton_.Click([this](auto const&, auto const&) { Refresh(); });
      host.Children().Append(tryAgainButton_);
      break;
    }
    case apikeys::Body::Empty:
      // the last list stays through a failed reload, under this notice
      if (view.refreshFailed) {
        AppendProse(host, Loc("api_keys_load_failed"), colors::DangerBrush());
      }
      AppendProse(host, Loc("api_keys_empty"), colors::MutedBrush());
      break;
    case apikeys::Body::Rows:
      if (view.refreshFailed) {
        AppendProse(host, Loc("api_keys_load_failed"), colors::DangerBrush());
      }
      for (auto const& row : view.rows) AppendRow(host, row);
      break;
  }
  RestoreFocus(focused);
}

// One key: its mark, its name, when it was created and its id, and Delete.
void ApiKeysPage::AppendRow(Panel const& host, apikeys::Row const& row) {
  Border box;
  box.MinHeight(kRowMinHeight);
  box.Padding(ThicknessHelper::FromLengths(12, 10, 12, 10));
  box.BorderBrush(colors::BorderBrush());
  box.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));

  Grid grid;
  grid.ColumnSpacing(12);
  ColumnDefinition markColumn, textColumn, actionColumn;
  markColumn.Width(GridLengthHelper::Auto());
  textColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
  actionColumn.Width(GridLengthHelper::Auto());
  grid.ColumnDefinitions().Append(markColumn);
  grid.ColumnDefinitions().Append(textColumn);
  grid.ColumnDefinitions().Append(actionColumn);
  auto mark = kit::MakeRowGlyph(kKeyGlyph);
  mark.VerticalAlignment(VerticalAlignment::Center);
  grid.Children().Append(mark);

  StackPanel text;
  text.Spacing(2);
  text.VerticalAlignment(VerticalAlignment::Center);
  Grid::SetColumn(text, 1);
  // the name, or "Unnamed key"
  text.Children().Append(MakeLine(row.name, L"UrRowTitleStyle"));
  // when it was created, read out with the full date and time; no line when
  // the server sent no time
  if (!row.created.empty()) {
    text.Children().Append(MakeLine(row.created, L"UrRowNoteStyle", row.createdSpoken));
  }
  // its id, for use with the API: selectable, since the id is no secret
  auto idLine = MakeLine(row.idText, L"UrRowNoteStyle");
  idLine.IsTextSelectionEnabled(true);
  text.Children().Append(idLine);
  // the row's own delete error; Delete is offered again
  if (row.del == apikeys::DeleteState::Failed) {
    TextBlock failed;
    failed.Text(Loc("api_keys_delete_failed"));
    failed.FontSize(11);
    failed.TextWrapping(TextWrapping::Wrap);
    failed.Foreground(colors::DangerBrush());
    text.Children().Append(failed);
  }
  grid.Children().Append(text);

  // Delete: progress while the request is out, and disabled then, which with
  // the single-sheet gate and the model's refusal keeps a delete from going
  // out twice
  StackPanel action;
  action.Orientation(Orientation::Horizontal);
  action.Spacing(8);
  action.VerticalAlignment(VerticalAlignment::Center);
  Grid::SetColumn(action, 2);
  const bool pending = row.del == apikeys::DeleteState::Pending;
  if (pending) {
    ProgressRing ring;
    ring.IsActive(true);
    ring.Width(16);
    ring.Height(16);
    Automation::AutomationProperties::SetAccessibilityView(
        ring, Automation::Peers::AccessibilityView::Raw);
    action.Children().Append(ring);
  }
  Button del;
  del.Content(winrt::box_value(Loc("api_keys_delete")));
  del.IsEnabled(!pending);
  // which key: every row's Delete has the one name
  Automation::AutomationProperties::SetName(del, Loc("api_keys_delete"));
  Automation::AutomationProperties::SetFullDescription(del, H(row.name));
  const std::string keyId = row.id;
  del.Click([this, keyId](auto const&, auto const&) { ConfirmDelete(keyId); });
  action.Children().Append(del);
  grid.Children().Append(action);

  box.Child(grid);
  host.Children().Append(box);
  rowControls_.push_back(RowControls{keyId, del});
}

// ---- focus ------------------------------------------------------------------

ApiKeysPage::FocusTarget ApiKeysPage::FocusedTarget() const {
  FocusTarget target;
  auto root = w_.ApiKeysHost().XamlRoot();
  if (!root) return target;
  const IInspectable focused = Input::FocusManager::GetFocusedElement(root);
  if (!focused) return target;
  for (auto const& row : rowControls_) {
    if (row.del && focused == row.del) return {FocusKind::Delete, row.keyId};
  }
  if (tryAgainButton_ && focused == tryAgainButton_) return {FocusKind::TryAgain, {}};
  return target;
}

// The same control in the rebuilt body; when it is gone (its key was deleted)
// or disabled (its delete is running), Refresh, which is always there.
void ApiKeysPage::RestoreFocus(FocusTarget const& target) {
  if (target.kind == FocusKind::None) return;
  const auto focus = [](Button const& button) {
    return button && button.IsEnabled() && button.Focus(FocusState::Programmatic);
  };
  for (auto const& row : rowControls_) {
    if (row.keyId == target.keyId && target.kind == FocusKind::Delete && focus(row.del)) return;
  }
  if (target.kind == FocusKind::TryAgain && focus(tryAgainButton_)) return;
  w_.ApiKeysRefreshButton().Focus(FocusState::Programmatic);
}

// ---- actions ----------------------------------------------------------------

void ApiKeysPage::Refresh() { Load(); }

winrt::fire_and_forget ApiKeysPage::ShowCreateSheet() {
  if (w_.sheetOpen() || !open_ || !session_) co_return;
  // the open the sheet is for: its key's reload goes to this list only
  const uint64_t generation = model_.Current();
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  try {
    auto weak = w_.get_weak();
    createSheet_ = ApiKeyCreateSheet::Create(self->Content().XamlRoot(), [weak, generation] {
      if (auto window = weak.get()) window->apiKeys().OnCreated(generation);
    });
    co_await createSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  // the sheet wiped its key as it closed
  createSheet_.reset();
  w_.SetSheetOpen(false);
  // back on the button that opened it
  if (open_ && createButton_) createButton_.Focus(FocusState::Programmatic);
}

void ApiKeysPage::OnCreated(uint64_t generation) {
  if (!open_ || !model_.Admits(generation)) return;
  Load();
}

// Names the key, defaults to Cancel and deletes only on the explicit button
// (the Sessions page's sign-out confirmation).
winrt::fire_and_forget ApiKeysPage::ConfirmDelete(std::string keyId) {
  if (w_.sheetOpen() || !rendered_) co_return;
  std::optional<apikeys::Confirmation> confirmation;
  for (auto const& row : rendered_->rows) {
    if (row.id == keyId) confirmation = apikeys::DeleteConfirmationFor(row, ApiKeysText());
  }
  if (!confirmation) co_return;
  // the list this confirmation is for
  const uint64_t generation = model_.Current();
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  bool confirmed = false;
  try {
    auto dialog = rows::MakeSheet(self->Content().XamlRoot(), H(confirmation->title));
    dialog.PrimaryButtonText(Loc("api_keys_delete"));
    dialog.CloseButtonText(Loc("cancel"));
    dialog.DefaultButton(ContentDialogButton::Close);  // Enter must not delete
    TextBlock body;
    body.Text(H(confirmation->body));
    body.FontSize(14);
    body.TextWrapping(TextWrapping::Wrap);
    body.MinWidth(320);
    dialog.Content(body);
    confirmed = co_await dialog.ShowAsync() == ContentDialogResult::Primary;
  } catch (...) {
  }
  w_.SetSheetOpen(false);
  if (confirmed) Delete(keyId, generation);
  // back on the row the dialog was for, or Refresh once it is gone
  RestoreFocus({FocusKind::Delete, keyId});
}

void ApiKeysPage::Delete(std::string const& keyId, uint64_t generation) {
  // Closed, or opened again, while the dialog was up: it was for a list that
  // is no longer on screen. A delete already out for the key is not repeated.
  if (!open_ || !session_ || !model_.Admits(generation)) return;
  if (!model_.BeginDelete(keyId)) return;
  Render(/*force=*/false);
  urnet::DeleteApiKeyArgs args;
  args.id = keyId;
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  auto alive = alive_;
  try {
    Sdk().api().deleteApiKey(args, [queue, weak, alive, generation, keyId](
                                       std::optional<urnet::DeleteApiKeyResult> result,
                                       std::optional<std::string> err) {
      const apikeys::DeleteOutcome outcome = apikeys::DeleteOutcomeFrom(result, err);
      queue.TryEnqueue([weak, alive, generation, keyId, outcome] {
        if (!*alive) return;
        if (auto self = weak.get()) self->apiKeys().ApplyDelete(generation, keyId, outcome);
      });
    });
  } catch (std::exception const& e) {
    LogError("api keys: could not delete a key: {}", e.what());
    apikeys::DeleteOutcome failed;
    failed.error = e.what();
    ApplyDelete(generation, keyId, failed);
  }
}

void ApiKeysPage::ApplyDelete(uint64_t generation, std::string const& keyId,
                              apikeys::DeleteOutcome const& outcome) {
  if (!model_.Admits(generation)) return;
  if (!outcome.ok) LogWarn("api keys: delete failed: {}", outcome.error);
  // a deleted key leaves the list at once; a failure shows on its row
  model_.ApplyDelete(keyId, outcome.ok);
  Render(/*force=*/false);
  // the list as the server has it now
  if (outcome.ok) Load();
}

}  // namespace urnw
