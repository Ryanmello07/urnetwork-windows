// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "AuthSheets.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <vector>

#include <nlohmann/json.hpp>

#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.Web.WebView2.Core.h>

#include "BalanceSheets.h"  // SetTermsMarkerText (the terms/privacy link inlines)
#include "Ids.h"
#include "Localization.h"
#include "PageContext.h"
#include "Log.h"
#include "Paths.h"      // StorageRoot: the WebView2 user-data folder
#include "SheetFit.h"   // sheetfit: sheets clamp to the window at open time
#include "Strings.h"
#include "UrColors.h"
#include "UrComponents.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

// NOTE on captures: control event handlers capture the owning sheet weakly
// (see StatsSheets.cpp). The window holds the sheet's shared_ptr while the
// dialog is showing, so lock() always succeeds during interaction.

namespace urnw {
namespace {

hstring H(std::string const& s) { return winrt::to_hstring(s); }

// A UI string from the shared localization store, by key id (Localization.h).
hstring Loc(std::string_view key) { return hstring{Localized(key)}; }
// the same, boxed for a ContentControl's Content
winrt::Windows::Foundation::IInspectable LocBox(std::string_view key) {
  return winrt::box_value(Loc(key));
}

// The SsoSheet's Apple driver page: a document at the https://ur.io origin
// (served from disk through a virtual host mapping) that window.open()s the
// web_message authorize url and forwards Apple's oauthDone web message to the
// host. This is the ur.io website's AppleID.auth SDK opener (mmm ur.io
// ConnectDialog) reduced to the two calls of the SDK that matter here.
// Apple's completion page accepts only an opener whose origin matches the
// registered redirect's, which is why the page must BE https://ur.io.
constexpr const char* kAppleDriverPageHead = R"HTML(<!doctype html>
<meta charset="utf-8">
<title>Sign in with Apple</title>
<script>
window.addEventListener('message', function (e) {
  if (e.origin !== 'https://appleid.apple.com') return;
  try {
    var m = JSON.parse(e.data);
    if (!m || m.method !== 'oauthDone' || !m.data) return;
    window.chrome.webview.postMessage(JSON.stringify(m.data));
  } catch (err) {}
});
window.open(')HTML";
// the authorize url is inlined between the head and tail; its characters are
// safe in a js string literal by construction — every parameter value came off
// WalletConnect's Esc ([A-Za-z0-9-._~%]) and the fixed parts carry no quote
// or backslash
constexpr const char* kAppleDriverPageTail = R"HTML(', '_blank');
</script>
)HTML";

}  // namespace

// ---- GuestModeSheet ---------------------------------------------------------

std::shared_ptr<GuestModeSheet> GuestModeSheet::Create(XamlRoot const& root,
                                                       SdkHost& sdk) {
  auto sheet = std::shared_ptr<GuestModeSheet>(new GuestModeSheet(sdk));
  sheet->Build(root);
  return sheet;
}

void GuestModeSheet::Build(XamlRoot const& root) {
  dialog_ = ContentDialog();
  dialog_.XamlRoot(root);
  dialog_.Title(winrt::box_value(Loc("try_guest_mode_2")));
  dialog_.CloseButtonText(Loc("close"));
  // brand sheet surface (android SheetBlack; BalanceSheets::MakeDialog)
  dialog_.Background(colors::SheetBrush());
  dialog_.PrimaryButtonText(Loc("enter_urnetwork"));
  dialog_.IsPrimaryButtonEnabled(false);  // gated on the terms consent
  dialog_.DefaultButton(ContentDialogButton::Primary);

  StackPanel content;
  content.MinWidth(sheetfit::Width(root, 400));
  content.Spacing(12);

  // what guest mode is, and that a full account can come later
  TextBlock explainer;
  explainer.Text(Loc("guest_mode_explainer"));
  explainer.FontSize(13);
  explainer.Foreground(colors::MutedBrush());
  explainer.TextWrapping(TextWrapping::Wrap);
  content.Children().Append(explainer);

  // terms consent: checkbox + the tappable terms/privacy links (the same
  // terms_checkbox string the create step renders)
  Grid termsRow;
  ColumnDefinition c0, c1;
  c0.Width(GridLength{0, GridUnitType::Auto});
  c1.Width(GridLength{1, GridUnitType::Star});
  termsRow.ColumnDefinitions().Append(c0);
  termsRow.ColumnDefinitions().Append(c1);
  termsRow.ColumnSpacing(8);

  termsCheck_ = CheckBox();
  termsCheck_.MinWidth(0);
  termsCheck_.VerticalAlignment(VerticalAlignment::Top);
  auto onTermsChanged = [weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      const bool agreed =
          self->termsCheck_.IsChecked() && self->termsCheck_.IsChecked().Value();
      self->dialog_.IsPrimaryButtonEnabled(agreed && !self->creating_);
      self->errorText_.Visibility(Visibility::Collapsed);
    }
  };
  termsCheck_.Checked(onTermsChanged);
  termsCheck_.Unchecked(onTermsChanged);
  termsRow.Children().Append(termsCheck_);

  TextBlock termsText;
  termsText.VerticalAlignment(VerticalAlignment::Center);
  termsText.Foreground(colors::MutedBrush());
  SetTermsMarkerText(termsText, Localized("terms_checkbox"), 12);
  // this checkbox had no name at all in the UIA tree; same treatment as the
  // two on the login page
  PairTermsLabel(termsCheck_, termsText);
  Grid::SetColumn(termsText, 1);
  termsRow.Children().Append(termsText);
  content.Children().Append(termsRow);

  // optional referral code (android/apple instant-account parity): the server
  // links the referral on the guest create path too
  codeBox_ = TextBox();
  codeBox_.Header(winrt::box_value(Loc("bonus_referral_code_label")));
  codeBox_.PlaceholderText(Loc("enter_a_bonus_referral_code"));
  if (auto style = Application::Current()
                       .Resources()
                       .TryLookup(winrt::box_value(L"UrTextInputStyle"))
                       .try_as<Style>()) {
    codeBox_.Style(style);
  }
  codeBox_.TextChanged([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->codeStatus_.Visibility(Visibility::Collapsed);
  });
  content.Children().Append(codeBox_);

  codeStatus_ = TextBlock();
  codeStatus_.FontSize(12);
  codeStatus_.Foreground(colors::DangerBrush());
  codeStatus_.TextWrapping(TextWrapping::Wrap);
  codeStatus_.Visibility(Visibility::Collapsed);
  content.Children().Append(codeStatus_);

