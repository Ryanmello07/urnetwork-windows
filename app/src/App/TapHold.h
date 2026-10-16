// A connect or disconnect tap holds its state until the session pass that ran
// it has published (Apple parity, apple 8a8d0658 and db907e66). A tap shows the
// sdk controller's own first status at once, connecting for a connect and
// disconnected for a disconnect, but the session worker runs it later, behind
// a pass in flight that can wait out a 60 s rpc. Every stats push read before
// the pass ran it carries the older status, and a push applied as it came
// flipped the view back: a connect read as "Connect" again, and a disconnect
// showed nothing until a push came.
//
// SdkHost numbers each user gesture it records in the session-request slot
// (a connect of any kind, a Disconnect; never an ensure or a provider
// reconcile) and tells the views its number at once. A gesture is settled
// once it is neither waiting in the slot nor in the pass that runs it, whether
// it ran or went: replaced by a newer tap (last request wins), cancelled (a
// re-clicked row), or dropped by a sign-out or a quit. Each stats snapshot
// carries the newest number through which every gesture had settled when its
// read began, so a snapshot whose read began after the held gesture settled
// says what that gesture left, and it ends the hold; an older one cannot.
//
// Pure, no Windows dependencies; tools/tap-hold-tests.cpp is the spec. Not
// safe for concurrent use: SdkHost reads SettledThrough under the slot's lock,
// and the views use Hold on the UI thread.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>

namespace urnw::taphold {

// What a tap shows while it holds.
enum class Kind { Connect, Disconnect };

// The tap a view holds: its gesture's number and what it shows.
struct Hold {
  std::uint64_t gesture = 0;
  Kind kind = Kind::Connect;
};

// The newest gesture number through which every gesture has settled: the last
// number handed out when nothing is outstanding, else just below the oldest
// one still waiting in the slot or running in a pass. 0 means none.
inline constexpr std::uint64_t SettledThrough(std::uint64_t issued, std::uint64_t pending,
                                              std::uint64_t inFlight) {
  std::uint64_t oldest = 0;
  if (pending != 0) oldest = pending;
  if (inFlight != 0 && (oldest == 0 || inFlight < oldest)) oldest = inFlight;
  return oldest == 0 ? issued : oldest - 1;
}

// Whether a snapshot whose read began with `settled` ends the hold.
inline constexpr bool Releases(const Hold& hold, std::uint64_t settled) {
  return hold.gesture <= settled;
}

// The hold a view keeps after a snapshot arrives: ended by one read after its
// gesture settled, kept otherwise.
inline constexpr std::optional<Hold> AfterSnapshot(std::optional<Hold> hold,
                                                   std::uint64_t settled) {
  if (hold && Releases(*hold, settled)) return std::nullopt;
  return hold;
}

// The hold after a tap the slot recorded: the newer gesture wins, so a late
// notice of an older one never replaces it.
inline constexpr std::optional<Hold> AfterTap(std::optional<Hold> hold, Hold tap) {
  if (hold && tap.gesture < hold->gesture) return hold;
  return tap;
}

// What a view shows: a start waiting on its balance shows connecting
// (StartConnectWait.h), a held tap its own state, else the reported status.
template <class Status>
inline constexpr Status Shown(bool startWaiting, const std::optional<Hold>& hold,
                              Status reported, Status connecting, Status disconnected) {
  if (startWaiting) return connecting;
  if (hold) return hold->kind == Kind::Connect ? connecting : disconnected;
  return reported;
}

}  // namespace urnw::taphold
