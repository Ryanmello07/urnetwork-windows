// The app's delivery of the network space's ingress dns -- the connection
// region and the bootstrap DNS-over-HTTPS servers (sdk control_doh.go) -- to
// the service (Protocol.h set_ingress_dns): what the service's answer means,
// and the re-send a busy service is owed.
//
// The service runs the session's device and the provider-only device in its
// own copy of the network space, imported from the app's at start_tunnel and
// start_provider, and keeps the values it imported. A region or servers the
// app saved while a session ran, or before it reattached to one, therefore
// reached that session's device only at the next start: a user stuck
// connecting who chose "China" from the "Can't reach URnetwork" prompt stayed
// stuck, because the prompt's own session still resolved through the blocked
// resolvers. So after every save, and after it reattaches to a session, the
// app hands the service the values its space holds (SdkHost::
// QueueIngressDnsLocked), and the service writes them into the space it holds
// under the same key, where they apply in place (TunnelController::
// SetIngressDns), as they do for an in-process tunnel.
//
// A service whose session lock a tunnel or provider operation holds refuses
// the write as busy (Reply::ingress_dns_busy). The app owes it the values
// again once that operation has ended, which the service says by pushing a
// status that is not a transition's (extenderreset::EndsOperation, the same
// rule as an owed extender reset), and a busy answer to that send owes them
// again. A newer save replaces what is owed, and any other answer owes
// nothing: a service that did not take the values, or holds no such space,
// gets them at the next import of the space, whose values carry them. Nothing
// is kept past the app, for the same reason.
//
// Pure, header-only and free of Windows headers: tools/ingress-dns-tests.cpp
// runs it on any host, and SdkHost binds it. Safe for concurrent use: a send
// is answered on the app's provider-only worker and the pushes arrive on the
// control pipe's reader thread.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <mutex>
#include <optional>
#include <utility>

#include "ExtenderReset.h"
#include "Protocol.h"

namespace urnw::ingressdns {

// What the service answered a set_ingress_dns (ServiceClient::SetIngressDns).
enum class ServiceAnswer {
  // Answered: the values were written into the space the service holds under
  // the key, or the service held no such space, whose next import carries
  // them.
  Taken,
  // Refused because a tunnel or provider operation held the service's session
  // lock: owed until that operation ends.
  Busy,
  // No control channel, a transport failure, another refusal, or a service too
  // old to know the verb: the next import of the space carries the values.
  NotTaken,
};

// For logs.
constexpr const char* ToString(ServiceAnswer answer) {
  switch (answer) {
    case ServiceAnswer::Taken: return "taken";
    case ServiceAnswer::Busy: return "busy";
    case ServiceAnswer::NotTaken: return "not taken";
  }
  return "unknown";
}

// The values the service last refused as busy, owed until a pushed status ends
// the operation that refused them, or none.
class Owed {
 public:
  // The service answered `answer` to `request`. A busy refusal is owed. Any
  // other answer leaves nothing owed, older values included: these are newer,
  // and the space's values carry them to the next import.
  void Answered(const proto::SetIngressDns& request, ServiceAnswer answer) {
    std::scoped_lock lock(mutex_);
    if (answer == ServiceAnswer::Busy) {
      owed_ = request;
    } else {
      owed_.reset();
    }
  }

  // A status the service pushed. When it ends the operation the owed values
  // are due: returned once, and owed no more until a send of them is refused
  // as busy again.
  std::optional<proto::SetIngressDns> TakeDue(proto::TunnelState pushed) {
    std::scoped_lock lock(mutex_);
    if (!extenderreset::EndsOperation(pushed)) return std::nullopt;
    return std::exchange(owed_, std::nullopt);
  }

  // Whether a send is owed.
  bool IsOwed() const {
    std::scoped_lock lock(mutex_);
    return owed_.has_value();
  }

 private:
  mutable std::mutex mutex_;
  std::optional<proto::SetIngressDns> owed_;
};

}  // namespace urnw::ingressdns
