// Where Settings sends a user who wants to reach the project, decided pure so
// tools/support-contact-tests.cpp can pin it on any host against the header the
// app compiles.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace urnw::support {

inline constexpr std::wstring_view kSupportEmail = L"support@ur.io";
inline constexpr std::wstring_view kSupportEmailUrl = L"mailto:support@ur.io";

// One row of the About pane's Stay in touch card.
enum class StayInTouchLink {
  Discord,       // join_the_community_on_discord_https_discord_com
  SupportEmail,  // email_support_at + the address
  DePinHub,      // verified_project_on_depin_hub_https_depinhub_io
};

// The card's rows, in order. The Discord invite is unreachable in some
// regions, so the support address sits right under it.
inline constexpr std::array<StayInTouchLink, 3> kStayInTouchLinks = {
    StayInTouchLink::Discord,
    StayInTouchLink::SupportEmail,
    StayInTouchLink::DePinHub,
};

// Every place Discord is offered must offer the support address too.
template <size_t N>
bool OffersSupportEmailWithDiscord(std::array<StayInTouchLink, N> const& links) {
  const auto has = [&links](StayInTouchLink link) {
    return std::find(links.begin(), links.end(), link) != links.end();
  };
  return !has(StayInTouchLink::Discord) || has(StayInTouchLink::SupportEmail);
}

// The store's label ("Email support at") with the address as an inline
// markdown link, for SetMarkdownLinkText.
inline std::wstring SupportEmailMarkdown(std::wstring_view label) {
  std::wstring markdown{label};
  markdown += L" [";
  markdown += kSupportEmail;
  markdown += L"](";
  markdown += kSupportEmailUrl;
  markdown += L")";
  return markdown;
}

}  // namespace urnw::support
