// The crypto payment views (crypto-pay CONTRACT §6), built in code like every
// sheet here, and drawn from the decisions in CryptoPayPresentation.h:
//
//   PaymentMethodRow   the segmented Card · USDC · Zcash · Monero row
//                      (SelectorBar) directly above a buy button, shared by
//                      the upgrade sheet's products page and the onboarding
//                      welcome and offer pages, with "Temporarily
//                      unavailable" under it after a coin failed to quote;
//   CryptoInvoicePanel the native invoice, the upgrade sheet's Invoice page:
//                      the network line, the QR of the server's uri (only
//                      once the sdk's check passed), Open in wallet, the
//                      amount, the address and the memo with Copy, the quote's
//                      countdown with Refresh quote, the status steps, the
//                      wallets that work and Pay another way;
//   CryptoAccountRows  the Account plan pane's rows: "Does not renew" under a
//                      plan paid once in crypto, the crypto_term notice with
//                      Renew, and "Payment in progress" for a saved invoice.
//
// Each keeps its controls in a shared State the XAML handlers capture weakly
// (PlanPicker's pattern): a control tapped after its host is gone touches
// nothing freed. All of it runs on the UI thread.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include "CryptoPayPresentation.h"

namespace urnw {

struct BalanceSnapshot;

// ---- the payment-method row (§6.1) ------------------------------------------
class PaymentMethodRow {
 public:
  // The row and its unavailable line, collapsed until SetMethods lists a coin.
  winrt::Microsoft::UI::Xaml::Controls::StackPanel Build();
  // The methods the server sells on this surface (cryptopay::PaymentMethodsFor:
  // Card first) and the method wanted (the device's remembered one, or the
  // selection the user made here). Rebuilds the segments when the list
  // changed and selects cryptopay::MethodToSelect: Card when the wanted coin
  // is not listed, and Card with the row hidden when no coin is.
  void SetMethods(std::vector<std::string> const& methods, std::string const& wanted);
  // The selection: Card while the row is hidden.
  std::string Method() const { return state_->selected; }
  // Select programmatically (a coin must be listed): repaints, does NOT fire
  // onSelect.
  void Select(std::string const& method);
  void SetEnabled(bool enabled);
  // "Temporarily unavailable" under the row while `method` (the coin whose
  // start failed with asset_unavailable) is selected; "" clears it. The coin
  // stays selectable for a retry.
  void SetUnavailable(std::string const& method);

  // Fired on a tap, after the selection changed.
  std::function<void(std::string const& method)> onSelect;

 private:
  struct State {
    winrt::Microsoft::UI::Xaml::Controls::StackPanel root{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::SelectorBar bar{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock unavailable{nullptr};
    std::vector<std::string> methods;
    std::vector<winrt::Microsoft::UI::Xaml::Controls::SelectorBarItem> items;
    std::string selected = cryptopay::kMethodCard;
    std::string unavailableMethod;
    // a programmatic selection: SelectionChanged stays quiet
    bool applying = false;
    std::function<void(std::string const&)>* onSelect = nullptr;
  };
  static void Apply(State& state);

  std::shared_ptr<State> state_ = std::make_shared<State>();
};

// ---- the invoice (§6.2) --------------------------------------------------------
class CryptoInvoicePanel {
 public:
  // The invoice's content, for the sheet to place; `qrMaxSide` is the room the
  // QR's frame may take (cryptopay::QrLayoutFor).
  winrt::Microsoft::UI::Xaml::Controls::StackPanel Build(
      winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue, double qrMaxSide);
  // Paint one state. `uriAllowed` is cryptopay::UriAllowed's answer for it,
  // which alone lets the QR and Open in wallet show; `wallets` the sdk's
  // suggestions for its asset. The QR is encoded again only when its uri
  // changes; the countdown repaints on every tick.
  void Render(cryptopay::CryptoInvoiceView const& view, bool uriAllowed,
              std::vector<std::string> const& wallets);
  // the Copied feedback's timer, with the sheet
  void Stop();

  std::function<void()> onOpenWallet;
  std::function<void()> onRefresh;
  std::function<void()> onPayAnotherWay;

 private:
  struct Step {
    winrt::Microsoft::UI::Xaml::Controls::StackPanel row{nullptr};
    winrt::Microsoft::UI::Xaml::Shapes::Ellipse dot{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock label{nullptr};
  };
  struct State {
    double qrMaxSide = 0;
    winrt::Microsoft::UI::Xaml::Controls::TextBlock networkLine{nullptr};
    // a start in flight, or a resumed invoice before its first status: a
    // brief loading line in place of the payment details
    winrt::Microsoft::UI::Xaml::Controls::StackPanel busyLine{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ProgressRing busyRing{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel instructions{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Border qrFrame{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Canvas qrCanvas{nullptr};
    std::string qrUri;  // the uri the canvas holds ("" for none)
    winrt::Microsoft::UI::Xaml::Controls::Button openWallet{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel amountBlock{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock amountText{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button copyAmount{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel addressBlock{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock addressText{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button copyAddress{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock usdcWarning{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel memoBlock{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock memoText{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button copyMemo{nullptr};
    // the values the Copy buttons put on the clipboard: the bare amount (a
    // wallet's amount field takes no ticker), the address, the memo
    std::string amount;
    std::string address;
    std::string memo;
    winrt::Microsoft::UI::Xaml::Controls::Grid quoteRow{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock quoteText{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button refreshButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ProgressRing refreshRing{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel steps{nullptr};
    std::vector<Step> stepRows;  // waiting, seen, confirming, paid
    std::string statusKey;       // the status line last announced
    winrt::Microsoft::UI::Xaml::Controls::TextBlock remainingText{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock reviewText{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock walletsText{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock zcashNote{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock errorText{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button payAnotherWay{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer copiedTimer{nullptr};
    std::function<void()>* onOpenWallet = nullptr;
    std::function<void()>* onRefresh = nullptr;
    std::function<void()>* onPayAnotherWay = nullptr;
  };
  // the code for `uri` on the canvas, or the frame hidden when it will not encode
  static void DrawQr(State& state, std::string const& uri);
  // "Copied" on the button that copied, "Copy" back on the others
  static void ShowCopied(State& state, winrt::Microsoft::UI::Xaml::Controls::Button const& copied);

  std::shared_ptr<State> state_ = std::make_shared<State>();
};

// ---- the Account plan pane's rows (§6.2) ---------------------------------------
class CryptoAccountRows {
 public:
  // The block for the Account plan pane, every row collapsed until it has
  // something to say.
  winrt::Microsoft::UI::Xaml::Controls::StackPanel Build();
  // "Does not renew" under a plan paid once in crypto, and the crypto_term
  // notice while the paid term has ended and its grace has not.
  void ApplyBalance(BalanceSnapshot const& balance);
  // "Payment in progress" while a saved invoice is not paid.
  void ApplyInvoice(cryptopay::CryptoInvoiceView const& view);

  std::function<void()> onRenew;
  std::function<void()> onOpenInvoice;

 private:
  struct State {
    winrt::Microsoft::UI::Xaml::Controls::StackPanel root{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Border doesNotRenewRow{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::InfoBar termNotice{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button inProgress{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock inProgressNote{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock inProgressValue{nullptr};
    std::function<void()>* onRenew = nullptr;
    std::function<void()>* onOpenInvoice = nullptr;
  };

  std::shared_ptr<State> state_ = std::make_shared<State>();
};

}  // namespace urnw
