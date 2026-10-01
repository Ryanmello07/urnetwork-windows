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

}  // namespace urnw::netspace