  errorText_ = TextBlock();
  errorText_.FontSize(12);
  errorText_.Foreground(colors::DangerBrush());
  errorText_.TextWrapping(TextWrapping::Wrap);
  errorText_.Visibility(Visibility::Collapsed);
  content.Children().Append(errorText_);

  dialog_.Content(content);

  dialog_.PrimaryButtonClick([weak = weak_from_this()](
                                 auto const&, ContentDialogButtonClickEventArgs const& args) {
    args.Cancel(true);  // keep the dialog open; ApplyResult decides what shows next
    if (auto self = weak.lock()) self->Submit();
  });
}

void GuestModeSheet::Submit() {
  const bool agreed = termsCheck_.IsChecked() && termsCheck_.IsChecked().Value();
  if (creating_ || !agreed || !sdk_.apiReady()) return;
  creating_ = true;
  dialog_.IsPrimaryButtonEnabled(false);
  termsCheck_.IsEnabled(false);
  errorText_.Visibility(Visibility::Collapsed);
  codeStatus_.Visibility(Visibility::Collapsed);

  auto queue = dialog_.DispatcherQueue();
  auto weak = weak_from_this();

  const std::string code = pages::TrimWhitespace(Narrow(codeBox_.Text().c_str()));
  if (code.empty()) {
    sdk_.LoginAsGuest([queue, weak](AuthResult r) {
      queue.TryEnqueue([weak, r] {
        if (auto self = weak.lock()) self->ApplyResult(r.ok, r.error);
      });
    });
    return;
  }

  // a code was entered: it must validate before the create, so a typo shows
  // an error instead of silently dropping the bonus
  urnet::ValidateReferralCodeArgs args;
  args.referral_code = code;
  sdk_.api().validateReferralCode(
      args, [queue, weak, code](std::optional<urnet::ValidateReferralCodeResult> result,
                                std::optional<std::string> err) {
        const bool ok = !err && result.has_value();
        const bool valid = ok && result->is_valid;
        const bool capped = ok && result->is_capped;
        queue.TryEnqueue([weak, code, ok, valid, capped] {
          auto self = weak.lock();
          if (!self) return;
          if (!ok || !valid || capped) {
            self->creating_ = false;
            self->termsCheck_.IsEnabled(true);
            self->dialog_.IsPrimaryButtonEnabled(true);
            self->codeStatus_.Text(!ok    ? Loc("something_went_wrong")
                                   : capped ? Loc("referral_code_capped")
                                            : Loc("invalid_referral_code"));
            self->codeStatus_.Visibility(Visibility::Visible);
            return;
          }
          auto queue2 = self->dialog_.DispatcherQueue();
          auto weak2 = self->weak_from_this();
          self->sdk_.LoginAsGuest(
              [queue2, weak2](AuthResult r) {
                queue2.TryEnqueue([weak2, r] {
                  if (auto inner = weak2.lock()) inner->ApplyResult(r.ok, r.error);
                });
              },
              code);
        });
      });
}

void GuestModeSheet::ApplyResult(bool ok, std::string const& error) {
  creating_ = false;
  if (ok) {
    // the auth-state relay swaps the login panel for the home view underneath
    dialog_.Hide();
    return;
  }
  termsCheck_.IsEnabled(true);
  dialog_.IsPrimaryButtonEnabled(termsCheck_.IsChecked() &&
                                 termsCheck_.IsChecked().Value());
  // a server error is not localizable; show it when there is one
  errorText_.Text(error.empty() ? Loc("guest_mode_failed") : H(error));
  errorText_.Visibility(Visibility::Visible);
}

// ---- SeedphraseDisplaySheet -------------------------------------------------

namespace {

// Split a normalized phrase into its words.
std::vector<std::string> SeedWords(std::string const& phrase) {
  std::vector<std::string> words;
  size_t i = 0;
  while (i < phrase.size()) {
    while (i < phrase.size() && std::isspace(static_cast<unsigned char>(phrase[i]))) ++i;
    const size_t start = i;
    while (i < phrase.size() && !std::isspace(static_cast<unsigned char>(phrase[i]))) ++i;
    if (i > start) words.push_back(phrase.substr(start, i - start));
  }
  return words;
}

}  // namespace

std::shared_ptr<SeedphraseDisplaySheet> SeedphraseDisplaySheet::Create(
    XamlRoot const& root, std::string const& seedphrase, std::function<void()> onCopied,
    std::function<void()> onConfirmed) {
  auto sheet = std::shared_ptr<SeedphraseDisplaySheet>(new SeedphraseDisplaySheet(
      seedphrase, std::move(onCopied), std::move(onConfirmed)));
  sheet->Build(root);
  return sheet;
}

