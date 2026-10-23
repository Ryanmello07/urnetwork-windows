// The connected-provider rows behind the provider-locations view, plus the pure
// label/ordering logic the globe and the list share — a port of the android
// ProviderLocationRow / ProviderLocationsViewModel data layer
// (ui/connect/providerlocations/, see sdk/PROVIDERLOCATIONS.md).
//
// The SDK returns ConnectedProviderLocation sorted oldest-connected first;
// SdkHost::CurrentProviderLocations maps it into these rows, dedupes by value
// (the change listener is signal-only and the SDK re-emits on every window
// event, so an identity compare would thrash the UI), and pushes them here.
//
// Pure standard C++ — no WinRT, no localization — so the android JVM tests port
// straight across to tools/globe-tests.cpp. Anything user-facing that needs the
// string store (the "unknown" placeholder, the duration text) is formatted in
// ProviderLocationsSheet.cpp from the primitives below. App.vcxproj compiles
// this with PrecompiledHeader=NotUsing.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ExtenderRingGeometry.h"

namespace urnw {

// One connected provider, as rendered by the globe and the list.
struct ProviderLocationRow {
  // the EGRESS provider client id (Destination tail) -- the id to display and
  // copy, not the window-local ephemeral id
  std::string clientId;
  std::string country;
  std::string countryCode;  // lowercase; feeds getColorHex
  std::string region;
  std::string city;
  bool hasLocation = false;
  // the coordinates to plot: the city centroid when known, else the region
  // centroid. hasCoordinates is false when the provider has neither
  bool hasCoordinates = false;
  double lat = 0;
  double lon = 0;
  int64_t connectedSinceMillis = 0;
  // the address families the platform proved for this provider (IPV6.md D1):
  // the SDK's category ("dualstack" / "v4-only" / "v6-only") and its row label
  // token ("both" / "v4" / "v6"); empty from an SDK that predates the field,
  // which the view renders as v4 (what such a provider carries)
  std::string ipFamily;
  std::string ipFamilyLabel;
  // the extenders carrying this client's connection to the provider right now
  // (connect/EXTENDER.md K1, K9), in the SDK's order, which is the order of the
  // dot's rings from the outside in: none over a direct or p2p route, one
  // through an extender, briefly two across a transport migration
  std::vector<ExtenderMark> extenders;

  bool Plottable() const { return hasCoordinates; }

