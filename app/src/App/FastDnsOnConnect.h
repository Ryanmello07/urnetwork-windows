// "Fast DNS on connect": the DNS settings toggle over the SDK's
// DnsResolverSettings.EnableFallback, which races a handicapped DoH resolver
// over the host's local network while the tunnel's DNS starts. It is an
// opt-in, off by default: DNS resolves only through the tunnel unless the user
// turns it on, because the fallback can reveal lookups to the local network.
// The SDK owns the default (getDefaultDnsResolverSettings) and migrates older
// saved settings to off; the app maps the flag through unchanged and never
// turns it on by itself.
//
// Pure standard C++, templated over the settings type, so
// tools/dns-settings-tests.cpp runs it on any host against a stand-in for
// urnet::DnsResolverSettings; the DNS editor sheet and the connect drawer DNS
// card use it with the real SDK type.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string_view>

namespace urnw::fast_dns_on_connect {

// The store keys for the editor toggle (also the drawer status row label) and
// the description under the toggle.
inline constexpr std::string_view kLabelKey = "fast_dns_on_connect";
inline constexpr std::string_view kDescriptionKey = "fast_dns_on_connect_description";

// With no applied settings to read, the toggle shows off.
template <class Settings>
bool FromSettings(std::optional<Settings> const& settings) {
  return settings ? settings->EnableFallback : false;
}

template <class Settings>
void ToSettings(Settings& settings, bool enabled) {
  settings.EnableFallback = enabled;
}

}  // namespace urnw::fast_dns_on_connect
