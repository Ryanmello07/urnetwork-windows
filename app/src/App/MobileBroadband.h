// The Windows half of the network country (Common/NetworkCountry.h decides,
// open bug P052): the default route out of the forward table, and when a
// mobile broadband adapter carries it, that adapter's registration through the
// Mobile Broadband API (MbnApi). Plus the OS notifications that say the default
// route may have moved, for NetworkCountryWatch.
//
// Read in the app, which hands the country to the service over the control
// pipe (Protocol.h SetNetworkCountry): the app's own sdk needs it before its
// network spaces are built (the sign-in dials too), the app already holds COM,
// and the service stays free of it (TunnelController's GuidText note).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>

#include "NetworkCountry.h"

namespace urnw {

// The network country now, never "" without a source that says why. Blocks
// for a COM call only when a mobile broadband adapter carries the default
// route; on every other PC it is two IP helper reads.
netcountry::Reading ReadNetworkCountry();

// NotifyRouteChange2 and NotifyIpInterfaceChange, both families, for as long as
// this lives. `sink` runs on system threads: for a change to a default route
// (the tun's capture routes come and go in bursts of 31 at every bring-up and
// teardown, and say nothing about the network) and for every interface change.
// It must only record (NetworkCountryWatch::NetworkEventSink). A registration
// that fails is logged and leaves the other one working.
class DefaultRouteChanges {
 public:
  explicit DefaultRouteChanges(std::function<void()> sink);
  // Unregisters, which waits out a callback already running.
  ~DefaultRouteChanges();

  DefaultRouteChanges(const DefaultRouteChanges&) = delete;
  DefaultRouteChanges& operator=(const DefaultRouteChanges&) = delete;

  // Called by the notification callbacks.
  void Notify() const { sink_(); }

 private:
  std::function<void()> sink_;
  void* routeHandle_ = nullptr;      // HANDLE
  void* interfaceHandle_ = nullptr;  // HANDLE
};

}  // namespace urnw