void SeedphraseDisplaySheet::Build(XamlRoot const& root) {
  dialog_ = ContentDialog();
  dialog_.XamlRoot(root);
  dialog_.Title(winrt::box_value(Loc("secure_your_account")));
  dialog_.Background(colors::SheetBrush());
  // Leaving CloseButtonText unset is NOT a dismissal guard. ContentDialog binds
  // Esc to its own close regardless of whether a close button exists, and the
  // Esc that resulted dropped a network the server had ALREADY minted — the
  // seedphrase on screen was the only way back into it and had not been
  // written down. The Closing handler below is the actual guard; macOS's
  // .interactiveDismissDisabled(true) is the same idea expressed as a modifier.
  dialog_.PrimaryButtonText(Loc("seedphrase_saved_confirm"));
  dialog_.DefaultButton(ContentDialogButton::Primary);

  StackPanel content;
  content.MinWidth(sheetfit::Width(root, 400));
  content.Spacing(10);

  TextBlock title;
  title.Text(Loc("your_seedphrase"));
  title.FontSize(20);
  title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  content.Children().Append(title);

  // The warning is the point of the screen, so it wears the one colour on this
  // sheet that is not text-muted.
  TextBlock warning;
  warning.Text(Loc("seedphrase_only_time"));
  warning.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  warning.TextWrapping(TextWrapping::Wrap);
  warning.Foreground(colors::MakeBrush(colors::kUrAmber));
  content.Children().Append(warning);

  TextBlock instructions;
  instructions.Text(Loc("seedphrase_store_safely"));
  instructions.FontSize(12);
  instructions.TextWrapping(TextWrapping::Wrap);
  instructions.Foreground(colors::MutedBrush());
  content.Children().Append(instructions);

  // Numbered two-column word grid (macOS wordGridView). Numbering matters:
  // order is part of the credential, and a bare wrapped paragraph gives the
  // user no way to check they transcribed it in sequence.
  const auto words = SeedWords(seedphrase_);
  Border gridFrame;
  gridFrame.BorderBrush(colors::BorderBrush());
  gridFrame.BorderThickness(ThicknessHelper::FromUniformLength(1));
  gridFrame.CornerRadius(CornerRadiusHelper::FromUniformRadius(8));
  gridFrame.Padding(ThicknessHelper::FromUniformLength(12));

  Grid grid;
  grid.ColumnSpacing(8);
  grid.RowSpacing(6);
  for (int c = 0; c < 2; ++c) {
    ColumnDefinition column;
    column.Width(GridLength{1, GridUnitType::Star});
    grid.ColumnDefinitions().Append(column);
  }
  const size_t rows = (words.size() + 1) / 2;
  for (size_t r = 0; r < rows; ++r) grid.RowDefinitions().Append(RowDefinition());
  for (size_t i = 0; i < words.size(); ++i) {
    Border cell;
    cell.Background(colors::CardBrush());
    cell.CornerRadius(CornerRadiusHelper::FromUniformRadius(6));
    cell.Padding(ThicknessHelper::FromLengths(8, 6, 8, 6));

    StackPanel row;
    row.Orientation(Orientation::Horizontal);
    row.Spacing(6);

    TextBlock ordinal;
    ordinal.Text(winrt::hstring{std::to_wstring(i + 1) + L"."});
    ordinal.FontFamily(Media::FontFamily(L"Consolas"));
    ordinal.FontSize(12);
    ordinal.MinWidth(22);
    ordinal.TextAlignment(TextAlignment::Right);
    ordinal.Foreground(colors::MutedBrush());
    row.Children().Append(ordinal);

    TextBlock word;
    word.Text(H(words[i]));
    word.FontFamily(Media::FontFamily(L"Consolas"));
    word.FontSize(13);
    row.Children().Append(word);

    cell.Child(row);
    // fill column-major down each half, so reading top-to-bottom in the left
    // column then the right gives 1..n (macOS LazyVGrid fills row-major; a
    // 2-up desktop grid reads more naturally in columns)
    Grid::SetRow(cell, static_cast<int32_t>(i % rows));
    Grid::SetColumn(cell, static_cast<int32_t>(i / rows));
    grid.Children().Append(cell);
  }
  gridFrame.Child(grid);
  content.Children().Append(gridFrame);

  // Copy is a SECONDARY action with a leading copy glyph, inside the content
  // rather than on the dialog's button row: the primary (confirm) is this
  // sheet's one call to action. A plain Button is the secondary look; only
  // the confirm carries the accent.
  Button copyButton;
  copyButton.HorizontalAlignment(HorizontalAlignment::Stretch);
  StackPanel copyContent;
  copyContent.Orientation(Orientation::Horizontal);
  copyContent.Spacing(8);
  copyContent.HorizontalAlignment(HorizontalAlignment::Center);
  FontIcon copyGlyph;
  copyGlyph.FontFamily(Media::FontFamily(L"Segoe Fluent Icons"));
  copyGlyph.Glyph(L"\uE8C8");  // Copy
  copyGlyph.FontSize(16);
  copyGlyph.VerticalAlignment(VerticalAlignment::Center);
  // decoration beside a label that already carries the word
  Automation::AutomationProperties::SetAccessibilityView(
      copyGlyph, Automation::Peers::AccessibilityView::Raw);
  copyContent.Children().Append(copyGlyph);
  TextBlock copyLabel;
  copyLabel.Text(Loc("copy_to_clipboard"));
  copyLabel.VerticalAlignment(VerticalAlignment::Center);
  copyContent.Children().Append(copyLabel);
  copyButton.Content(copyContent);
  // the content is a glyph + text panel, not a string: name it for Narrator
  Automation::AutomationProperties::SetName(copyButton, Loc("copy_to_clipboard"));
  // Copy leaves the sheet OPEN — copying is not confirming.
  copyButton.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->CopyToClipboard();
  });
  content.Children().Append(copyButton);

  dialog_.Content(content);
  dialog_.PrimaryButtonClick([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      self->confirmed_ = true;  // lets Closing through; see the handler below
      auto confirmed = self->onConfirmed_;
      // Drop the app's copy of the credential the moment the sheet is done
      // with it; the dialog is about to close and nothing else may read it.
      self->seedphrase_.assign(self->seedphrase_.size(), '\0');
      self->seedphrase_.clear();
      if (confirmed) confirmed();
    }
  });

  // THE dismissal guard. Esc (and any other route that closes without a
  // button) raises Closing with Result::None; cancelling it is the only thing
  // that actually keeps this sheet on screen. Without it the account behind
  // the sheet was already created server-side and the phrase that was its one
  // credential went with the dialog.
  //
  // Gated on confirmed_ rather than on the Result alone so that a
  // PrimaryButtonClick handler which cancelled its own args (none does today)
  // could not deadlock the sheet shut.
  dialog_.Closing([weak = weak_from_this()](
                      auto const&, ContentDialogClosingEventArgs const& args) {
    auto self = weak.lock();
    if (!self) return;
    if (args.Result() == ContentDialogResult::None && !self->confirmed_) {
      args.Cancel(true);
    }
  });
}

void SeedphraseDisplaySheet::CopyToClipboard() {
  namespace dt = winrt::Windows::ApplicationModel::DataTransfer;
  dt::DataPackage package;
  package.SetText(H(seedphrase_));
  // A seedphrase must not enter Clipboard History (Win+V, readable by anything
  // that asks) and must not be uploaded to the cloud clipboard, which syncs it
  // to the signed-in Microsoft account and off this machine entirely. Both
  // flags are what password managers set, and both are OFF by default — plain
  // Clipboard::SetContent opts into both.
  dt::ClipboardContentOptions options;
  options.IsAllowedInHistory(false);
  options.IsRoamable(false);
  if (!dt::Clipboard::SetContentWithOptions(package, options)) {
    // Do NOT fall back to SetContent: that is the leak this exists to close.
    // No acknowledgement either — a snackbar over an empty clipboard is worse
    // than silence.
    urnw::LogError("seedphrase sheet: the clipboard refused a history-excluded copy");
    return;
  }
  if (onCopied_) onCopied_();
}

// ---- network server normalization (iOS NetworkServerUtils.swift) -----------

