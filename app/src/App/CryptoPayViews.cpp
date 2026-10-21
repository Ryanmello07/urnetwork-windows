// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "CryptoPayViews.h"

#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.Globalization.DateTimeFormatting.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <optional>

#include <qrcodegen.hpp>

#include "CryptoPaySdk.h"  // cryptopay::TermNoticeDue: the sdk's rule
#include "ExtenderPresentation.h"  // ExtenderQrRunsFor: one rectangle per run of dark modules
#include "Localization.h"
#include "Log.h"
#include "PageContext.h"
#include "PaymentRefusal.h"
#include "SettingsSheets.h"  // rows::CopyToClipboard
#include "Strings.h"
#include "SubscriptionBalance.h"
#include "UrColors.h"
#include "UrComponents.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using urnw::pages::H;
using urnw::pages::Loc;
namespace automation = winrt::Microsoft::UI::Xaml::Automation;
namespace peers = winrt::Microsoft::UI::Xaml::Automation::Peers;
// wingdi.h declares ::Ellipse and ::Rectangle; alias the XAML shapes
using ShapeEllipse = winrt::Microsoft::UI::Xaml::Shapes::Ellipse;
using ShapeRectangle = winrt::Microsoft::UI::Xaml::Shapes::Rectangle;

namespace urnw {
namespace {

// the QR's ink and its frame: near-black on white, the contrast a camera
// reads best (the frame is the quiet zone, so it has to be light)
constexpr winrt::Windows::UI::Color kQrDark{255, 0x10, 0x10, 0x10};
constexpr winrt::Windows::UI::Color kQrLight{255, 0xFF, 0xFF, 0xFF};
constexpr winrt::Windows::UI::Color kTransparent{0, 0, 0, 0};
// how long a Copy button says Copied
constexpr auto kCopiedFor = std::chrono::seconds(2);

TextBlock MakeText(hstring const& text, double size, Brush const& brush = nullptr,
                   bool wrap = false) {
  TextBlock block;
  block.Text(text);
  block.FontSize(size);
  if (brush) block.Foreground(brush);
  if (wrap) block.TextWrapping(TextWrapping::Wrap);
  return block;
}

// An address is read and compared character by character: a fixed-width face.
FontFamily MonoFont() { return FontFamily{L"Cascadia Mono, Consolas"}; }

void Show(UIElement const& element, bool visible) {
  if (element) element.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
}

std::optional<Style> AccentStyle() {
  auto resources = Application::Current().Resources();
  auto key = winrt::box_value(hstring(L"AccentButtonStyle"));
  if (resources.HasKey(key)) {
    if (auto style = resources.Lookup(key).try_as<Style>()) return style;
  }
  return std::nullopt;
}

// The words a method goes by on its segment and its button.
std::wstring MethodName(std::string const& method) {
  return Localized(cryptopay::MethodLabelKey(method));
}

// The status line is a polite live region, so a screen reader hears the
// payment move on (seen, confirming, paid) without going to it. WinUI
// announces a live region only when its peer raises LiveRegionChanged, and a
// peer exists only while assistive technology is asking (SolanaWalletSheets).
void Announce(FrameworkElement const& line) {
  if (!line) return;
  if (auto peer = peers::FrameworkElementAutomationPeer::FromElement(line)) {
    peer.RaiseAutomationEvent(peers::AutomationEvents::LiveRegionChanged);
  }
}

// The clipboard can be held by another program: a refused copy is logged and
// the invoice goes on.
void CopyText(std::string const& text) {
  if (text.empty()) return;
  try {
    rows::CopyToClipboard(text);
  } catch (...) {
    LogWarn("crypto invoice: the clipboard refused the copy");
  }
}

// A label over a value with its Copy button: the value in the fixed-width
// face, wrapping (an address has no break of its own) and selectable.
StackPanel MakeCopyBlock(hstring const& label, double valueSize, TextBlock& outValue,
                         Button& outCopy) {
  StackPanel block;
  block.Spacing(4);
  block.Children().Append(MakeText(label, 12, colors::MutedBrush(), true));
  Grid line;
  line.ColumnSpacing(8);
  ColumnDefinition valueColumn;
  valueColumn.Width(GridLength{1, GridUnitType::Star});
  ColumnDefinition copyColumn;
  copyColumn.Width(GridLength{0, GridUnitType::Auto});
  line.ColumnDefinitions().Append(valueColumn);
  line.ColumnDefinitions().Append(copyColumn);
  outValue = MakeText(hstring{}, valueSize, colors::TextBrush(), true);
  outValue.FontFamily(MonoFont());
  outValue.IsTextSelectionEnabled(true);
  outValue.VerticalAlignment(VerticalAlignment::Center);
  line.Children().Append(outValue);
  outCopy = Button();
  outCopy.Content(winrt::box_value(Loc("crypto_invoice_copy")));
  outCopy.VerticalAlignment(VerticalAlignment::Top);
  // three buttons say Copy: each says what it copies
  automation::AutomationProperties::SetHelpText(outCopy, label);
  Grid::SetColumn(outCopy, 1);
  line.Children().Append(outCopy);
  block.Children().Append(line);
  return block;
}

}  // namespace

// ---- PaymentMethodRow -----------------------------------------------------------

StackPanel PaymentMethodRow::Build() {
  State& state = *state_;
  state.onSelect = &onSelect;
  state.root = StackPanel();
  state.root.Spacing(6);
  state.root.Visibility(Visibility::Collapsed);

  // the segments; "Pay with" names the row for UIA (the row has no visible
  // label: it sits directly above the button it changes)
  state.bar = SelectorBar();
  state.bar.HorizontalAlignment(HorizontalAlignment::Left);
  automation::AutomationProperties::SetName(state.bar, Loc("pay_method_label"));
  automation::AutomationProperties::SetAutomationId(state.bar, L"acceptance.upgrade.payment-method");
  state.bar.SelectionChanged([weak = std::weak_ptr<State>(state_)](
                                 SelectorBar const& sender, SelectorBarSelectionChangedEventArgs const&) {
    auto self = weak.lock();
    if (!self || self->applying) return;
    const SelectorBarItem item = sender.SelectedItem();
    if (!item) return;
    for (size_t index = 0; index < self->items.size() && index < self->methods.size(); ++index) {
      if (self->items[index] != item) continue;
      if (self->methods[index] == self->selected) return;
      self->selected = self->methods[index];
      Apply(*self);
      if (self->onSelect && *self->onSelect) (*self->onSelect)(self->selected);
      return;
    }
  });
  state.root.Children().Append(state.bar);

  state.unavailable = MakeText(Loc("pay_method_unavailable"), 12, colors::MakeBrush(colors::kUrAmber), true);
  state.unavailable.Visibility(Visibility::Collapsed);
  state.root.Children().Append(state.unavailable);
  return state.root;
}

void PaymentMethodRow::SetMethods(std::vector<std::string> const& methods, std::string const& wanted) {
  State& state = *state_;
  if (!state.bar) return;
  // Card and the coins this app knows, in the sdk's order; nothing at all when
  // no coin is listed, which hides the row
  std::vector<std::string> listed;
  if (cryptopay::MethodRowShows(methods)) {
    for (std::string const& method : methods) {
      if (method == cryptopay::kMethodCard || cryptopay::IsCoin(method)) listed.push_back(method);
    }
  }
  if (listed != state.methods) {
    state.applying = true;
    state.bar.Items().Clear();
    state.items.clear();
    for (std::string const& method : listed) {
      SelectorBarItem item;
      item.Text(hstring{MethodName(method)});
      automation::AutomationProperties::SetAutomationId(
          item, hstring{L"acceptance.payment-method-" + Widen(method)});
      state.bar.Items().Append(item);
      state.items.push_back(item);
    }
    state.methods = std::move(listed);
    state.applying = false;
  }
  state.selected = cryptopay::MethodToSelect(state.methods, wanted);
  Apply(state);
}

void PaymentMethodRow::Select(std::string const& method) {
  state_->selected = cryptopay::MethodToSelect(state_->methods, method);
  if (state_->bar) Apply(*state_);
}

void PaymentMethodRow::SetEnabled(bool enabled) {
  if (state_->bar) state_->bar.IsEnabled(enabled);
}

void PaymentMethodRow::SetUnavailable(std::string const& method) {
  state_->unavailableMethod = method;
  if (state_->bar) Apply(*state_);
}

void PaymentMethodRow::Apply(State& state) {
  state.root.Visibility(state.methods.empty() ? Visibility::Collapsed : Visibility::Visible);
  state.applying = true;
  for (size_t index = 0; index < state.items.size() && index < state.methods.size(); ++index) {
    if (state.methods[index] == state.selected) state.bar.SelectedItem(state.items[index]);
  }
  state.applying = false;
  const bool unavailable = !state.unavailableMethod.empty() && state.unavailableMethod == state.selected;
  state.unavailable.Visibility(unavailable ? Visibility::Visible : Visibility::Collapsed);
}

// ---- CryptoInvoicePanel -----------------------------------------------------------

StackPanel CryptoInvoicePanel::Build(winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue,
                                     double qrMaxSide) {
  State& state = *state_;
  state.onOpenWallet = &onOpenWallet;
  state.onRefresh = &onRefresh;
  state.onPayAnotherWay = &onPayAnotherWay;
  state.qrMaxSide = qrMaxSide;
  auto weak = std::weak_ptr<State>(state_);

  StackPanel root;
  root.Spacing(14);

  // 1. the network the coin is sent on, first
  state.networkLine = MakeText(hstring{}, 22, colors::TextBrush(), true);
  state.networkLine.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
  automation::AutomationProperties::SetHeadingLevel(state.networkLine,
                                                    peers::AutomationHeadingLevel::Level2);
  root.Children().Append(state.networkLine);

  // a start in flight, or a resumed invoice before its first status: a brief
  // loading line in place of the payment details (nothing is payable yet)
  state.busyLine = StackPanel();
  state.busyLine.Orientation(Orientation::Horizontal);
  state.busyLine.Spacing(10);
  state.busyLine.Margin(Thickness{0, 8, 0, 8});
  state.busyRing = ProgressRing();
  state.busyRing.Width(16);
  state.busyRing.Height(16);
  state.busyRing.IsActive(false);
  state.busyRing.VerticalAlignment(VerticalAlignment::Center);
  state.busyLine.Children().Append(state.busyRing);
  auto loading = MakeText(Loc("loading"), 14, colors::MutedBrush());
  loading.VerticalAlignment(VerticalAlignment::Center);
  state.busyLine.Children().Append(loading);
  state.busyLine.Visibility(Visibility::Collapsed);
  root.Children().Append(state.busyLine);

  // 2-3. what to send, where
  state.instructions = StackPanel();
  state.instructions.Spacing(12);
  state.qrFrame = Border();
  state.qrFrame.Background(colors::MakeBrush(kQrLight));
  state.qrFrame.CornerRadius(CornerRadius{8, 8, 8, 8});
  state.qrFrame.HorizontalAlignment(HorizontalAlignment::Center);
  state.qrFrame.Visibility(Visibility::Collapsed);
  state.qrCanvas = Canvas();
  state.qrFrame.Child(state.qrCanvas);
  // a picture of the uri: the words below say the same for a screen reader
  automation::AutomationProperties::SetAccessibilityView(state.qrFrame,
                                                         automation::Peers::AccessibilityView::Raw);
  state.instructions.Children().Append(state.qrFrame);

  state.openWallet = Button();
  state.openWallet.Content(winrt::box_value(Loc("crypto_invoice_open_wallet")));
  state.openWallet.HorizontalAlignment(HorizontalAlignment::Stretch);
  if (auto style = AccentStyle()) state.openWallet.Style(*style);
  automation::AutomationProperties::SetAutomationId(state.openWallet, L"acceptance.crypto-panel.open-wallet");
  state.openWallet.Click([weak](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      if (self->onOpenWallet && *self->onOpenWallet) (*self->onOpenWallet)();
    }
  });
  state.instructions.Children().Append(state.openWallet);

