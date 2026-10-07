// The user's system proxy, as a kind (Common/DiagnosticLines.h UserProxyKind),
// for the [app][proxy] line the service writes to the log feedback uploads when
// it starts a tunnel (Protocol.h StartTunnel::system_proxy). Read here because
// the setting is per user (WinINet, what browsers use) and the service runs as
// LocalSystem. A browser behind a proxy app or a PAC script can leave by a
// path the tunnel does not decide, which is the question this answers for
// support, without the proxy's host, port or URL.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>

namespace urnw {

// "none", "auto-detect", "pac", the manual proxies by where their hosts are,
// or "unknown" when the setting cannot be read.
std::string ReadUserProxyKind();

}  // namespace urnw
