// What signing out of URnetwork asks of the service, in what order, and what
// keeps the request standing until the service has done it (owner decision,
// 2026-10-05: "Windows sign-out: stop the tunnel and provider, the same as
// Quit").
//
// The requests. Quit's two, in Quit's order (AppLifetime.h, SdkHost::Quit),
// then the service's logout:
//   stop_tunnel    ends any session, tunnel or rpc-only, and lifts whatever
//                  firewall policy is in force, the kill switch's armed floor
//                  included, exactly as Quit and Disconnect lift it
//                  (TunnelController::StopLocked with finalDisarm);
//   stop_provider  retires the provider-only device;
//   logout         severs this machine's device identity and clears what the
//                  service's sdk stored for the signed-out account, its client
//                  credential among it (TunnelController::Logout).
// All three go out even when one fails: each is idempotent and does a part the
// others do not.
//
// The obligation. A service that was not told still runs what it ran, or still
// holds the old account's identity and credential. So a sign-out is recorded as
// owed before the first request goes out, in a marker that outlives the app,
// and it is cleared only once all three requests succeeded. While it is owed:
//   * the sign-out has still completed in the app: its credentials, its saved
//     rpc session and its device are gone (SdkHost::Logout);
//   * every pass of the session worker delivers it before anything else, and
//     a start waits for it: neither start_tunnel nor start_provider goes out
//     while it is owed, so the next account never runs on the old identity, nor
//     beside the old account's tunnel;
//   * the service watchdog keeps retrying, signed in or not, so a service that
//     comes back is told at once, and the next launch's first pass tells it.
// A restarted service, or a machine that rebooted, starts nothing by itself (it
// starts only on a request), so the old account's work can come back only
// through a start from this app, and every start waits for the obligation.
//
// Pure, header-only and free of Windows headers: tools/sign-out-tests.cpp runs
// it against a fake service and marker on any host, and SdkHost binds the real
// ones. Not safe for concurrent use: SdkHost calls it under mutex_, and only
// Owed may be read from another thread.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <utility>

namespace urnw::signout {

enum class Request { StopTunnel, StopProvider, Logout };

// The order every delivery sends them in: the machine first, then the
// provider, as Quit sends them, and the identity last.
inline constexpr std::array<Request, 3> kRequests{Request::StopTunnel, Request::StopProvider,
                                                  Request::Logout};

// The wire names (Protocol.h msg), for logs.
constexpr const char* ToString(Request request) {
  switch (request) {
    case Request::StopTunnel: return "stop_tunnel";
    case Request::StopProvider: return "stop_provider";
    case Request::Logout: return "logout";
  }
  return "unknown";
}

enum class Delivery {
  // Nothing is owed: the three requests succeeded, now or before.
  Delivered,
  // The control channel could not be reached, so nothing was sent.
  Unreachable,
  // The service was reached and at least one request failed.
  Refused,
};

// For logs.
constexpr const char* ToString(Delivery delivery) {
  switch (delivery) {
    case Delivery::Delivered: return "delivered";
    case Delivery::Unreachable: return "the service could not be reached";
    case Delivery::Refused: return "the service did not do every request";
  }
  return "unknown";
}

// The service as a delivery sees it.
struct Service {
  // Dial the control channel when it is down; true when it is up afterwards.
  std::function<bool()> reach;
  // Send one request; true when the service answered that it did it.
  std::function<bool(Request)> send;
};

// Where an owed sign-out outlives the app: a relaunch, a reboot. Production
// keeps a file beside the app's preferences (Paths.h SignOutOwedFile).
struct Marker {
  std::function<bool()> read;
  std::function<void(bool owed)> write;
};

// One owed sign-out, or none. Begin records it, Settle delivers it.
class Obligation {
 public:
  explicit Obligation(Marker marker) : marker_(std::move(marker)) {}

  // Read what an earlier run left. Once, before the first Settle or Owed.
  void Load() { owed_.store(marker_.read()); }

  // The user signed out: owed from here on, written down before anything is
  // sent, then delivered when the service can be told now.
  Delivery Begin(const Service& service) {
    owed_.store(true);
    marker_.write(true);
    return Settle(service);
  }

  // Deliver an owed sign-out. Delivered at once when nothing is owed, and the
  // marker is cleared only after all three requests succeeded.
  Delivery Settle(const Service& service) {
    if (!owed_.load()) return Delivery::Delivered;
    if (!service.reach()) return Delivery::Unreachable;
    bool done = true;
    for (Request request : kRequests) {
      if (!service.send(request)) done = false;
    }
    if (!done) return Delivery::Refused;
    owed_.store(false);
    marker_.write(false);
    return Delivery::Delivered;
  }

  // A start may go out only while this is false. Safe from any thread.
  bool Owed() const { return owed_.load(); }

 private:
  Marker marker_;
  std::atomic<bool> owed_{false};
};

}  // namespace urnw::signout
