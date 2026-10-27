// The past-due notice for a failed Stripe renewal. When a renewal is declined,
// Stripe retries it for about two weeks and then cancels the subscription.
// While it retries, the server reports the renewal on the balance
// (SubscriptionBalanceResult.payment_past_due), and the Account plan pane shows
// "Your UR Pro payment didn't go through..." with "Update payment method". An
// account with no email (a wallet or phone sign-in) learns about the failure
// only from this notice. It shows exactly while the latest balance carries the
// field: there is no time rule and no state kept here, and an older server,
// which never sends the field, never shows it.
//
// The action opens ur.io's Manage Subscription in the browser, signed in with
// a one-time auth code (POST /auth/code-create), the way apple's
// manageSubscriptionOnWebURL(authCode:) does. ur.io consumes the code, removes
// it from the address bar, and asks before it switches a browser that is
// signed in to another network. The url is built from kManageSubscriptionUrl
// alone and never from the wire's update_payment_url, so a sign-in code is
// only ever attached to ur.io.
//
// PaymentPastDueSdk.h is the one place that reads the sdk's struct into
// PaymentPastDueView. WinRT/SDK-free so tools/payment-past-due-tests.cpp runs
// it on any host.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "UrlQuery.h"

namespace urnw::pastdue {

// ur.io's Manage Subscription (server StripeUpdatePaymentMethodUrl). Its
// "Update payment method" opens the Stripe billing portal for the network
// signed in there.
inline constexpr const char* kManageSubscriptionUrl = "https://ur.io/app/subscription";

// The one-time code's life: one use within five minutes, as apple's
// manage-on-web code and the Settings auth code sheet make it.
inline constexpr double kAuthCodeDurationMinutes = 5;
inline constexpr int64_t kAuthCodeUses = 1;

// The balance's payment_past_due, as this app reads it.
struct PaymentPastDueView {
  // the balance carries payment_past_due
  bool present = false;
  // the store retrying the renewal: "stripe"
  std::string store;
  // when the renewal first failed, RFC 3339
  std::string failTime;
};

// Exactly while the latest balance carries payment_past_due.
inline bool NoticeShows(const PaymentPastDueView& view) {
  return view.present;
}

// The code without the white space around it (apple trims
// whitespacesAndNewlines; the server's codes are base64url, all ASCII).
inline std::string TrimmedAuthCode(std::string_view code) {
  const auto space = [](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
  };
  size_t begin = 0;
  size_t end = code.size();
  while (begin < end && space(code[begin])) ++begin;
  while (begin < end && space(code[end - 1])) --end;
  return std::string(code.substr(begin, end - begin));
}

// "https://ur.io/app/subscription?auth_code=<code>", the code percent-encoded
// as one query value by the app's one encoder (UrlQuery.h PercentEncode). It
// encodes every byte but the RFC 3986 unreserved characters, so "&", "=",
// "+", "#", "?" and a space in a code can neither add a parameter or a
// fragment nor read as a space. That is a superset of what apple encodes, and
// for a server code (base64url, "=" padded) the url is byte for byte apple's.
// A blank code (none could be made) gives the bare url, and ur.io asks the
// user to sign in.
inline std::string UpdatePaymentUrl(std::string_view authCode) {
  const std::string code = TrimmedAuthCode(authCode);
  if (code.empty()) return kManageSubscriptionUrl;
  return std::string(kManageSubscriptionUrl) + "?auth_code=" + PercentEncode(code);
}

}  // namespace urnw::pastdue
