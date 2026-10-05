// A legacy guest network (UPGRADE.md A4, D8): a network minted before guest
// mode was retired, with no login method at all. It can hold a plan and a
// balance, so "Create an account" must never leave it: signing out abandons
// the network for good (nothing can sign back in), and signing in to a new
// one strands the balance.
//
// Who is a guest: the jwt's GuestMode claim OR the server's
// SubscriptionBalanceResult.guest. The claim alone misses a guest whose token
// was refreshed (every refresh signs the jwt without it); the server reads the
// live auth methods. A claim that is still set (an older server, or a guest
// that just added a login method and has not refreshed yet) still counts.
//
// The conversion adds a login method to the CURRENT network in place:
// email/phone + password -> addAuth -> re-sign the jwt and re-read the balance
// (the network has a login method now, so it is no longer a guest) -> send a
// verification code -> authVerify. The jwt authVerify returns is not
// installed: the session never leaves the network, and there is no sign-out
// anywhere in this flow.
//
// The same flow adds an email/phone sign-in from Settings (AddAuthSheet): an
// added email or phone counts as added only once its code is verified. That
// session's RefreshJwt / RefreshBalance do nothing (the network was never a
// guest). SSO sign-ins are verified by their provider and wallets by their
// signature; neither is added through this flow.
//
// After a rate-limited send, Resend is refused until the server's retry time
// passes on the injected clock, and the notice counts the minutes down (the
// sheet re-renders each second while CoolingDown()).
//
// Header-only and free of WinRT and the SDK, so tools/guest-conversion-tests.cpp
// pins it on any host against the header the app compiles. Not thread safe:
// the session delivers every answer on the UI thread.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "VerifySendNotice.h"

namespace urnw {

inline bool IsGuestNetwork(bool guestModeClaim, bool serverGuest) {
  return guestModeClaim || serverGuest;
}

// The server's code for a checkout or payment intent it refused because the
// network is a legacy guest (server refuseGuestPurchase; the SDK's
// urnet::PurchaseErrorCodeGuestSignInRequired).
inline constexpr std::string_view kPurchaseErrorCodeGuestSignInRequired = "guest_sign_in_required";

// What a refused payment sheet or checkout session leads to. The server refuses
// a guest network with guest_sign_in_required when the app did not know it was
// one (a refreshed guest before its balance loaded): no later checkout stage
// can sell it a plan, so the upgrade sheet closes and the conversion opens, as
// it does for every purchase entry of a known guest. Any other refusal is the
// payment error it always was.
enum class PurchaseRefusal { PaymentError, AddSignIn };

inline PurchaseRefusal PurchaseRefusalFor(std::string_view code) {
  return code == kPurchaseErrorCodeGuestSignInRequired ? PurchaseRefusal::AddSignIn
                                                       : PurchaseRefusal::PaymentError;
}

// the server's minimum (model.MinPasswordLength), and AddAuthSheet's gate
inline constexpr size_t kGuestConversionMinPasswordLength = 12;

// What the conversion needs from the app. Every `done` runs on the UI thread.
class GuestConversionSession {
 public:
  virtual ~GuestConversionSession() = default;
  // Api addAuth{user_auth, password} on the current network; "" on success,
  // else the message to show.
  virtual void AddSignIn(const std::string& userAuth, const std::string& password,
                         std::function<void(std::string error)> done) = 0;
  // Re-sign the jwt for the same network (the refresh drops GuestMode).
  virtual void RefreshJwt() = 0;
  // Re-read the subscription balance (its `guest` turns false).
  virtual void RefreshBalance() = 0;
  // Api authVerifySend{user_auth}.
  virtual void SendCode(const std::string& userAuth,
                        std::function<void(VerifySendNotice notice)> done) = 0;
  // Api authVerify{user_auth, verify_code}, without installing its jwt; ""
  // on success, else the message to show.
  virtual void VerifyCode(const std::string& userAuth, const std::string& code,
                          std::function<void(std::string error)> done) = 0;
};

enum class GuestConversionStep {
  EnterSignIn,   // email/phone + password
  AddingSignIn,  // addAuth in flight
  EnterCode,     // the login method exists; verify it with the emailed code
  Verifying,     // authVerify in flight
  Done,          // added and verified
};

class GuestConversion {
  std::shared_ptr<uint64_t> generation_ = std::make_shared<uint64_t>(0);

  // Wraps an answer so it is dropped after a Reset or destruction.
  template <typename F>
  auto Guard(F f) {
    return [generation = generation_, seen = *generation_, f = std::move(f)](auto value) mutable {
      if (*generation != seen) return;
      f(std::move(value));
    };
  }

 public:
  // `now` is the cooldown's clock; tests inject it.
  explicit GuestConversion(GuestConversionSession& session,
                           std::function<ResendCooldown::Clock::time_point()> now =
                               ResendCooldown::Clock::now)
      : session_(session), now_(std::move(now)) {}
  ~GuestConversion() { ++*generation_; }  // drops every answer still in flight
  GuestConversion(const GuestConversion&) = delete;
  GuestConversion& operator=(const GuestConversion&) = delete;

  // Called after every step, error or notice change.
  std::function<void()> on_changed;

