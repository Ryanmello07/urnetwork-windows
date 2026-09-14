// The Earnings destination's Solana connect sheet: connect the Solana wallet
// legacy USDC payouts are paid to, until the migration to Bittensor completes.
// Opened from the "Connect Solana wallet" overflow item beside "Connect
// Bittensor wallet".
//
//   ConnectSolanaWalletSheet   two ways in. A wallet app: Phantom or Solflare
//                              over the ur.io/wallet-connect browser bridge,
//                              connect only (SdkHost::ConnectSolanaWallet hands
//                              back the public key; nothing is signed). Or a
//                              pasted Solana USDC address: checked locally,
//                              then by POST /wallet/validate-address for SOL.
//                              Either way the account wallet is created here
//                              (createAccountWallet {SOL, address, USDC}); the
//                              page makes it the payout wallet and reloads.
//
// Every decision is SolanaWalletPresentation's ConnectMachine; this unit only
// renders it and runs the requests. Plain C++ like EarningsSheets: no runtime
// classes; every method runs on the UI thread. Control handlers capture the
// sheet WEAKLY: the page holds the shared_ptr for the life of ShowAsync, so
// lock() always succeeds during interaction and a late answer after dismissal
// finds nothing.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "SdkHost.h"
#include "SolanaWalletPresentation.h"

namespace urnw {

class ConnectSolanaWalletSheet : public std::enable_shared_from_this<ConnectSolanaWalletSheet> {
 public:
  // `allowActions` is WalletPage::CanCallApi(): false under --preview-ui, where
  // the sheet still opens and still READS, but the providers and Connect are
  // disabled and a typed address is checked locally only. `onConnected` fires on
  // the UI thread with the new account wallet's id, just before the sheet
  // closes itself.
  static std::shared_ptr<ConnectSolanaWalletSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk, bool allowActions,
      std::function<void(std::string walletId)> onConnected);

  ~ConnectSolanaWalletSheet();

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  ConnectSolanaWalletSheet(SdkHost& sdk, bool allowActions,
                           std::function<void(std::string)> onConnected);

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  void ToggleManual();

  // ---- a wallet app
  void StartBridge(WalletConnect::Provider provider);
  void ApplyPublicKey(uint32_t generation, bool ok, std::string const& address,
                      std::string const& error);

  // ---- a pasted address
  void OnAddressChanged();
  void CheckAddress();  // the debounce fired
  void ApplyVerdict(uint32_t generation, solana::ServerVerdict verdict);
  void SubmitManual();

  // ---- either way
  void Link(std::string const& address);
  void ApplyCreateResult(uint32_t generation, bool ok, std::string const& walletId,
                         std::string const& error);

  // Arms the watchdog for the round trip or the request now going out, and
  // returns the generation its answer has to carry.
  uint32_t ArmFlow(int timeoutMs);
  void OnWatchdog();
  void StopTimers();
  // The controls, the status line, the verdict and the failure, from the machine.
  void Render();

  SdkHost& sdk_;
  bool allowActions_ = false;
  std::function<void(std::string)> onConnected_;
  solana::ConnectMachine machine_;
  bool manualOpen_ = false;
  bool closed_ = false;
  // A keystroke makes the check in flight stale; a new attempt, a watchdog or
  // the dismissal makes the round trip or the create call in flight stale.
  uint32_t checkGeneration_ = 0;
  uint32_t flowGeneration_ = 0;

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button phantomButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button solflareButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock statusText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel manualPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox addressBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock verdictText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button connectButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock errorText_{nullptr};
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer debounceTimer_{nullptr};
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer watchdog_{nullptr};
};

}  // namespace urnw