  state.amountBlock = MakeCopyBlock(Loc("crypto_invoice_send_exactly"), 18, state.amountText,
                                    state.copyAmount);
  state.amountText.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  state.copyAmount.Click([weak](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      // the bare amount: a wallet's amount field takes no ticker
      CopyText(self->amount);
      ShowCopied(*self, self->copyAmount);
    }
  });
  state.instructions.Children().Append(state.amountBlock);

  state.addressBlock = MakeCopyBlock(Loc("crypto_invoice_to_address"), 13, state.addressText,
                                     state.copyAddress);
  state.copyAddress.Click([weak](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      CopyText(self->address);
      ShowCopied(*self, self->copyAddress);
    }
  });
  state.instructions.Children().Append(state.addressBlock);

  // USDC: the chain to send on, under the address (a transfer on another
  // chain is lost)
  state.usdcWarning = MakeText(Loc("usdc_solana_network_only"), 12, colors::MakeBrush(colors::kUrAmber), true);
  state.usdcWarning.Visibility(Visibility::Collapsed);
  state.instructions.Children().Append(state.usdcWarning);

  state.memoBlock = MakeCopyBlock(Loc("crypto_invoice_memo"), 13, state.memoText, state.copyMemo);
  state.copyMemo.Click([weak](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      CopyText(self->memo);
      ShowCopied(*self, self->copyMemo);
    }
  });
  state.memoBlock.Visibility(Visibility::Collapsed);
  state.instructions.Children().Append(state.memoBlock);
  state.instructions.Visibility(Visibility::Collapsed);
  root.Children().Append(state.instructions);

  // 4. the quote: its countdown, or that it expired, with Refresh quote
  state.quoteRow = Grid();
  state.quoteRow.ColumnSpacing(8);
  ColumnDefinition quoteColumn;
  quoteColumn.Width(GridLength{1, GridUnitType::Star});
  ColumnDefinition ringColumn;
  ringColumn.Width(GridLength{0, GridUnitType::Auto});
  ColumnDefinition refreshColumn;
  refreshColumn.Width(GridLength{0, GridUnitType::Auto});
  state.quoteRow.ColumnDefinitions().Append(quoteColumn);
  state.quoteRow.ColumnDefinitions().Append(ringColumn);
  state.quoteRow.ColumnDefinitions().Append(refreshColumn);
  state.quoteText = MakeText(hstring{}, 14, colors::MutedBrush(), true);
  state.quoteText.VerticalAlignment(VerticalAlignment::Center);
  // the countdown changes every second: read when asked, never announced
  automation::AutomationProperties::SetLiveSetting(state.quoteText, peers::AutomationLiveSetting::Off);
  state.quoteRow.Children().Append(state.quoteText);
  state.refreshRing = ProgressRing();
  state.refreshRing.Width(16);
  state.refreshRing.Height(16);
  state.refreshRing.IsActive(false);
  state.refreshRing.Visibility(Visibility::Collapsed);
  state.refreshRing.VerticalAlignment(VerticalAlignment::Center);
  Grid::SetColumn(state.refreshRing, 1);
  state.quoteRow.Children().Append(state.refreshRing);
  state.refreshButton = Button();
  state.refreshButton.Content(winrt::box_value(Loc("crypto_invoice_refresh_quote")));
  state.refreshButton.VerticalAlignment(VerticalAlignment::Center);
  automation::AutomationProperties::SetAutomationId(state.refreshButton, L"acceptance.crypto-panel.refresh");
  state.refreshButton.Click([weak](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      if (self->onRefresh && *self->onRefresh) (*self->onRefresh)();
    }
  });
  Grid::SetColumn(state.refreshButton, 2);
  state.quoteRow.Children().Append(state.refreshButton);
  state.quoteRow.Visibility(Visibility::Collapsed);
  root.Children().Append(state.quoteRow);

  // 5. the status: waiting -> payment seen -> confirming n of N -> paid
  state.steps = StackPanel();
  state.steps.Spacing(8);
  for (int index = 0; index < 4; ++index) {
    Step step;
    step.row = StackPanel();
    step.row.Orientation(Orientation::Horizontal);
    step.row.Spacing(10);
    step.dot = ShapeEllipse();
    step.dot.Width(10);
    step.dot.Height(10);
    step.dot.StrokeThickness(2);
    step.dot.VerticalAlignment(VerticalAlignment::Center);
    automation::AutomationProperties::SetAccessibilityView(step.dot,
                                                           automation::Peers::AccessibilityView::Raw);
    step.row.Children().Append(step.dot);
    step.label = MakeText(hstring{}, 14, colors::MutedBrush(), true);
    automation::AutomationProperties::SetLiveSetting(step.label, peers::AutomationLiveSetting::Polite);
    step.row.Children().Append(step.label);
    state.steps.Children().Append(step.row);
    state.stepRows.push_back(step);
  }
  state.steps.Visibility(Visibility::Collapsed);
  root.Children().Append(state.steps);

  state.remainingText = MakeText(hstring{}, 14, colors::MakeBrush(colors::kUrAmber), true);
  state.remainingText.Visibility(Visibility::Collapsed);
  root.Children().Append(state.remainingText);

  state.reviewText = MakeText(Loc("crypto_status_review"), 14, colors::TextBrush(), true);
  automation::AutomationProperties::SetLiveSetting(state.reviewText, peers::AutomationLiveSetting::Polite);
  state.reviewText.Visibility(Visibility::Collapsed);
  root.Children().Append(state.reviewText);

  // 6. help
  state.walletsText = MakeText(hstring{}, 12, colors::MutedBrush(), true);
  state.walletsText.Visibility(Visibility::Collapsed);
  root.Children().Append(state.walletsText);
  state.zcashNote = MakeText(Loc("crypto_invoice_zcash_exchange_note"), 12, colors::MutedBrush(), true);
  state.zcashNote.Visibility(Visibility::Collapsed);
  root.Children().Append(state.zcashNote);

  // the last requote's failure
  state.errorText = MakeText(hstring{}, 12, colors::DangerBrush(), true);
  state.errorText.IsTextSelectionEnabled(true);
  state.errorText.Visibility(Visibility::Collapsed);
  root.Children().Append(state.errorText);

  // 7. another way (Close is the dialog's own)
  state.payAnotherWay = Button();
  state.payAnotherWay.Content(winrt::box_value(Loc("crypto_invoice_pay_another_way")));
  state.payAnotherWay.HorizontalAlignment(HorizontalAlignment::Stretch);
  state.payAnotherWay.Background(colors::MakeBrush(kTransparent));
  automation::AutomationProperties::SetAutomationId(state.payAnotherWay,
                                                    L"acceptance.crypto-panel.pay-another-way");
  state.payAnotherWay.Click([weak](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      if (self->onPayAnotherWay && *self->onPayAnotherWay) (*self->onPayAnotherWay)();
    }
  });
  state.payAnotherWay.Visibility(Visibility::Collapsed);
  root.Children().Append(state.payAnotherWay);

  // "Copied" goes back to "Copy" after a moment
  state.copiedTimer = queue.CreateTimer();
  state.copiedTimer.Interval(std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(kCopiedFor));
  state.copiedTimer.IsRepeating(false);
  state.copiedTimer.Tick([weak](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      for (Button const& button : {self->copyAmount, self->copyAddress, self->copyMemo}) {
        button.Content(winrt::box_value(Loc("crypto_invoice_copy")));
      }
    }
  });

  automation::AutomationProperties::SetAutomationId(root, L"acceptance.crypto-panel");
  return root;
}