  // The extenders are part of the value: a transport migration changes nothing
  // else, and SdkHost publishes only a row set that differs.
  bool operator==(const ProviderLocationRow& other) const {
    return clientId == other.clientId && country == other.country &&
           countryCode == other.countryCode && region == other.region && city == other.city &&
           hasLocation == other.hasLocation && hasCoordinates == other.hasCoordinates &&
           lat == other.lat && lon == other.lon &&
           connectedSinceMillis == other.connectedSinceMillis &&
           ipFamily == other.ipFamily && ipFamilyLabel == other.ipFamilyLabel &&
           extenders == other.extenders;
  }
  bool operator!=(const ProviderLocationRow& other) const { return !(*this == other); }
};

// ConnectedProviderLocation's ExtenderIps and ExtenderColorHexes, comma
// separated in the same order, paired back up by index (K1, K3). The SDK keeps
// the two lists the same length; if they ever differ, the unpaired tail is
// dropped rather than drawn in a color the SDK did not give it. (The connect
// grid's PairExtenderMarks keeps an address with no color and draws it muted.)
std::vector<ExtenderMark> ProviderExtenders(std::string_view ips, std::string_view colorHexes);

// ---- the dots' extender rings (K2, K9) ---------------------------------------
// A provider reached through extenders wears their rings on its dot, in the
// list and on the globe, each at that dot's own size. The rings grow inward
// from the dot's edge, so a ringed dot keeps its footprint and the selection
// ring outside it stays where it is.

// The row dot's diameter: the location chooser's (MakeDot(..., 12)), so a
// country reads the same size everywhere.
inline constexpr double kProviderRowDotDiameter = 12;
// The globe dot's radius in the globe's 600-unit space (the web and android
// globes' 7); the globe scales it with everything else it draws.
inline constexpr float kProviderGlobeDotRadius = 7.0f;

// A row's dot and its rings at K2's numbers, as on every app's rows (K9): the
// 12px dot holds one ring, and a second extender dashes it
// (ExtenderRingFit::Drop) rather than thinning the rings to keep the count.
ExtenderRingLayout ProviderRowDotRings(const ProviderLocationRow& row);

// A globe dot and its rings in canvas units: laid out at the dot's size in the
// 600-unit space, which holds one ring at K2's numbers, so a second extender
// dashes it (ExtenderRingFit::Drop), then scaled by the globe's `unit`.
ExtenderRingLayout ProviderGlobeDotRings(const ProviderLocationRow& row, double unit);

// "City, Region, Country" -- omitting whichever parts the server does not know.
// Empty when nothing is known; the view substitutes the localized
// provider_location_unknown placeholder.
std::string PlaceLabel(const ProviderLocationRow& row);

// "37.7749, -122.4194" at 4 decimal places, or an em dash when the provider has
// no coordinates.
std::string CoordinatesLabel(const ProviderLocationRow& row);

// The connected duration split for display. `valid` is false when the SDK has
// no connected-since stamp (fixed/peer destinations from an older device), in
// which case the view shows nothing rather than "0s".
struct ConnectedDuration {
  bool valid = false;
  int64_t hours = 0;
  int64_t minutes = 0;
  int64_t seconds = 0;
};
ConnectedDuration SplitConnectedDuration(int64_t connectedSinceMillis, int64_t nowMillis);

// ---- "Stay on this exit" ----------------------------------------------------
// Reconnect to one provider of the current connection, by its client id, so new
// connections keep that provider's IP address. The SDK dials a client id
// location directly (connect's fixed destination: nothing is discovered and
// nothing replaces it), and the location is not marked as a network peer, so
// the provider keeps carrying the traffic as the public exit it already is. The
// rows are the user's own current exits, so this pins one of them; it is not a
// way to browse or pick from all providers.

// What a provider row shows for "Stay on this exit".
enum class StayOnExitState {
  None,
  Offer,    // the selected row offers the action
  Staying,  // the connection already stays on this provider
};

// The selected row offers to stay on its provider; the provider the connection
// already stays on says so instead, selected or not. `stayingClientId` is the
// client id of the current location when it is a client id location (a stayed
// exit or a network peer), else empty. Ids compare ignoring ASCII case.
StayOnExitState StayOnExitStateFor(const ProviderLocationRow& row,
                                   const std::string& selectedClientId,
                                   const std::string& stayingClientId);

// "018f…5c6d": the first and last four characters of a client id (the form the
// android connect drawer shows a client id location in). A short id is
// returned as it is.
std::string ShortClientId(const std::string& clientId);

// "018f…5c6d · Berlin, Germany": the short client id, which is what makes the
// location one provider, then the city (or the region) and the country. The id
// comes first so a narrow drawer trims the place rather than the id. Just the
// short id when the server does not know where the provider is.
std::string StayOnExitName(const ProviderLocationRow& row);

// What "Stay on this exit" connects to, field for field what the sheet copies
// into the SDK's ConnectLocation (this header stays SDK-free): the provider's
// client id as the location id, the name above, and the place. The sheet sets
// network_peer false.
struct StayOnExitTarget {
  std::string clientId;
  std::string name;
  std::string city;
  std::string region;
  std::string country;
  std::string countryCode;

  bool operator==(const StayOnExitTarget& other) const {
    return clientId == other.clientId && name == other.name && city == other.city &&
           region == other.region && country == other.country &&
           countryCode == other.countryCode;
  }
};
// nullopt when the row has no client id
std::optional<StayOnExitTarget> MakeStayOnExitTarget(const ProviderLocationRow& row);

// The globe's wheel order (the plottable providers west to east about their
// centroid) and its clamped stepping are NOT here: they live in the SDK's
// shared ProviderLocationsViewController, which every URnetwork app binds --
// see SdkHost::StepProviderSelection.

}  // namespace urnw
