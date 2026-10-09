// The cloud proxies page on ur.io that Settings opens, decided pure so
// tools/cloud-proxy-link-tests.cpp can pin it on any host against the header
// the app compiles.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string_view>

namespace urnw::cloudproxy {

// The app has no protocol switch: WireGuard, SOCKS and HTTPS proxies are
// created on ur.io (SOCKS and WireGuard on Pro), and Settings opens this page.
// It carries no credential; ur.io asks the user to sign in when it needs to.
inline constexpr std::wstring_view kProxiesUrl = L"https://ur.io/app/proxies";

}  // namespace urnw::cloudproxy