namespace netserver {
namespace {

std::string Trim(std::string const& raw) {
  const auto first = raw.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  const auto last = raw.find_last_not_of(" \t\r\n");
  return raw.substr(first, last - first + 1);
}

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::optional<std::string> ExplicitScheme(std::string const& raw) {
  const std::string value = Trim(raw);
  const auto at = value.find("://");
  if (at == std::string::npos) return std::nullopt;
  const std::string scheme = Lower(Trim(value.substr(0, at)));
  if (scheme.empty()) return std::nullopt;
  return scheme;
}

}  // namespace

std::string NormalizeHost(std::string const& raw) {
  std::string value = Lower(Trim(raw));
  if (const auto at = value.find("://"); at != std::string::npos) value = value.substr(at + 3);
  for (const char* sep : {"/", "?", "#"}) {
    if (const auto at = value.find(sep); at != std::string::npos) value = value.substr(0, at);
  }
  if (const auto at = value.find('@'); at != std::string::npos) value = value.substr(at + 1);
  // Strip a trailing :port for an IPv4 host:port or a BRACKETED IPv6 literal.
  // A bare IPv6 address is left alone on purpose: its colons are
  // indistinguishable from a port separator, and mangling one silently points
  // the client at the wrong server.
  //
  // This field is a HOST NAME — the SDK derives "api.<host>" and
  // "connect.<host>" from it, and a port has no meaning in that derivation, so
  // one typed here is dropped rather than carried into a name it cannot be
  // part of. A deployment that does not listen on 443 is reached through the
  // explicit API URL / Connect URL fields instead: NormalizeApiUrl and
  // NormalizeConnectUrl preserve a port ("https://api.example.com:8443" stays
  // exactly that), and an explicit url bypasses the derivation entirely.
  if (const auto at = value.rfind("]:"); at != std::string::npos) {
    value = value.substr(0, at) + "]";
  } else if (value.find('[') == std::string::npos) {
    if (const auto at = value.rfind(':'); at != std::string::npos) value = value.substr(0, at);
  }
  value = Trim(value);
  while (!value.empty() && value.front() == '.') value.erase(value.begin());
  while (!value.empty() && value.back() == '.') value.pop_back();
  return value;
}

namespace {
std::string NormalizeUrl(std::string const& raw, const char* defaultScheme) {
  std::string value = Trim(raw);
  while (!value.empty() && value.back() == '/') value.pop_back();
  if (value.empty()) return "";
  if (value.find("://") != std::string::npos) return value;
  return std::string(defaultScheme) + "://" + value;
}
}  // namespace

std::string NormalizeApiUrl(std::string const& raw) { return NormalizeUrl(raw, "https"); }
std::string NormalizeConnectUrl(std::string const& raw) { return NormalizeUrl(raw, "wss"); }

bool HasInsecureScheme(std::string const& raw, std::string const& secureScheme) {
  const auto scheme = ExplicitScheme(raw);
  if (!scheme) return false;  // no scheme = we will add the secure one
  return *scheme != secureScheme;
}

std::string DerivedServiceUrl(std::string const& hostName, std::string const& migrationHostName,
                              std::string const& envName, std::string const& scheme,
                              std::string const& service) {
  const std::string serviceHost = migrationHostName.empty() ? hostName : migrationHostName;
  const std::string serviceHostName = (envName == "main" || envName.empty())
                                          ? service + "." + serviceHost
                                          : envName + "-" + service + "." + serviceHost;
  return scheme + "://" + serviceHostName;
}

}  // namespace netserver

// ---- NetworkServerSheet -----------------------------------------------------

std::shared_ptr<NetworkServerSheet> NetworkServerSheet::Create(XamlRoot const& root,
                                                               SdkHost& sdk) {
  auto sheet = std::shared_ptr<NetworkServerSheet>(new NetworkServerSheet(sdk));
  sheet->Build(root);
  return sheet;
}