  GuestConversionStep Step() const { return step_; }
  // the last failure to show, "" for none
  const std::string& Error() const { return error_; }
  const std::string& UserAuth() const { return userAuth_; }
  // the last code send's outcome, none before the first send answers
  const std::optional<VerifySendNotice>& Notice() const { return notice_; }
  // the notice to show now: a rate limit counts its minutes down and clears
  // once a new code can be requested
  std::optional<VerifySendNotice> ShownNotice() const {
    if (notice_ && notice_->kind == VerifySendNoticeKind::RateLimited && cooldown_.Armed()) {
      const auto now = now_();
      if (cooldown_.CanSend(now)) return std::nullopt;
      VerifySendNotice notice = *notice_;
      notice.minutes = cooldown_.Minutes(now);
      notice.retryAfterSeconds = cooldown_.RemainingSeconds(now);
      return notice;
    }
    return notice_;
  }
  // a rate limit is still running
  bool CoolingDown() const { return !cooldown_.CanSend(now_()); }
  bool CanResend() const {
    return step_ == GuestConversionStep::EnterCode && !sending_ && !CoolingDown();
  }
  bool Busy() const {
    return step_ == GuestConversionStep::AddingSignIn || step_ == GuestConversionStep::Verifying ||
           sending_;
  }

  static std::string Trim(const std::string& value) {
    const char* space = " \t\r\n";
    const size_t start = value.find_first_not_of(space);
    if (start == std::string::npos) return std::string();
    return value.substr(start, value.find_last_not_of(space) - start + 1);
  }

  static bool CanSubmitSignIn(const std::string& userAuth, const std::string& password) {
    return !Trim(userAuth).empty() && kGuestConversionMinPasswordLength <= password.size();
  }

  // Back to the first step (the sheet reopened); drops answers in flight.
  void Reset() {
    ++*generation_;
    step_ = GuestConversionStep::EnterSignIn;
    error_.clear();
    userAuth_.clear();
    notice_.reset();
    cooldown_.Clear();
    sending_ = false;
    Changed();
  }

  void SubmitSignIn(const std::string& userAuth, const std::string& password) {
    if (step_ != GuestConversionStep::EnterSignIn || !CanSubmitSignIn(userAuth, password)) return;
    userAuth_ = Trim(userAuth);
    step_ = GuestConversionStep::AddingSignIn;
    error_.clear();
    Changed();
    session_.AddSignIn(userAuth_, password, Guard([this](std::string error) {
      if (!error.empty()) {
        step_ = GuestConversionStep::EnterSignIn;
        error_ = std::move(error);
        Changed();
        return;
      }
      // The network has a login method now: it is no longer a guest even if
      // the code step is abandoned, so lift the guest state right away.
      session_.RefreshJwt();
      session_.RefreshBalance();
      step_ = GuestConversionStep::EnterCode;
      Changed();
      Send();
    }));
  }

  void Resend() {
    if (!CanResend()) return;
    Send();
  }

  void SubmitCode(const std::string& code) {
    const std::string trimmed = Trim(code);
    if (step_ != GuestConversionStep::EnterCode || trimmed.empty()) return;
    step_ = GuestConversionStep::Verifying;
    error_.clear();
    Changed();
    session_.VerifyCode(userAuth_, trimmed, Guard([this](std::string error) {
      if (!error.empty()) {
        step_ = GuestConversionStep::EnterCode;
        error_ = std::move(error);
        Changed();
        return;
      }
      step_ = GuestConversionStep::Done;
      session_.RefreshBalance();
      Changed();
    }));
  }

 private:
  void Send() {
    sending_ = true;
    Changed();
    session_.SendCode(userAuth_, Guard([this](VerifySendNotice notice) {
      sending_ = false;
      cooldown_.Start(notice, now_());
      notice_ = std::move(notice);
      Changed();
    }));
  }

  void Changed() {
    if (on_changed) on_changed();
  }

  GuestConversionSession& session_;
  std::function<ResendCooldown::Clock::time_point()> now_;
  GuestConversionStep step_ = GuestConversionStep::EnterSignIn;
  std::string error_;
  std::string userAuth_;
  std::optional<VerifySendNotice> notice_;
  ResendCooldown cooldown_;
  bool sending_ = false;
};

// A purchase entry (Get Pro, the upgrade sheet, the onboarding checkout) that
// sent a guest to the conversion continues to the checkout it was opening once
// the conversion is done and the network no longer reads as a guest. Without
// it the conversion closed back to where the user started, and the upgrade
// had to be opened again. A cancelled conversion drops it; the checkout stays
// closed to a guest (the server's `guest` clears once the balance is re-read).
class GuestUpgradeContinuation {
 public:
  // `checkout` opens what the entry was opening.
  void Divert(std::function<void()> checkout) {
    checkout_ = std::move(checkout);
    converted_ = false;
  }

  // The sign-in was added and verified.
  void ConversionDone() {
    if (checkout_) converted_ = true;
  }

  // The conversion closed: a close before it was done drops the checkout.
  void ConversionClosed() {
    if (!converted_) checkout_ = nullptr;
  }

  // Called when the conversion closes and whenever the guest status changes;
  // opens the checkout once, when the conversion is done and the network is no
  // longer a guest.
  void Poll(bool isGuest) {
    if (!converted_ || !checkout_ || isGuest) return;
    auto checkout = std::move(checkout_);
    checkout_ = nullptr;
    converted_ = false;
    checkout();
  }

  void Clear() {
    checkout_ = nullptr;
    converted_ = false;
  }

  bool Pending() const { return checkout_ != nullptr; }

 private:
  std::function<void()> checkout_;
  bool converted_ = false;
};

}  // namespace urnw