void CryptoInvoicePanel::Render(cryptopay::CryptoInvoiceView const& view, bool uriAllowed,
                                std::vector<std::string> const& wallets) {
  State& state = *state_;
  if (!state.networkLine) return;
  const cryptopay::CryptoInvoiceDisplay display = cryptopay::DisplayFor(view, uriAllowed);

  // 1. the network, first
  state.networkLine.Text(display.networkLineKey.empty() ? hstring{} : Loc(display.networkLineKey));
  state.busyRing.IsActive(display.busy);
  Show(state.busyLine, display.busy);

  // 2-3. what to send, where (CONTRACT §6.2.1): the amount, the QR and Open in
  // wallet only while payable, the QR and Open in wallet only for a uri the
  // sdk's check passed (cryptopay::UriAllowed), drawn again only for a new
  // uri; the address also for a top-up
  Show(state.instructions, display.showAmount || display.showQr || display.showAddress);
  if (display.showQr) {
    if (state.qrUri != view.uri) DrawQr(state, view.uri);
  } else if (!state.qrUri.empty()) {
    state.qrCanvas.Children().Clear();
    state.qrUri.clear();
  }
  Show(state.qrFrame, display.showQr && !state.qrUri.empty());
  Show(state.openWallet, display.showQr);
  state.amount = view.amount;
  state.amountText.Text(H(display.amountText));
  Show(state.amountBlock, display.showAmount);
  state.address = display.address;
  state.addressText.Text(H(display.address));
  Show(state.addressBlock, display.showAddress);
  Show(state.usdcWarning, display.showUsdcWarning);
  state.memo = display.memo;
  state.memoText.Text(H(display.memo));
  Show(state.memoBlock, display.showMemo);

  // 4. the quote
  if (display.quote == cryptopay::QuoteLine::Counting) {
    state.quoteText.Text(hstring{Format("crypto_invoice_expires_in", Widen(display.countdown))});
  } else if (display.quote == cryptopay::QuoteLine::Expired) {
    state.quoteText.Text(Loc("crypto_invoice_quote_expired"));
  }
  Show(state.quoteText, display.quote != cryptopay::QuoteLine::Hidden);
  Show(state.refreshButton, display.showRefresh);
  state.refreshButton.IsEnabled(display.refreshEnabled);
  const bool refreshing = display.showRefresh && view.refreshing;
  state.refreshRing.IsActive(refreshing);
  Show(state.refreshRing, refreshing);
  Show(state.quoteRow, display.quote != cryptopay::QuoteLine::Hidden || display.showRefresh);

  // 5. the status steps
  Show(state.steps, display.showSteps);
  const std::vector<int> steps = cryptopay::StepsFor(view.asset, view.confirmationsRequired);
  const auto pending = colors::MakeBrush(colors::kTextFaint);
  for (int index = 0; index < static_cast<int>(state.stepRows.size()); ++index) {
    Step& step = state.stepRows[static_cast<size_t>(index)];
    const bool listed = std::find(steps.begin(), steps.end(), index) != steps.end();
    Show(step.row, listed);
    if (!listed) continue;
    switch (index) {
      case 0: step.label.Text(Loc("crypto_status_waiting")); break;
      case 1: step.label.Text(Loc("crypto_status_seen")); break;
      case 2:
        step.label.Text(hstring{Format("crypto_status_confirming", display.confirmations,
                                       display.confirmationsRequired)});
        break;
      default: step.label.Text(Loc("crypto_status_paid")); break;
    }
    const bool done = index < display.step || (display.paid && index == 3);
    const bool current = index == display.step && !display.paid;
    const auto accent = done ? colors::MakeBrush(colors::kUrGreen) : colors::MakeBrush(colors::kUrPink);
    step.dot.Stroke(done || current ? accent : pending);
    step.dot.Fill(done ? accent : colors::MakeBrush(kTransparent));
    step.label.Foreground(done || current ? colors::TextBrush() : pending);
    step.label.FontWeight(current ? winrt::Windows::UI::Text::FontWeights::SemiBold()
                                  : winrt::Windows::UI::Text::FontWeights::Normal());
  }
  // the one line that says where the payment is, for a screen reader, and
  // announced when the payment moves on
  automation::AutomationProperties::SetName(state.steps, Loc(display.statusKey));
  if (display.statusKey != state.statusKey) {
    state.statusKey = display.statusKey;
    if (display.review) {
      Announce(state.reviewText);
    } else if (display.showSteps && 0 <= display.step &&
               display.step < static_cast<int>(state.stepRows.size())) {
      Announce(state.stepRows[static_cast<size_t>(display.step)].label);
    }
  }
  Show(state.remainingText, display.showRemaining);
  if (display.showRemaining) {
    state.remainingText.Text(hstring{Format("crypto_invoice_remaining", Widen(display.remainingText))});
  }
  Show(state.reviewText, display.review);

  // 6. help
  const std::string names = cryptopay::JoinNames(wallets);
  Show(state.walletsText, display.showWallets && !names.empty());
  if (!names.empty()) state.walletsText.Text(hstring{Format("crypto_invoice_wallets", Widen(names))});
  Show(state.zcashNote, display.showZcashNote);

  // a failure in this app's words (PaymentRefusal.h): the last requote's, or
  // an invoice the server no longer knows
  if (display.showUnavailable) {
    state.errorText.Text(hstring{Format("crypto_invoice_unavailable", MethodName(view.asset))});
  } else if (display.showRefusal) {
    const PaymentRefusalText refusal =
        PaymentRefusalTextFor(view.errorCode, view.errorMessage, "something_went_wrong");
    state.errorText.Text(hstring{PaymentRefusalMessage(Localized(refusal.key), Widen(refusal.detail))});
  }
  Show(state.errorText, display.showUnavailable || display.showRefusal);

  // 7. another way while nothing was received, or after a failure; else only
  // the sheet's Close, which hides the invoice and keeps it saved
  Show(state.payAnotherWay, display.showPayAnotherWay);
}