void NetworkServerSheet::Build(XamlRoot const& root) {
  current_ = sdk_.CurrentNetworkServer();

  dialog_ = ContentDialog();
  dialog_.XamlRoot(root);
  dialog_.Title(winrt::box_value(Loc("change_network_api_title")));
  dialog_.Background(colors::SheetBrush());
  // Close is the ONLY command-bar button. ContentDialog splits that bar evenly
  // between however many buttons it has, and with three the middle one rendered
  // as "Use default networl" - clipped mid-word. Both actions therefore live in
  // the body, which is also where iOS's NetworkServerSheet puts them.
  dialog_.CloseButtonText(Loc("close"));

  StackPanel content;
  content.MinWidth(sheetfit::Width(root, 420));
  content.Spacing(12);

  TextBlock description;
  description.Text(Loc("network_api_description"));
  description.FontSize(12);
  description.TextWrapping(TextWrapping::Wrap);
  description.Foreground(colors::MutedBrush());
  content.Children().Append(description);

  // What is in force RIGHT NOW, before anything is typed. Without these two
  // lines the sheet cannot tell you which server you are already on, which is
  // the first question anyone opening it has.
  auto currentLine = [&content](std::wstring const& value) {
    TextBlock line;
    line.Text(winrt::hstring{value});
    line.FontSize(11);
    line.TextWrapping(TextWrapping::Wrap);
    line.Foreground(colors::FaintBrush());
    content.Children().Append(line);
  };
  currentLine(urnw::Format("network_api_current_api", urnw::Widen(current_.apiUrl)));
  currentLine(urnw::Format("network_api_current_connect", urnw::Widen(current_.connectUrl)));

  auto field = [&content, this](TextBox& box, std::string_view labelKey,
                                std::string_view helpKey, std::string const& value) {
    StackPanel group;
    group.Spacing(2);
    box = TextBox();
    box.Header(winrt::box_value(Loc(labelKey)));
    box.Text(H(value));
    box.IsEnabled(current_.managerAvailable);
    if (auto style = Application::Current()
                         .Resources()
                         .TryLookup(winrt::box_value(L"UrTextInputStyle"))
                         .try_as<Style>()) {
      box.Style(style);
    }
    group.Children().Append(box);
    TextBlock help;
    help.Text(Loc(helpKey));
    help.FontSize(11);
    help.TextWrapping(TextWrapping::Wrap);
    help.Foreground(colors::MutedBrush());
    group.Children().Append(help);
    content.Children().Append(group);
  };

  const std::string initialHost = netserver::NormalizeHost(current_.hostName);
  field(hostBox_, "network_api_domain_label", "network_api_domain_help",
        initialHost.empty() ? DefaultHost() : initialHost);
  field(apiBox_, "network_api_api_url_label", "network_api_api_url_help",
        current_.configuredApiUrl);
  field(connectBox_, "network_api_connect_url_label", "network_api_connect_url_help",
        current_.configuredConnectUrl);

  insecureText_ = TextBlock();
  insecureText_.Text(Loc("network_api_insecure_warning"));
  insecureText_.FontSize(11);
  insecureText_.TextWrapping(TextWrapping::Wrap);
  // AMBER, not danger red. This line is ADVISORY and always was: Apply goes
  // ahead with an http:// or ws:// endpoint, because a self-hosted deployment
  // behind a local reverse proxy is a real thing people do. Painted in the
  // colour this app uses for refusals it read as a block, so the one case it
  // is meant to catch — a typo'd scheme against a public host — looked
  // indistinguishable from one the sheet had already stopped. Same amber the
  // seedphrase sheet's "this is the ONLY time" warning uses.
  insecureText_.Foreground(colors::MakeBrush(colors::kUrAmber));
  insecureText_.Visibility(Visibility::Collapsed);
  content.Children().Append(insecureText_);

  statusText_ = TextBlock();
  statusText_.FontSize(11);
  statusText_.TextWrapping(TextWrapping::Wrap);
  statusText_.Foreground(colors::MutedBrush());
  content.Children().Append(statusText_);

  if (!current_.managerAvailable) {
    // The SDK space manager never came up: say so instead of leaving three
    // dead fields and a button that does nothing.
    kit::ApplySupportingText(statusText_, Loc("network_api_manager_unavailable"),
                             kit::ValidationState::Invalid);
  }

  auto onEdit = [weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      self->ApplyDerivedPlaceholders();
      self->UpdateInsecureWarning();
    }
  };
  hostBox_.TextChanged(onEdit);
  apiBox_.TextChanged(onEdit);
  connectBox_.TextChanged(onEdit);

  Button useDefault;
  useDefault.Content(LocBox("network_api_use_default"));
  useDefault.HorizontalAlignment(HorizontalAlignment::Stretch);
  useDefault.IsEnabled(current_.managerAvailable);
  useDefault.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->UseDefault();
  });
  content.Children().Append(useDefault);

  Button apply;
  apply.Content(LocBox("network_api_apply"));
  apply.HorizontalAlignment(HorizontalAlignment::Stretch);
  apply.IsEnabled(current_.managerAvailable);
  if (auto style = Application::Current()
                       .Resources()
                       .TryLookup(winrt::box_value(L"AccentButtonStyle"))
                       .try_as<Style>()) {
    apply.Style(style);
  }
  apply.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      self->Apply(urnw::Narrow(self->hostBox_.Text().c_str()),
                  urnw::Narrow(self->apiBox_.Text().c_str()),
                  urnw::Narrow(self->connectBox_.Text().c_str()));
    }
  });
  content.Children().Append(apply);

  dialog_.Content(content);
  ApplyDerivedPlaceholders();
  UpdateInsecureWarning();
}

void NetworkServerSheet::ApplyDerivedPlaceholders() {
  const std::string typed = netserver::NormalizeHost(urnw::Narrow(hostBox_.Text().c_str()));
  const std::string host = typed.empty() ? DefaultHost() : typed;
  // No space carries a migration domain: the official key IS the operator
  // host (ids::kNetworkSpaceHostName, see SdkHost::BuildNetworkSpace), so
  // production and a custom or test deployment alike derive straight off the
  // host name.
  const std::string migration;
  const std::string env(ids::kNetworkSpaceEnvName);

  hostBox_.PlaceholderText(H(DefaultHost()));
  apiBox_.PlaceholderText(
      H(netserver::DerivedServiceUrl(host, migration, env, "https", "api")));
  connectBox_.PlaceholderText(
      H(netserver::DerivedServiceUrl(host, migration, env, "wss", "connect")));
}

void NetworkServerSheet::UpdateInsecureWarning() {
  const std::string api = urnw::Narrow(apiBox_.Text().c_str());
  const std::string connect = urnw::Narrow(connectBox_.Text().c_str());
  const bool insecure = (!api.empty() && netserver::HasInsecureScheme(api, "https")) ||
                        (!connect.empty() && netserver::HasInsecureScheme(connect, "wss"));
  insecureText_.Visibility(insecure ? Visibility::Visible : Visibility::Collapsed);
}

void NetworkServerSheet::Apply(std::string const& host, std::string const& apiUrl,
                               std::string const& connectUrl) {
  const std::string normalizedHost = netserver::NormalizeHost(host);
  if (normalizedHost.empty()) {
    kit::ApplySupportingText(statusText_, Loc("network_api_enter_domain"),
                             kit::ValidationState::Invalid);
    return;
  }
  const std::string normalizedApi = netserver::NormalizeApiUrl(apiUrl);
  const std::string normalizedConnect = netserver::NormalizeConnectUrl(connectUrl);

  if (sdk_.ApplyNetworkServer(normalizedHost, normalizedApi, normalizedConnect)) {
    kit::ApplySupportingText(
        statusText_,
        hstring{urnw::Format("network_api_switched_to", urnw::Widen(normalizedHost))},
        kit::ValidationState::Valid);
    dialog_.Hide();
    return;
  }
  kit::ApplySupportingText(statusText_, Loc("network_api_manager_unavailable"),
                           kit::ValidationState::Invalid);
}

void NetworkServerSheet::UseDefault() {
  // current_.defaultHostName, NOT ids::kNetworkSpaceHostName. This used to be
  // the compiled-in host both times, so on a session started against a
  // test network (URNETWORK_NETWORK_HOST) the button labelled "Use default
  // network" moved the client to PRODUCTION without saying so.
  const std::string host = DefaultHost();
  hostBox_.Text(H(host));
  apiBox_.Text(L"");
  connectBox_.Text(L"");
  Apply(host, "", "");
}

// What this process considers the default network. SdkHost resolves it; the
// fallback is only for a sheet built before the space manager came up.
std::string NetworkServerSheet::DefaultHost() const {
  return current_.defaultHostName.empty() ? std::string(ids::kNetworkSpaceHostName)
                                          : current_.defaultHostName;
}

// ---- SsoSheet (embedded Google / Apple sign-in) ----------------------------

std::shared_ptr<SsoSheet> SsoSheet::Create(XamlRoot const& root, SdkHost& sdk,
                                           std::string provider, std::string authorizeUrl,
                                           std::function<void()> onCancel,
                                           std::function<void()> onFailed) {
  auto sheet = std::shared_ptr<SsoSheet>(new SsoSheet(sdk, std::move(provider),
                                                      std::move(authorizeUrl),
                                                      std::move(onCancel), std::move(onFailed)));
  sheet->Build(root);
  return sheet;
}

