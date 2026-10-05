// SPDX-License-Identifier: MPL-2.0
// Built without the precompiled header (App.vcxproj): plain Win32, no WinRT.
#include "SystemProxy.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include "DiagnosticLines.h"
#include "Strings.h"

#pragma comment(lib, "winhttp.lib")

namespace urnw {

std::string ReadUserProxyKind() {
  WINHTTP_CURRENT_USER_IE_PROXY_CONFIG config{};
  if (!::WinHttpGetIEProxyConfigForCurrentUser(&config)) return std::string(diag::kProxyUnknown);
  // Only the kind leaves this function: the PAC URL, the proxy list and the
  // bypass list are read for it and freed (they are the caller's, GlobalFree).
  const std::string proxyList = config.lpszProxy ? Narrow(config.lpszProxy) : std::string();
  const bool pacScript = config.lpszAutoConfigUrl && config.lpszAutoConfigUrl[0] != L'\0';
  const std::string kind = diag::UserProxyKind(config.fAutoDetect != FALSE, pacScript, proxyList);
  if (config.lpszAutoConfigUrl) ::GlobalFree(config.lpszAutoConfigUrl);
  if (config.lpszProxy) ::GlobalFree(config.lpszProxy);
  if (config.lpszProxyBypass) ::GlobalFree(config.lpszProxyBypass);
  return kind;
}

}  // namespace urnw
