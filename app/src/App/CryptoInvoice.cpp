// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "CryptoInvoice.h"

#include <exception>
#include <system_error>
#include <utility>

#include "CryptoPaySdk.h"
#include "Log.h"
#include "Paths.h"
#include "Strings.h"

namespace urnw {

CryptoInvoiceStore::~CryptoInvoiceStore() {
  *alive_ = false;  // a change still queued for the UI thread stops here
  try {
    Close();
  } catch (...) {
    // the sdk may already be shutting down at teardown; nothing left to close on
  }
}

void CryptoInvoiceStore::Initialize(winrt::Microsoft::UI::Dispatching::DispatcherQueue queue) {
  queue_ = queue;
}

std::filesystem::path CryptoInvoiceStore::StorageDir() {
  // the app's own per-user root (%LOCALAPPDATA%\URnetwork\app, or the
  // worktree's URNETWORK_APP_ROOT), beside the sdk's storage
  return StorageRoot(/*isService=*/false) / "crypto_pay";
}

void CryptoInvoiceStore::Start() {
  Close();  // a repeated sign-in push opens a fresh controller
  // IsLoggedIn(), not apiReady(): apiReady is set at SDK init, not at sign-in
  if (!queue_ || !sdk_.apiReady() || !sdk_.IsLoggedIn()) {
    Read();
    return;
  }
  // Without its directory the invoice is not saved (no resume, no remembered
  // method) but can still be paid: the sdk takes "" as "no persistence".
  std::error_code dirError;
  const std::filesystem::path dir = StorageDir();
  std::filesystem::create_directories(dir, dirError);
  if (dirError) {
    LogWarn("crypto invoice: no storage directory ({}); the invoice is not saved",
            dirError.message());
  }
  const std::string storage = dirError ? std::string() : Narrow(dir.wstring());
  try {
    controller_.emplace(urnet::newCryptoInvoiceViewController(sdk_.api(), storage));
  } catch (const std::exception& e) {
    LogError("crypto invoice: could not open the controller: {}", e.what());
    controller_.reset();
    Read();
    return;
  }
  if (!*controller_) {
    // a handle of 0: the sdk refused the Api
    LogError("crypto invoice: the sdk opened no controller");
    controller_.reset();
    Read();
    return;
  }
  const uint64_t generation = ++generation_;
  auto queue = queue_;
  auto alive = alive_;
  try {
    sub_.emplace(controller_->addCryptoInvoiceListener([this, queue, alive, generation] {
      // an sdk thread: only the hop. The state is read on the UI thread, from
      // the controller the generation names, and only while it is open.
      queue.TryEnqueue([this, alive, generation] {
        if (!*alive || generation != generation_) return;
        Read();
      });
    }));
  } catch (const std::exception& e) {
    LogError("crypto invoice: could not listen to the controller: {}", e.what());
  }
  // the invoice saved for this network, at launch and at sign-in
  Resume();
  ApplyRunning();
}

void CryptoInvoiceStore::Close() {
  ++generation_;  // a change already queued for this controller is dropped
  sub_.reset();   // unsubscribes, before the controller closes
  if (controller_) {
    try {
      if (running_) controller_->stop();
      controller_->close();
    } catch (const std::exception& e) {
      LogWarn("crypto invoice: closing the controller failed: {}", e.what());
    }
    controller_.reset();
  }
  running_ = false;
}

void CryptoInvoiceStore::Stop(bool signedOut) {
  Close();
  if (signedOut) {
    // the session's saved invoice (its status token among it) and the
    // remembered method do not outlive the sign-in
    std::error_code error;
    std::filesystem::remove_all(StorageDir(), error);
    if (error) LogWarn("crypto invoice: could not clear the saved invoice: {}", error.message());
  }
  Read();
}

void CryptoInvoiceStore::SetPresenting(bool presenting) {
  presenting_ = presenting;
  ApplyRunning();
}

void CryptoInvoiceStore::SetShowing(bool showing) {
  showing_ = showing;
  ApplyRunning();
}

void CryptoInvoiceStore::ApplyRunning() {
  // polls and ticks only while an invoice is on screen in a presenting window:
  // nothing runs in the background (ZCASH.md §0.3)
  const bool run = controller_.has_value() && presenting_ && showing_;
  if (run == running_) return;
  running_ = run;
  try {
    if (run) {
      controller_->start();
    } else if (controller_) {
      controller_->stop();
    }
  } catch (const std::exception& e) {
    LogWarn("crypto invoice: could not {} the controller: {}", run ? "start" : "stop", e.what());
  }
}

bool CryptoInvoiceStore::StartInvoice(std::string const& asset, std::string const& plan) {
  if (!controller_) return false;
  bool started = true;
  try {
    // no storefront on the desktop: the server resolves the price tier itself,
    // as it does for the balance call (SubscriptionBalanceStore::Fetch)
    controller_->startInvoice(asset, plan, /*item_id=*/std::string(),
                              /*storefront_country=*/std::string());
  } catch (const std::exception& e) {
    LogError("crypto invoice: the start failed: {}", e.what());
    started = false;
  }
  Read();
  return started;
}

bool CryptoInvoiceStore::Resume() {
  if (!controller_) return false;
  bool resumed = false;
  try {
    resumed = controller_->resume();
  } catch (const std::exception& e) {
    LogWarn("crypto invoice: resume failed: {}", e.what());
  }
  Read();
  return resumed;
}

void CryptoInvoiceStore::Requote() {
  if (!controller_) return;
  try {
    controller_->requote();
  } catch (const std::exception& e) {
    LogWarn("crypto invoice: requote failed: {}", e.what());
  }
  Read();
}

void CryptoInvoiceStore::Dismiss() {
  if (!controller_) return;
  try {
    controller_->dismiss();
  } catch (const std::exception& e) {
    LogWarn("crypto invoice: dismiss failed: {}", e.what());
  }
  Read();
}

std::string CryptoInvoiceStore::PaymentMethod() const {
  if (!controller_) return std::string();
  try {
    return controller_->getPaymentMethod();
  } catch (const std::exception& e) {
    LogWarn("crypto invoice: reading the payment method failed: {}", e.what());
    return std::string();
  }
}

void CryptoInvoiceStore::SetPaymentMethod(std::string const& method) {
  if (!controller_) return;
  try {
    controller_->setPaymentMethod(method);
  } catch (const std::exception& e) {
    LogWarn("crypto invoice: saving the payment method failed: {}", e.what());
  }
}

void CryptoInvoiceStore::Read() {
  cryptopay::CryptoInvoiceView view;
  if (controller_) {
    try {
      if (const std::optional<urnet::CryptoInvoiceState> state = controller_->getState()) {
        view = cryptopay::ViewFromSdk(*state);
      }
    } catch (const std::exception& e) {
      LogWarn("crypto invoice: reading the state failed: {}", e.what());
      return;  // the last state stands
    }
  }
  // The asset, phase and status only: never the intent, its token, an
  // address or an amount (CONTRACT §3.11).
  if (view.phase != view_.phase || view.status != view_.status) {
    LogInfo("crypto invoice: {} {} {}", view.asset.empty() ? "-" : view.asset,
            view.phase.empty() ? "none" : view.phase, view.status.empty() ? "-" : view.status);
  }
  view_ = std::move(view);
  if (onChange_) onChange_(view_);
}

}  // namespace urnw