void SsoSheet::Build(XamlRoot const& root) {
  dialog_ = ContentDialog();
  dialog_.XamlRoot(root);
  dialog_.Title(winrt::box_value(
      Loc(provider_ == "apple" ? "sign_in_with_apple" : "sign_in_with_google")));
  dialog_.Background(colors::SheetBrush());
  // Close IS the cancel: the sign-in answers only through the sheet's return
  // paths (the fragment interception, Apple's web message)
  dialog_.CloseButtonText(Loc("cancel"));

  StackPanel content;
  content.MinWidth(sheetfit::Width(root, 440));
  content.Spacing(8);

  // the webview (inserted once the slot loads) sits under a loading ring
  webviewSlot_ = Grid();
  webviewSlot_.MinWidth(sheetfit::Width(root, 440));
  // The provider's form wants room; the reserve covers the dialog's title and
  // command bar (sheetfit).
  webviewSlot_.Height(sheetfit::Height(root, 600, 120));
  webviewSlot_.Background(colors::BackgroundBrush());
  ring_ = ProgressRing();
  ring_.Width(36);
  ring_.Height(36);
  ring_.IsActive(true);
  ring_.HorizontalAlignment(HorizontalAlignment::Center);
  ring_.VerticalAlignment(VerticalAlignment::Center);
  webviewSlot_.Children().Append(ring_);
  content.Children().Append(webviewSlot_);
  dialog_.Content(content);

  // WebView2 init needs the control bound to a shown window, so it starts when
  // the slot enters the live tree, not from the ctor.
  webviewSlot_.Loaded([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->OpenProviderPage();
  });

  dialog_.Closed([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      self->closed_ = true;
      self->TeardownWebView();
      // Closed without an answer and not already routed to the browser-flow
      // fallback: a certain cancel — the login page re-enables its buttons.
      if (!self->completed_ && !self->failed_ && self->onCancel_) self->onCancel_();
    }
  });
}

winrt::fire_and_forget SsoSheet::OpenProviderPage() {
  // Google answers through the redirect's fragment; Apple through the web
  // message popup (the class comment says why the transports differ)
  if (provider_ == "apple") {
    OpenApplePopup();
    co_return;
  }
  namespace wv2 = winrt::Microsoft::Web::WebView2::Core;
  auto weak = weak_from_this();
  auto queue = dialog_.DispatcherQueue();

  // one webview per showing (a closed WebView2 cannot be revived)
  TeardownWebView();
  const uint32_t generation = ++webviewGeneration_;
  pageLoaded_ = false;
  webview_ = WebView2();
  // brand surface while the provider's page loads — never a white flash
  webview_.DefaultBackgroundColor(colors::kBackground);

  webview_.NavigationStarting([weak, queue](auto const&, auto const& args) {
    const std::string uri = urnw::Narrow(std::wstring_view{args.Uri()});
    auto self = weak.lock();
    if (!self) return;
    auto link = WalletConnect::OAuthFragmentReturn(self->provider_, uri);
    if (!link) return;
    // the provider handing the sign-in's result back in the redirect's
    // fragment — never a real navigation: the ur.io page never loads, and the
    // token never leaves the machine except to the provider
    args.Cancel(true);
    // deferred: handling closes the sheet and tears this webview down, which
    // must not happen from inside its own event
    queue.TryEnqueue([weak, link = std::move(*link)] {
      if (auto self = weak.lock()) self->HandleReturn(link);
    });
  });
  webview_.NavigationCompleted([weak, queue, generation](auto const&, auto const& args) {
    const bool ok = args.IsSuccess();
    queue.TryEnqueue([weak, generation, ok] {
      auto self = weak.lock();
      if (!self || generation != self->webviewGeneration_) return;
      if (ok) {
        self->pageLoaded_ = true;
        self->ring_.IsActive(false);
      } else if (!self->pageLoaded_) {
        // the authorize page never rendered (offline, dns, tls): nothing was
        // shown, so no sign-in can be lost — the browser flow can still run
        self->Fail();
      }
    });
  });
  webview_.CoreProcessFailed([weak, queue, generation](auto const&, auto const&) {
    queue.TryEnqueue([weak, generation] {
      auto self = weak.lock();
      if (!self || generation != self->webviewGeneration_) return;
      if (!self->pageLoaded_) {
        // died before anything rendered: the browser flow can still run
        self->Fail();
        return;
      }
      // Died mid-sign-in: no token ever existed here, so there is nothing to
      // lose — closing is the honest cancel, not a browser retry over a
      // half-completed provider attempt.
      self->dialog_.Hide();
    });
  });

  // under the loading ring (appended in Build, so it stays on top)
  webviewSlot_.Children().InsertAt(0, webview_);
  ring_.IsActive(true);

  try {
    // Explicit user data folder: WebView2's default is next to the exe, which
    // an install under Program Files cannot write. StorageRoot is the app's
    // own per-user dir (%LOCALAPPDATA%\URnetwork\app).
    const std::wstring dataDir = (StorageRoot(/*isService=*/false) / "webview2").wstring();
    auto environment = co_await wv2::CoreWebView2Environment::CreateWithOptionsAsync(
        hstring{}, hstring{dataDir}, wv2::CoreWebView2EnvironmentOptions());
    auto self = weak.lock();
    if (!self || generation != self->webviewGeneration_ || !self->webview_) co_return;
    co_await self->webview_.EnsureCoreWebView2Async(environment);
    // C++/WinRT resumes on the awaiting (UI) apartment; `self` kept the sheet
    // alive across the await, the generation says whether it still wants us
    if (generation != self->webviewGeneration_ || !self->webview_) co_return;
    auto core = self->webview_.CoreWebView2();
    core.NewWindowRequested([](auto const&, auto const& args) {
      // target=_blank links (the provider's terms/privacy): the system browser
      args.Handled(true);
      try {
        winrt::Windows::System::Launcher::LaunchUriAsync(
            winrt::Windows::Foundation::Uri(args.Uri()));
      } catch (...) {
      }
    });
    core.Navigate(H(self->authorizeUrl_));
  } catch (...) {
    // WebView2 init failed (no Evergreen runtime, disk, group policy): the
    // browser flow saves the sign-in
    auto self = weak.lock();
    if (!self || generation != self->webviewGeneration_) co_return;
    self->Fail();
  }
}

