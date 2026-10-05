// Writes the service's diagnostic lines into its sdk log, which is what "send
// feedback with logs" uploads (Common/DiagnosticLines.h has the lines, the
// path, and the rule that every value in them is a token or a small number).
// It is the only writer of that log in this repo.
//
// The moments: NoteStatus runs at every state the service pushes to the app
// (ControlServer::PushState): a request that moved the tunnel, the provider
// or the kill switch, and every transition the service makes on its own (the
// capture activation, the dead-tunnel failsafe). NoteStart runs when a
// start_tunnel arrives, before it is served, and NoteNetworkCountry when the
// app's set_network_country has been applied. A line is written only when it
// differs from the last one written under its tag, so a push that changes
// nothing a line says writes nothing, and the volume follows the user's
// actions, not a clock. The DNS settings and the network country are written
// once for each session that comes up, and again when they change in it.
//
// Where it may run: it calls into the sdk (urnet::logAppInfo is a cgo call),
// so only at the RPC boundary, where ControlServer::PushState already flushes
// glog: never on the teardown path ahead of the route revert, never in an
// EgressMonitor callback, never under the session lock. What it reads is the
// lock-free status (TunnelController::Status), the interface table and the
// registry, none of which can wedge on a session.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "NetworkCountry.h"
#include "Protocol.h"

namespace urnw {

// One per service, owned by ControlServer; its rules are the header's. Safe
// from any thread: one lock serializes every line.
class ServiceDiagnostics {
 public:
  // [app][service] and [app][adapter] for every status, and [app][dns] and
  // [app][network-country] while a session is live, each when it changed.
  // `killSwitch` is the preference the app last sent
  // (TunnelController::KillSwitchPreference); `networkCountry` the country in
  // force (TunnelController::NetworkCountry), none before the app sent one.
  void NoteStatus(const proto::TunnelStatus& status, bool killSwitch,
                  const std::optional<netcountry::Reading>& networkCountry);
  // [app][proxy]: the user's system proxy as the app read it for this start.
  void NoteStart(const proto::StartTunnel& request);
  // [app][network-country] when it changed, live session or not.
  void NoteNetworkCountry(const std::optional<netcountry::Reading>& networkCountry);

 private:
  // The line under `tag` unless it equals `last`, which then becomes it. Caller
  // holds mutex_.
  void WriteIfChangedLocked(std::string_view tag, std::string line, std::string& last);
  // [app][network-country] when it changed; nothing before the app sent a
  // country. Caller holds mutex_.
  void WriteNetworkCountryLocked(const std::optional<netcountry::Reading>& networkCountry);

  std::mutex mutex_;
  std::string lastService_;
  std::string lastAdapter_;
  std::string lastDns_;
  std::string lastNetworkCountry_;
};

}  // namespace urnw
