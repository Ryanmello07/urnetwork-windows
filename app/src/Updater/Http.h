// The update helper's HTTPS GET: redirects refused, and the server's
// certificate trusted only as far as this machine's own trust goes.
//
// WinHTTP already checks the server's certificate, but against the trust of
// the user this process runs as, and the current user's root store is theirs
// to add to. The helper installs as an administrator what it downloads, so
// every response it reads also has its certificate chain built again against
// the machine's stores alone (HCCE_LOCAL_MACHINE), where only an administrator
// can add a root, and the TLS name checked against the host. A response that
// fails that check is never read.
//
// No redirect is ever followed: a 3xx comes back as its status and Location,
// for the caller to judge (ReleaseSelection.h IsAllowedAssetRedirect).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace urnw::updater {

struct HttpResponse {
  unsigned long status = 0;
  // The Date header in Unix seconds, 0 when it had none.
  std::int64_t serverUnixSeconds = 0;
  // The Location header, empty when it had none.
  std::string location;
};

// GETs the https `url`. The body is streamed to `sink`, at most `maxBytes` of
// it, only when the status is 200. False, with `error`, when the request
// failed, the certificate is not one the machine trusts for the host, the
// body is larger than `maxBytes`, or `sink` refused a chunk.
bool HttpGet(const std::wstring& url, const wchar_t* accept, std::uint64_t maxBytes,
             const std::function<bool(const char* data, unsigned long size)>& sink,
             HttpResponse& response, std::string& error);

// Whether WinHTTP itself reads `url` as https on port 443 to one of `hosts`,
// the parser HttpGet connects with, so the pure check of the same URL and the
// connection cannot disagree about where it goes.
bool HttpUrlHostIsOneOf(const std::wstring& url, const std::string_view* hosts, std::size_t count);

template <std::size_t Count>
bool HttpUrlHostIsOneOf(const std::wstring& url, const std::string_view (&hosts)[Count]) {
  return HttpUrlHostIsOneOf(url, hosts, Count);
}

}  // namespace urnw::updater