void SsoSheet::HandleReturn(std::string const& deepLink) {
  if (completed_ || closed_) return;
  completed_ = true;
  // Exactly the deep link the api's callback would have delivered for this
  // attempt in the browser flow: the armed attempt's state check, nonce check
  // and authLogin all run off it unchanged (SdkHost's on_sso).
  sdk_.HandleDeepLink(deepLink);
  dialog_.Hide();
}

winrt::fire_and_forget SsoSheet::OpenApplePopup() {
  namespace wv2 = winrt::Microsoft::Web::WebView2::Core;
  auto weak = weak_from_this();
  auto queue = dialog_.DispatcherQueue();

  TeardownWebView();
  const uint32_t generation = ++webviewGeneration_;
  pageLoaded_ = false;

  // The visible webview hosts Apple's page as the driver page's POPUP; the
  // hidden driver is the opener the result's web message returns to.
  webview_ = WebView2();
  webview_.DefaultBackgroundColor(colors::kBackground);

  webview_.NavigationStarting([weak, queue](auto const&, auto const& args) {
    const std::string uri = urnw::Narrow(std::wstring_view{args.Uri()});
    auto self = weak.lock();
    if (!self) return;
    auto link = WalletConnect::OAuthFragmentReturn(self->provider_, uri);
    if (!link) return;
    // belt and braces: Apple's answer normally arrives by web message, but if
    // the popup ever does navigate to the registered redirect, the fragment
    // return is still the right read — and the ur.io page never loads
    args.Cancel(true);
    queue.TryEnqueue([weak, link = std::move(*link)] {
      if (auto self = weak.lock()) self->HandleReturn(link);
    });
  });
  webview_.NavigationCompleted([weak, queue, generation](auto const&, auto const& args) {
    const bool ok = args.IsSuccess();
    queue.TryEnqueue([weak, generation, ok] {
      auto self = weak.lock();
      if (!self || generation != self->webviewGeneration_) return;
      if (ok) {
        self->pageLoaded_ = true;
        self->ring_.IsActive(false);
      } else if (!self->pageLoaded_) {
        // Apple's page never rendered (offline, dns, tls): nothing was shown,
        // so no sign-in can be lost — the browser flow can still run
        self->Fail();
      }
    });
  });
  webview_.CoreProcessFailed([weak, queue, generation](auto const&, auto const&) {
    queue.TryEnqueue([weak, generation] {
      auto self = weak.lock();
      if (!self || generation != self->webviewGeneration_) return;
      if (!self->pageLoaded_) {
        self->Fail();
        return;
      }
      // Died mid-sign-in: no token ever existed here, so there is nothing to
      // lose — closing is the honest cancel, not a browser retry over a
      // half-completed provider attempt.
      self->dialog_.Hide();
    });
  });

  driverWebview_ = WebView2();
  // a working opener, not a surface: the driver never shows
  driverWebview_.Width(1);
  driverWebview_.Height(1);
  driverWebview_.Opacity(0);
  driverWebview_.HorizontalAlignment(HorizontalAlignment::Left);
  driverWebview_.VerticalAlignment(VerticalAlignment::Top);
  driverWebview_.WebMessageReceived([weak, queue](auto const&, auto const& args) {
    // the driver page forwarding Apple's oauthDone payload (a json string)
    std::string json;
    try {
      json = urnw::Narrow(std::wstring{args.TryGetWebMessageAsString()});
    } catch (...) {
      try {
        json = urnw::Narrow(std::wstring{args.WebMessageAsJson()});
      } catch (...) {
        return;
      }
    }
    queue.TryEnqueue([weak, json] {
      if (auto self = weak.lock()) self->HandleAppleMessage(json);
    });
  });
  driverWebview_.NavigationCompleted([weak, queue, generation](auto const&, auto const& args) {
    const bool ok = args.IsSuccess();
    queue.TryEnqueue([weak, generation, ok] {
      auto self = weak.lock();
      if (!self || generation != self->webviewGeneration_) return;
      // the driver is local (a virtual host mapping to a file we just wrote):
      // it failing means no popup will ever open, and nothing was shown
      if (!ok && !self->pageLoaded_) self->Fail();
    });
  });
  driverWebview_.CoreProcessFailed([weak, queue, generation](auto const&, auto const&) {
    queue.TryEnqueue([weak, generation] {
      auto self = weak.lock();
      if (!self || generation != self->webviewGeneration_) return;
      // the driver dying kills the result's return path
      if (!self->pageLoaded_) {
        self->Fail();
        return;
      }
      self->dialog_.Hide();
    });
  });

  // popup over driver, both under the loading ring (appended in Build)
  webviewSlot_.Children().InsertAt(0, driverWebview_);
  webviewSlot_.Children().InsertAt(0, webview_);
  ring_.IsActive(true);

  try {
    const std::wstring dataDir = (StorageRoot(/*isService=*/false) / "webview2").wstring();
    auto environment = co_await wv2::CoreWebView2Environment::CreateWithOptionsAsync(
        hstring{}, hstring{dataDir}, wv2::CoreWebView2EnvironmentOptions());
    auto self = weak.lock();
    if (!self || generation != self->webviewGeneration_ || !self->webview_ ||
        !self->driverWebview_)
      co_return;
    // the popup first: the driver's window.open assigns it as the NewWindow,
    // which wants an ensured core
    co_await self->webview_.EnsureCoreWebView2Async(environment);
    co_await self->driverWebview_.EnsureCoreWebView2Async(environment);
    if (generation != self->webviewGeneration_ || !self->webview_ || !self->driverWebview_)
      co_return;
    auto popupCore = self->webview_.CoreWebView2();
    auto driverCore = self->driverWebview_.CoreWebView2();
    // window.open from the driver hosts Apple's page in the sheet's visible
    // webview. NewWindow (not Handled + the system browser) is what keeps
    // window.opener — and with it the web_message return path — intact.
    driverCore.NewWindowRequested([popupCore](auto const&, auto const& args) {
      args.NewWindow(popupCore);
    });
    popupCore.NewWindowRequested([](auto const&, auto const& args) {
      // target=_blank links inside Apple's page (terms, privacy, help): the
      // system browser
      args.Handled(true);
      try {
        winrt::Windows::System::Launcher::LaunchUriAsync(
            winrt::Windows::Foundation::Uri(args.Uri()));
      } catch (...) {
      }
    });
    // A page at the ur.io origin, served from disk through a virtual host
    // mapping: Apple's completion page postMessages the result to the opener
    // only when the opener's origin matches the registered redirect's
    // (https://ur.io), so the driver page must BE https://ur.io — without
    // depending on the real site being reachable.
    const auto driverDir = StorageRoot(/*isService=*/false) / "sso-driver";
    std::error_code ec;
    std::filesystem::create_directories(driverDir, ec);
    const auto driverFile = driverDir / "apple-popup-driver.html";
    {
      std::ofstream out(driverFile, std::ios::binary | std::ios::trunc);
      out << kAppleDriverPageHead << authorizeUrl_ << kAppleDriverPageTail;
    }
    driverCore.SetVirtualHostNameToFolderMapping(
        hstring{L"ur.io"}, hstring{driverDir.wstring()},
        wv2::CoreWebView2HostResourceAccessKind::Allow);
    driverCore.Navigate(hstring{L"https://ur.io/apple-popup-driver.html"});
  } catch (...) {
    // WebView2 init failed (no Evergreen runtime, disk, group policy): the
    // browser flow saves the sign-in
    auto self = weak.lock();
    if (!self || generation != self->webviewGeneration_) co_return;
    self->Fail();
  }
}