void CryptoInvoicePanel::Stop() {
  if (state_->copiedTimer) state_->copiedTimer.Stop();
}

void CryptoInvoicePanel::DrawQr(State& state, std::string const& uri) {
  state.qrCanvas.Children().Clear();
  state.qrUri.clear();
  int moduleCount = 0;
  std::vector<bool> dark;
  try {
    // level M: a payment uri carries a long address (Zcash's unified address
    // is over 200 characters), and M keeps the code at 57-69 modules, four dip
    // each in the frame (tools/crypto-pay-tests.cpp)
    const qrcodegen::QrCode code =
        qrcodegen::QrCode::encodeText(uri.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    moduleCount = code.getSize();
    dark.assign(static_cast<size_t>(moduleCount) * static_cast<size_t>(moduleCount), false);
    for (int y = 0; y < moduleCount; ++y) {
      for (int x = 0; x < moduleCount; ++x) {
        dark[static_cast<size_t>(y) * static_cast<size_t>(moduleCount) + static_cast<size_t>(x)] =
            code.getModule(x, y);
      }
    }
  } catch (const std::exception& e) {
    // qrcodegen::data_too_long: no picture, but the address and Open in
    // wallet still pay
    LogWarn("crypto invoice: qr encode failed: {}", e.what());
    return;
  }
  const cryptopay::QrLayout layout = cryptopay::QrLayoutFor(moduleCount, state.qrMaxSide);
  if (layout.moduleCount <= 0) return;
  state.qrCanvas.Width(layout.codeSide);
  state.qrCanvas.Height(layout.codeSide);
  state.qrFrame.Padding(Thickness{layout.quietZone, layout.quietZone, layout.quietZone, layout.quietZone});
  const auto ink = colors::MakeBrush(kQrDark);
  // one rectangle per horizontal run of dark modules, nothing cleared
  for (const ExtenderQrRun& run : ExtenderQrRunsFor(moduleCount, dark, 0, 0)) {
    ShapeRectangle cell;
    cell.Width(run.length * layout.moduleSize);
    cell.Height(layout.moduleSize);
    cell.Fill(ink);
    Canvas::SetLeft(cell, run.x * layout.moduleSize);
    Canvas::SetTop(cell, run.y * layout.moduleSize);
    state.qrCanvas.Children().Append(cell);
  }
  state.qrUri = uri;
}

void CryptoInvoicePanel::ShowCopied(State& state, Button const& copied) {
  for (Button const& button : {state.copyAmount, state.copyAddress, state.copyMemo}) {
    if (button == copied) {
      button.Content(winrt::box_value(Loc("crypto_invoice_copied")));
    } else {
      button.Content(winrt::box_value(Loc("crypto_invoice_copy")));
    }
  }
  if (state.copiedTimer) {
    state.copiedTimer.Stop();
    state.copiedTimer.Start();
  }
}

// ---- CryptoAccountRows ---------------------------------------------------------

StackPanel CryptoAccountRows::Build() {
  State& state = *state_;
  state.onRenew = &onRenew;
  state.onOpenInvoice = &onOpenInvoice;
  auto weak = std::weak_ptr<State>(state_);
  state.root = StackPanel();

  // a plan paid once in crypto: nothing renews it, nothing cancels it
  state.doesNotRenewRow = Border();
  state.doesNotRenewRow.Padding(Thickness{12, 10, 12, 10});
  state.doesNotRenewRow.BorderBrush(colors::BorderBrush());
  state.doesNotRenewRow.BorderThickness(Thickness{0, 0, 0, 1});
  state.doesNotRenewRow.Child(MakeText(Loc("crypto_does_not_renew"), 13, colors::MutedBrush(), true));
  state.doesNotRenewRow.Visibility(Visibility::Collapsed);
  state.root.Children().Append(state.doesNotRenewRow);

  // the paid term has ended: renew within the grace (an in-app notice, never
  // a system notification)
  state.termNotice = InfoBar();
  state.termNotice.Severity(InfoBarSeverity::Warning);
  state.termNotice.IsClosable(false);
  state.termNotice.IsOpen(false);
  state.termNotice.Margin(Thickness{12, 8, 12, 8});
  Button renew;
  renew.Content(winrt::box_value(Loc("crypto_term_renew")));
  automation::AutomationProperties::SetAutomationId(renew, L"acceptance.account.crypto-renew");
  renew.Click([weak](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      if (self->onRenew && *self->onRenew) (*self->onRenew)();
    }
  });
  state.termNotice.ActionButton(renew);
  state.termNotice.Visibility(Visibility::Collapsed);
  state.root.Children().Append(state.termNotice);

  // a saved invoice not paid yet: the row reopens it
  auto row = kit::MakePaneTwoLineRowButton(Loc("crypto_payment_in_progress"));
  automation::AutomationProperties::SetAutomationId(row.root, L"acceptance.account.crypto-in-progress");
  row.root.Click([weak](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      if (self->onOpenInvoice && *self->onOpenInvoice) (*self->onOpenInvoice)();
    }
  });
  row.root.Visibility(Visibility::Collapsed);
  state.inProgress = row.root;
  state.inProgressNote = row.note;
  state.inProgressValue = row.value;
  state.root.Children().Append(row.root);
  return state.root;
}

