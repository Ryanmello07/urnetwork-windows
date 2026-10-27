// The sdk's side of the past-due notice (PaymentPastDue.h). This is the one
// place that names SubscriptionBalanceResult.payment_past_due and the fields
// of urnet::PaymentPastDue, and the one place that reads the answer of the
// auth code the "Update payment method" action asks for. Everything else in
// the app reads PaymentPastDueView and the code, so a change in the generated
// wrapper is a change here alone. The wire's update_payment_url is never read:
// the action's url comes from pastdue::kManageSubscriptionUrl.
//
// Header-only and WinRT-free. It needs the generated urnetwork_sdk.hpp (and
// nlohmann/json), so tools/payment-past-due-tests.cpp builds it against a
// header that has payment_past_due (URNETWORK_SDK_INCLUDE, the vendored
// app/third_party/urnetwork-sdk/<arch>, or the sdk checkout beside this
// repository). That pins every field name and type read below to the wrapper.
// Nothing here calls the C ABI, so the spec links without the sdk library.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string>

#include <urnetwork_sdk.hpp>

#include "PaymentPastDue.h"

namespace urnw::pastdue {

// SubscriptionBalanceResult.payment_past_due: present while Stripe retries a
// failed renewal; absent otherwise and from an older server.
inline PaymentPastDueView PastDueFromSdk(const urnet::SubscriptionBalanceResult& result) {
  PaymentPastDueView view;
  if (!result.payment_past_due) return view;
  view.present = true;
  view.store = result.payment_past_due->store;
  view.failTime = result.payment_past_due->fail_time;
  return view;
}

// The request for the action's one-time code: one use within five minutes.
inline urnet::AuthCodeCreateArgs UpdatePaymentAuthCodeArgs() {
  urnet::AuthCodeCreateArgs args;
  args.duration_minutes = kAuthCodeDurationMinutes;
  args.uses = kAuthCodeUses;
  return args;
}

// The code authCodeCreate answered with, or "" when there is none: a failure
// with no answer from the server (`err`), the server's refusal (the auth-code
// limit among them), or an answer without a code. "" opens the bare url
// (UpdatePaymentUrl), and ur.io asks the user to sign in.
inline std::string AuthCodeFromSdk(const std::optional<urnet::AuthCodeCreateResult>& result,
                                   const std::optional<std::string>& err) {
  if (err || !result || result->error || !result->auth_code) return std::string();
  return *result->auth_code;
}

}  // namespace urnw::pastdue
