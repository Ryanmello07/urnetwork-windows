// Build-time app configuration. Identity constants (service name, tray GUID,
// uri scheme) live in Common/Ids.h and must stay stable across releases; this is
// for values that vary by build or deployment.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace urnw::config {

// WalletConnect Cloud project id — one project id shared by every URnetwork
// client (see apple/NEXTSTEPS2.md). No flow passes it today: the Bittensor
// proof is the SDK session helper's bridge url (Talisman's extension only, no
// WalletConnect pairing; UPGRADE.md 4.6), and the Solana bridge drives the
// Phantom/Solflare extensions. Kept so a build that injects it still compiles.
//
// Inject it on the build (CI / build machine, the way android takes it from
// local.properties) rather than committing it:
//   msbuild ... /p:UrnWalletConnectProjectId=<project id>
//
// App.vcxproj passes the id as a BARE token and it is stringized here: an
// MSBuild PreprocessorDefinition cannot carry `\"`-escaped quotes through to cl
// (verified — the value ends at the backslash and the TU fails to compile), so
// the quoting has to happen in the preprocessor instead.
#define URN_CONFIG_STR2(x) #x
#define URN_CONFIG_STR(x) URN_CONFIG_STR2(x)

#if defined(URN_WALLETCONNECT_PROJECT_ID_RAW)
inline constexpr const char* kWalletConnectProjectId =
    URN_CONFIG_STR(URN_WALLETCONNECT_PROJECT_ID_RAW);
#else
inline constexpr const char* kWalletConnectProjectId = "";
#endif

// The GitHub repo the update checker polls for releases (beta-distribution
// spec §5). Official urnetwork repos only, never a personal fork — and the
// STABLE feed, not the nightly one: urnetwork/build holds the nightly builds
// the release pipeline (build/all/run.sh) mints; stable releases are published
// by hand to each app's own repo, so Windows polls the urnetwork/windows
// releases, where a stable release v<version> carries the per-arch MSIs
// (Common/ReleaseSelection.h matches its tag and asset names). Wide because it
// is spliced into WinHTTP request strings, which are UTF-16 end to end.
inline constexpr const wchar_t* kUpdateRepo = L"urnetwork/windows";

}  // namespace urnw::config
