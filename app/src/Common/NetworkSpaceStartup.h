// How the bundled network space comes up at launch, decided pure.
//
// The operator is bringyour.com (Ids.h kNetworkSpaceHostName). Builds before
// this one bundled the official space under "ur.network" — the key of the
// planned, now cancelled, move of the operator to *.ur.network — with
// bringyour.com as the space's migration host so the urls still resolved. A
// user's stored credentials and preferences live under the key the space was
// created with, so simply changing the constant would have signed everyone
// out. Instead every launch asks the SDK to re-key the legacy space to the
// operator host before anything binds to it (sdk network_space.go
// MigrateNetworkSpace: idempotent, a no-op once moved or when the new key
// already exists, and MUST run before UpdateNetworkSpace / SetActiveNetworkSpace
// / any Device construction).
//
// SdkHost::Initialize drives this through StartBundledSpace; the keys and the
// order touch no Windows headers, so tools/network-space-tests.cpp pins them on
// any host against the same headers the app compiles.
//
// The same goes for the values the two whole-values writers put in a space
// (BundledSpaceValuesOver, ServerSpaceValuesOver below): templates over
// urnet::NetworkSpaceValues by the SDK's field names, so the tests hand them
// stand-ins (and the SDK's own type when built against urnetwork_sdk.hpp).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>

#include "Ids.h"

namespace urnw::netspace {

struct Key {
  std::string hostName;
  std::string envName;
  friend bool operator==(const Key&, const Key&) = default;
};

// The space this build is the client for.
inline Key OfficialKey() {
  return {std::string(ids::kNetworkSpaceHostName), std::string(ids::kNetworkSpaceEnvName)};
}

// The key earlier builds bundled the official space under.
inline Key LegacyOfficialKey() {
  return {std::string(ids::kLegacyNetworkSpaceHostName),
          std::string(ids::kNetworkSpaceEnvName)};
}

// The key BuildNetworkSpace writes the bundled space with. URNETWORK_NETWORK_HOST
// points the client at a different backend; URNETWORK_NETWORK_ENV only counts
// with it (the official space is always `main`). `official` is whether the key
// is the operator's: a space that carries pinned endpoints a custom deployment
// does not have.
struct BundledSpace {
  Key key;
  bool official = false;
};

inline BundledSpace ResolveBundledSpace(const std::string& hostOverride,
                                        const std::string& envOverride) {
  BundledSpace out;
  out.key = OfficialKey();
  out.official = hostOverride.empty();
  if (!hostOverride.empty()) {
    out.key.hostName = hostOverride;
    if (!envOverride.empty()) out.key.envName = envOverride;
  }
  return out;
}

// The launch order the SDK contract requires: `migrate(from, to)` (the
// manager's migrateNetworkSpace, legacy key -> official key) runs FIRST, on
// every launch, and only then does `bindBundledSpace()` create or update the
// bundled space and derive the Api, LocalState and Device from it. A space
// handle obtained before the migration would name the closed legacy object,
// which is why nothing may be bound ahead of it.
template <class Migrate, class BindBundledSpace>
auto StartBundledSpace(Migrate&& migrate, BindBundledSpace&& bindBundledSpace) {
  migrate(LegacyOfficialKey(), OfficialKey());
  return bindBundledSpace();
}

// ---- the values written over what a space stores ----------------------------
//
// updateNetworkSpaceValues replaces a space's whole value set. SdkHost writes a
// space's values in two places, BuildNetworkSpace at every launch and
// ApplyNetworkServer when the login screen's network sheet applies a server,
// and both used to write a fresh set, carrying only the VLESS server across.
// So every launch dropped what the user had saved in the space: the extender
// dns name, gossip url and manual hosts (Account > Extenders), the root keys an
// import took, the private extender, the bootstrap DNS-over-HTTPS servers. Both
// now write the values they own over what the space stores -- the space's own
// json (SdkHost::StoredSpaceValuesLocked; nothing for a space never written),
// never the getters, which answer derived defaults -- as the Linux client's
// writers do (linux StoredNetworkSpace.hpp). Every value they do not name is
// kept as it is.

// The web app and link host (sign-in, checkout, referrals) of the official
// space: a site, not the operator's api/connect host.
inline constexpr char kOfficialLinkHostName[] = "ur.io";

// The bundled space's values over `values`, what it stores: BuildNetworkSpace's
// write, for the official key or a URNETWORK_NETWORK_HOST override alike.
//
// No migration host name, official or overridden. sdk/network_space.go's
// ServiceUrl prefers MigrationHostName over the key's HostName, so one here
// would silently redirect every api/connect url: the official key is the
// operator host (ids::kNetworkSpaceHostName), and an override must talk to the
// host it names. A stored one -- an earlier build bundled the space under
// ur.network with bringyour.com as its migration host -- is cleared, left unset
// rather than "": the wrapper's fields are std::optional and the Go side omits
// an unset one. Google (and Apple) sign-in run in the system browser against
// the provider, with the api's callback returning the token, so nothing is
// compiled in and the space always offers it.
//
// The bundled space has no url overrides, so a stored api or platform url is
// cleared too, as a write from nothing always did.
template <class Values>
Values BundledSpaceValuesOver(Values values) {
  values.bundled = true;
  values.net_expose_server_ips = true;
  values.net_expose_server_host_names = true;
  values.link_host_name = std::string(kOfficialLinkHostName);
  values.migration_host_name.reset();
  values.store = "";
  values.wallet = "circle";
  values.sso_google = true;
  values.env_secret = "";
  values.api_url.reset();
  values.platform_url.reset();
  return values;
}

// A server's values over `values`, what the space under its key stores (none
// for a host never applied before): ApplyNetworkServer's write, the bundled
// space's value set with the host-dependent parts varied (iOS
// DeviceManager.applyNetworkSpace parity). `bundled` only for the official host
// with no url overrides: a bundled space carries pinned endpoints a custom
// deployment does not have. The url overrides are what the sheet decides, so
// they replace the stored ones, an empty one deriving from the host.
template <class Values>
Values ServerSpaceValuesOver(Values values, bool official, const std::string& hostName,
                             const std::string& apiUrl, const std::string& connectUrl) {
  const bool explicitUrls = !apiUrl.empty() || !connectUrl.empty();
  values.bundled = official && !explicitUrls;
  values.net_expose_server_ips = true;
  values.net_expose_server_host_names = true;
  values.link_host_name = official ? std::string(kOfficialLinkHostName) : hostName;
  values.migration_host_name.reset();
  values.store = "";
  values.wallet = "circle";
  values.sso_google = true;
  values.env_secret = "";
  values.api_url = apiUrl;
  values.platform_url = connectUrl;
  return values;
}

}  // namespace urnw::netspace
