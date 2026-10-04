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
// A rate-limited code send holds Resend off until its retry time, and the
// notice counts the minutes down (ResendCooldown, as the login verify step).
// The session supplies the clock, so the countdown is testable.
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
#include <utility>

#include "VerifySendNotice.h"

namespace urnw {

inline bool IsGuestNetwork(bool guestModeClaim, bool serverGuest) {
  return guestModeClaim || serverGuest;
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
  // The clock the Resend cooldown runs on (steady_clock::now() in the app).
  virtual ResendCooldown::Clock::time_point Now() = 0;
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
  explicit GuestConversion(GuestConversionSession& session) : session_(session) {}
  ~GuestConversion() { ++*generation_; }  // drops every answer still in flight
  GuestConversion(const GuestConversion&) = delete;
  GuestConversion& operator=(const GuestConversion&) = delete;

  // Called after every step, error or notice change.
  std::function<void()> on_changed;

  GuestConversionStep Step() const { return step_; }
  // the last failure to show, "" for none
  const std::string& Error() const { return error_; }
  const std::string& UserAuth() const { return userAuth_; }
  // The last code send's outcome as it reads now, none before the first send
  // answers. A rate limit counts its minutes down and is gone once its retry
  // time has passed.
  std::optional<VerifySendNotice> Notice() const {
    if (!notice_ || notice_->kind != VerifySendNoticeKind::RateLimited || !cooldown_.Armed()) {
      return notice_;
    }
    const auto now = session_.Now();
    if (cooldown_.CanSend(now)) return std::nullopt;
    VerifySendNotice notice = *notice_;
    notice.minutes = cooldown_.Minutes(now);
    notice.retryAfterSeconds = cooldown_.RemainingSeconds(now);
    return notice;
  }
  // A rate limit is holding Resend off; the sheet re-renders every second
  // while it is, to count down and bring Resend back.
  bool CoolingDown() const { return !cooldown_.CanSend(session_.Now()); }
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
      cooldown_.Start(notice, session_.Now());
      notice_ = std::move(notice);
      Changed();
    }));
  }

  void Changed() {
    if (on_changed) on_changed();
  }

  GuestConversionSession& session_;
  GuestConversionStep step_ = GuestConversionStep::EnterSignIn;
  std::string error_;
  std::string userAuth_;
  std::optional<VerifySendNotice> notice_;
  ResendCooldown cooldown_;
  bool sending_ = false;
};

}  // namespace urnw