void SsoSheet::HandleAppleMessage(std::string const& json) {
  // Apple's oauthDone payload, forwarded by the driver page: {authorization:
  // {id_token, code, state}} on success — the code is the hybrid flow's other
  // half and is ignored, as on the website — or {error: "..."} on failure.
  auto data = nlohmann::json::parse(json, nullptr, false);
  if (!data.is_object()) return;
  const auto getStr = [](nlohmann::json const& obj, const char* key) {
    auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string();
  };
  std::string idToken, state;
  if (auto it = data.find("authorization"); it != data.end() && it->is_object()) {
    idToken = getStr(*it, "id_token");
    state = getStr(*it, "state");
  }
  const std::string error = getStr(data, "error");
  // not the sign-in's answer (a stray message, a malformed payload): keep
  // waiting — the sheet's close is the cancel
  if (idToken.empty() && error.empty()) return;
  HandleReturn(WalletConnect::OAuthDeepLink(provider_, state, idToken, error));
}

void SsoSheet::Fail() {
  // failed_ first: the Closed handler must not report a cancel on top of the
  // fallback, and onFailed_ re-arms (superseding this attempt) in the caller.
  failed_ = true;
  dialog_.Hide();
  if (onFailed_) onFailed_();
}

void SsoSheet::TeardownWebView() {
  // invalidate every async continuation of the current attempt first: init,
  // navigation results and the failure fallback all check the generation
  ++webviewGeneration_;
  pageLoaded_ = false;
  ring_.IsActive(false);
  if (!webview_ && !driverWebview_) return;
  auto view = webview_;
  auto driver = driverWebview_;
  webview_ = nullptr;
  driverWebview_ = nullptr;
  // Deferred: teardown is reachable from inside a webview's own event handlers
  // (a canceled fragment navigation or a web message closes the sheet), and
  // closing the control during its own event emission is the same reentrancy
  // the upgrade sheet defers around.
  auto slot = webviewSlot_;
  dialog_.DispatcherQueue().TryEnqueue([slot, view, driver] {
    for (auto const& v : {view, driver}) {
      if (!v) continue;
      uint32_t index = 0;
      if (slot.Children().IndexOf(v, index)) slot.Children().RemoveAt(index);
      try {
        v.Close();
      } catch (...) {
      }
    }
  });
}

// ---- Account menu (iOS Shared/Views/AccountMenu.swift) ----------------------

void ShowAccountMenu(FrameworkElement const& anchor, SdkHost& sdk,
                     std::string const& networkName, bool guest,
                     AccountMenuActions actions) {
  MenuFlyout flyout;

  // The identity row. Not actionable — it says which network these actions
  // apply to, which matters the moment anyone has signed in twice.
  MenuFlyoutItem identity;
  identity.Text(networkName.empty() ? Loc("guest") : H(networkName));
  identity.IsEnabled(false);
  flyout.Items().Append(identity);
  flyout.Items().Append(MenuFlyoutSeparator());

  if (guest && actions.onCreateAccount) {
    MenuFlyoutItem create;
    create.Text(Loc("create_account"));
    auto onCreate = actions.onCreateAccount;
    create.Click([onCreate](auto const&, auto const&) { onCreate(); });
    flyout.Items().Append(create);
  }

  // Share: the referral message with the code substituted, onto the clipboard.
  // iOS raises the system ShareLink; Windows' nearest honest equivalent for
  // "share this text" without an HWND interop dance is the clipboard plus an
  // acknowledgement the user can see.
  MenuFlyoutItem share;
  share.Text(Loc("share_urnetwork"));
  auto onShared = actions.onShared;
  auto queue = anchor.DispatcherQueue();
  share.Click([&sdk, onShared, queue](auto const&, auto const&) {
    // IsLoggedIn(), NOT apiReady(). apiReady() is api_.has_value(), set at SDK
    // INIT and true with no session at all, so guarding on it here would fire
    // an unauthenticated getNetworkReferralCode at the production API from
    // whatever machine this is — which is exactly what --preview-ui, now that
    // it can reach this menu, would have done on every open.
    if (!sdk.IsLoggedIn()) return;
    sdk.api().getNetworkReferralCode(
        [onShared, queue](std::optional<urnet::GetNetworkReferralCodeResult> result,
                          std::optional<std::string>) {
          // SDK callback thread: build the message here, but touch the
          // clipboard and the snackbar only on the UI thread.
          std::string code;
          if (result && result->referral_code) code = *result->referral_code;
          queue.TryEnqueue([onShared, code] {
            // The store's message takes the code; with no code yet there is
            // nothing useful to share, so say nothing rather than share a
            // sentence with a hole in it.
            if (code.empty()) return;
            winrt::Windows::ApplicationModel::DataTransfer::DataPackage package;
            package.SetText(hstring{urnw::Format("referral_share_message", urnw::Widen(code))});
            winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
            if (onShared) onShared();
          });
        });
  });
  flyout.Items().Append(share);

  flyout.Items().Append(MenuFlyoutSeparator());
  MenuFlyoutItem signOut;
  signOut.Text(Loc("sign_out"));
  auto onSignOut = actions.onSignOut;
  signOut.Click([onSignOut](auto const&, auto const&) {
    if (onSignOut) onSignOut();
  });
  flyout.Items().Append(signOut);

  flyout.ShowAt(anchor);
}

}  // namespace urnw
