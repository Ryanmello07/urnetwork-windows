// The referral invitation the share actions copy (support inbox 1698): the
// localized message, which names the code, then the code's ur.io/c link on its
// own line, in the shape of the sdk's ConnectLinkUrl for bonus=<code>:
// ur.io/c opens it in the Android app (or Play, with the link as the install
// referrer) and in web signup everywhere else. The code stays in the message:
// installs without a Play referrer still type it. Pure so
// tools/referral-share-tests.cpp pins it against the header the app compiles.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <string_view>

#include "UrlQuery.h"

namespace urnw {

// https://<link host>/c?bonus=<code>, or "" without a code. The code is
// percent-encoded (UrlQuery.h), so it never adds a parameter to the link.
inline std::string ReferralLinkUrl(std::string_view linkHostName, std::string_view code) {
  if (code.empty()) return std::string();
  return "https://" + std::string(linkHostName) + "/c?bonus=" + PercentEncode(std::string(code));
}

// The message, then the link on its own line; the message alone without a
// link.
inline std::wstring ReferralShareText(std::wstring message, std::wstring_view link) {
  if (link.empty()) return message;
  message += L'\n';
  message.append(link);
  return message;
}

}  // namespace urnw
