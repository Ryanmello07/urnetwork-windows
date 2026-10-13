// Build-time app configuration. Identity constants (service name, tray GUID,
// uri scheme) live in Common/Ids.h and must stay stable across releases; this is
// for values that vary by build or deployment.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <string_view>

namespace urnw::config {

// WalletConnect Cloud project id — one project id shared by every URnetwork
// client (see apple/NEXTSTEPS2.md). The Bittensor WalletConnect wallet passes
// it to the ur.io bridge page (SdkHost::BeginBittensorProof ->
// BittensorWalletSession::setWalletConnectProjectId -> wc_project_id), which
// pairs with it; empty = the page's own project id. The Solana bridge drives
// the Phantom/Solflare extensions and does not use it.
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

// The update feed is not configured here: Common/ReleaseSelection.h holds the
// feed table, where the tray app and the update helper both read it, each
// feed addressed by its GitHub repository id.

// ---- "Continue in browser": a sign-in approved on a web page -------------------
//
// The action in the "Sign in with browser" sheet is OFF in this build. It is
// offered only when this is true AND the active network is one a request can name
// (SdkHost::CurrentApprovalNetwork: the official network, or another whose API url
// is https://host[:port]), the approval page answers its descriptor with the contract
// version that network needs (below, ApprovalCheck.h), and the process is not elevated.
// Turn it on when the page is live; until then the sheet behaves exactly as it always
// has. A test pins it false.
inline constexpr bool kBrowserApprovalEnabled = false;

// The origin of the approval page, a build constant: the app never takes a page from
// a server, a file, a network space or a request. For a local end-to-end run against
// a page served at another address, build with URN_APPROVAL_TEST_ORIGIN defined on
// the compiler's command line as a string literal, for example
//   cl ... /DURN_APPROVAL_TEST_ORIGIN=\"http://localhost:5173\"
// Through MSBuild, which is how this repository builds, and where a \" in a define passed
// as a property is a known trap (Version.h), put the same text in the CL environment
// variable, which cl.exe reads itself:
//   set CL=/DURN_APPROVAL_TEST_ORIGIN=\"http://localhost:5173\"
//   msbuild ...
// (checked through msbuild: the macro reaches the compiler as the string literal, and a
// build without it keeps the real page.) Either way it replaces the scheme, host and port
// of BOTH urls below (the descriptor check still applies to it). No project file,
// property sheet or script of this repository defines it, and a test says so: a shipped
// build always talks to the real page. An environment variable stays set in the shell it
// was set in, so a build with the override says so in the app's log at start, the host only
// (kApprovalOriginIsATest, AppController::Start).
#if defined(URN_APPROVAL_TEST_ORIGIN)
#define URN_APPROVAL_ORIGIN URN_APPROVAL_TEST_ORIGIN
inline constexpr bool kApprovalOriginIsATest = true;
#else
#define URN_APPROVAL_ORIGIN "https://manager.bringyour.com"
inline constexpr bool kApprovalOriginIsATest = false;
#endif

// The page that shows the person which network is signing in and asks them to
// approve (contract v1: the request is this url plus a fragment).
inline constexpr char kApprovalPageUrl[] = URN_APPROVAL_ORIGIN "/login-app";
// What the page says about itself, fetched with one GET before the action is offered:
// {"contracts":[1,2],"platforms":["windows"]} as application/json. The official network
// needs the page to list 1, another network needs 2 (ApprovalNetwork.h).
inline constexpr char kApprovalDescriptorUrl[] = URN_APPROVAL_ORIGIN "/approval-contract.json";

#undef URN_APPROVAL_ORIGIN

// The host (and port) of a url, for the plain text beside the button that names where
// the browser is going.
constexpr std::string_view HostOfUrl(std::string_view url) {
  const std::size_t scheme = url.find("://");
  if (scheme == std::string_view::npos) return {};
  url.remove_prefix(scheme + 3);
  return url.substr(0, url.find_first_of("/?#"));
}

inline constexpr std::string_view kApprovalHost = HostOfUrl(kApprovalPageUrl);

static_assert(HostOfUrl("https://manager.bringyour.com/login-app") == "manager.bringyour.com");
static_assert(HostOfUrl("http://localhost:5173") == "localhost:5173");
static_assert(HostOfUrl("no scheme").empty());
#if !defined(URN_APPROVAL_TEST_ORIGIN)
static_assert(!kApprovalOriginIsATest);
static_assert(std::string_view(kApprovalPageUrl) == "https://manager.bringyour.com/login-app");
static_assert(std::string_view(kApprovalDescriptorUrl) == "https://manager.bringyour.com/approval-contract.json");
static_assert(kApprovalHost == "manager.bringyour.com");
#endif

}  // namespace urnw::config