void CryptoAccountRows::ApplyBalance(BalanceSnapshot const& balance) {
  State& state = *state_;
  if (!state.root) return;
  // the sdk's rule: from the paid term's end until the grace ends, never before
  const bool notice = !balance.guest && cryptopay::TermNoticeDue(balance.cryptoTerm);
  if (notice) {
    // "Renew by <the grace's last day>", in the user's locale and time zone
    std::wstring date;
    if (const std::optional<int64_t> graceEnd = cryptopay::ParseRfc3339(balance.cryptoTerm.graceEndTime)) {
      try {
        winrt::Windows::Globalization::DateTimeFormatting::DateTimeFormatter formatter(L"shortdate");
        date = std::wstring{formatter.Format(
            winrt::clock::from_sys(std::chrono::sys_seconds{std::chrono::seconds{*graceEnd}}))};
      } catch (...) {
        date = Widen(balance.cryptoTerm.graceEndTime.substr(0, 10));
      }
    }
    state.termNotice.Message(hstring{Format("crypto_term_ended_notice", date)});
  }
  state.termNotice.IsOpen(notice);
  Show(state.termNotice, notice);
  Show(state.doesNotRenewRow,
       !balance.guest && cryptopay::DoesNotRenewShows(balance.subscriptionStoreFamily, notice));
}

void CryptoAccountRows::ApplyInvoice(cryptopay::CryptoInvoiceView const& view) {
  State& state = *state_;
  if (!state.root) return;
  const bool inProgress = cryptopay::ShowsPaymentInProgress(view);
  Show(state.inProgress, inProgress);
  if (!inProgress) return;
  // the status on the second line (cryptopay::InProgressNoteKey), the coin's
  // network at the right
  const std::string noteKey = cryptopay::InProgressNoteKey(view);
  const hstring status = noteKey.empty() ? hstring{}
                         : noteKey == "crypto_status_confirming"
                             ? hstring{Format("crypto_status_confirming", view.confirmations,
                                              view.confirmationsRequired)}
                             : Loc(noteKey);
  kit::SetTextOrCollapse(state.inProgressNote, status);
  const std::string networkKey = cryptopay::NetworkLineKey(view.asset);
  state.inProgressValue.Text(networkKey.empty() ? hstring{} : Loc(networkKey));
  automation::AutomationProperties::SetFullDescription(state.inProgress, status);
}

}  // namespace urnw
