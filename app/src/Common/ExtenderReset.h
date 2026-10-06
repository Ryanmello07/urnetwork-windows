// The app's delivery of "Reset extenders" to the service (connect EXTENDER.md
// E7; Protocol.h reset_extenders): what the service's answer means, and the one
// re-send a busy service is owed.
//
// The app resets its own space first and then hands the reset to the service
// (SdkHost::ResetExtenders). A service whose session lock a tunnel or provider
// operation holds refuses it as busy (Reply::reset_busy), and the app sends the
// same request again -- the same space key, the same reset id -- once that
// operation has ended, which the service says by pushing a status that is not
// a transition's (Starting, Stopping): the push that ends the bring-up, the
// teardown or the provider start that held the lock. Once: the re-send's
// answer is only logged, an id the service has applied already is a no-op
// there, and what the re-send does not deliver the next import of the space
// does, since the space's values carry the id. Nothing is kept past the app:
// an app that exits first drops the re-send, for the same reason.
//
// Pure, header-only and free of Windows headers: tools/reset-extenders-tests.cpp
// runs it on any host, and SdkHost binds it. Safe for concurrent use: a press
// is answered on a background thread and the pushes arrive on the control
// pipe's reader thread.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <mutex>
#include <optional>
#include <utility>

#include "Protocol.h"

namespace urnw::extenderreset {

// What the service answered a reset_extenders (ServiceClient::ResetExtenders).
enum class ServiceAnswer {
  // Answered: the reset applied, was applied already, or the service held no
  // such space, whose next import applies it.
  Taken,
  // Refused because a tunnel or provider operation held the service's session
  // lock: owed until that operation ends.
  Busy,
  // No control channel, a transport failure, another refusal, or a service too
  // old to know the verb: the next import of the space carries the reset.
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

// Whether a status the service pushed says that the operation which held its
// session lock has ended: every state but a transition's.
constexpr bool EndsOperation(proto::TunnelState state) {
  return state != proto::TunnelState::Starting && state != proto::TunnelState::Stopping;
}

// The reset the service last refused as busy, owed until a pushed status ends
// the operation that refused it, or none.
class Owed {
 public:
  // The service answered `answer` to a press's `request`. A busy refusal is
  // owed. Any other answer leaves nothing owed, an older owed reset included:
  // the press's newer id supersedes it, and the space's values carry that id
  // to the next import.
  void Answered(const proto::ResetExtenders& request, ServiceAnswer answer) {
    std::scoped_lock lock(mutex_);
    if (answer == ServiceAnswer::Busy) {
      owed_ = request;
    } else {
      owed_.reset();
    }
  }

  // A status the service pushed. When it ends the operation the owed request
  // is due: returned once, and owed no more.
  std::optional<proto::ResetExtenders> TakeDue(proto::TunnelState pushed) {
    std::scoped_lock lock(mutex_);
    if (!EndsOperation(pushed)) return std::nullopt;
    return std::exchange(owed_, std::nullopt);
  }

  // Whether a re-send is owed.
  bool IsOwed() const {
    std::scoped_lock lock(mutex_);
    return owed_.has_value();
  }

 private:
  mutable std::mutex mutex_;
  std::optional<proto::ResetExtenders> owed_;
};

}  // namespace urnw::extenderreset
