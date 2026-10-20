// Session-local evidence and the capture transaction. SDK callbacks only
// invalidate proof; a completed, generation-consistent sample grants it.
// No OS or SDK dependencies: the same transaction drives the service and tests.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <chrono>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

namespace urnw {

struct CaptureWindow {
  int64_t generation = 0;
  int64_t added = 0;
};

struct CaptureTicket {
  uint64_t revision = 0;
  int64_t generation = 0;
  int64_t sampledMillis = 0;
};

struct CaptureSample {
  uint64_t revision = 0;
  int64_t proofSinceMillis = -1;
  int64_t startedMillis = 0;
};

struct CaptureNetworkEpoch {
  uint64_t revision = 0;
  int64_t millis = -1;
};

// The capture transaction's timing, in milliseconds. The controller builds each
// session's readiness from the watchdog's settings (TunnelWatchdogSettings::
// capture, TunnelWatchdogSettings.h), so the readiness, the sampler that feeds
// it and the evaluator that watches it read the same values.
struct CaptureReadinessSettings {
  // The resolution of the sdk's probe age (ProbeAgeSeconds: whole seconds,
  // truncated). A proof qualifies after a network change only when its whole
  // possible interval follows the change, which needs at least this much
  // between the change and the sample; the sampler waits this long after a
  // change before its re-probe pass for the same reason, so what that pass
  // proves can qualify. It is the age's resolution, not a preference, and moves
  // only with it. Zero or less: ages are exact, so a proof qualifies once it
  // postdates the change, and the re-probe runs at once.
  int64_t probeAgeResolutionMillis = 1000;
  // How long a completed sample's proof may authorize a capture. A ticket cannot
  // survive a suspended evaluator or a long blocked activation merely because no
  // callback ran to invalidate it, and regular samples issue fresh tickets
  // rather than renewing old ones: four of the default 2 s samples. Zero: only
  // a sample from the same millisecond is fresh; negative: none is, and no
  // capture ever activates.
  int64_t freshnessMillis = 8000;
  // How long a pending capture may go without a completed sample before the
  // evaluator logs the stall. A diagnostic only, at the sdk's unresponsive
  // window by default. Zero or less: every evaluator tick reports a stall.
  int64_t sampleStallMillis = 30000;
};

// ProbeAgeSeconds is truncated. Its entire possible timestamp interval must
// follow the network event; a historical qualification is not a new uplink proof.
inline bool CaptureProofAfterNetworkChange(int64_t ageSeconds,
                                           int64_t sampleMillis,
                                           int64_t changedMillis,
                                           const CaptureReadinessSettings& settings) {
  if (changedMillis < 0) return true;
  const int64_t resolution =
      settings.probeAgeResolutionMillis > 0 ? settings.probeAgeResolutionMillis : 0;
  const int64_t elapsed = sampleMillis - changedMillis;
  // the age's unit is the second: 1000 ms
  return ageSeconds >= 0 && elapsed >= resolution &&
         ageSeconds <= (elapsed - resolution) / 1000;
}

struct CaptureExit {
  bool proven = false;
  bool done = false;
  bool quarantined = false;
  bool warning = false;
  int64_t probeAgeSeconds = -1;
};

// Provider presence, historical qualification, and a usable current exit are
// distinct. Fold the SDK snapshot here so every exclusion is regression-tested.
inline bool CaptureExitIsUsable(CaptureExit exit, int64_t sampleMillis,
                                int64_t changedMillis,
                                const CaptureReadinessSettings& settings) {
  return exit.proven && !exit.done && !exit.quarantined && !exit.warning &&
         exit.probeAgeSeconds >= 0 &&
         CaptureProofAfterNetworkChange(exit.probeAgeSeconds, sampleMillis, changedMillis,
                                        settings);
}

// Emit only these fixed diagnostic labels; SDK reason strings are untrusted
// network data and must not become endpoint/identity disclosures in service logs.
inline const char* CaptureWaitReason(CaptureWindow window, int64_t usableProven,
                                     std::string_view stallReason) {
  if (window.generation == 0) return "waiting-selection";
  if (usableProven > 0 && window.added > 0) return "qualified";
  if (stallReason == "platform-unreachable") return "platform-unreachable";
  if (stallReason == "providers-unresponsive") return "providers-unresponsive";
  if (stallReason == "auth-failing") return "auth-failing";
  if (stallReason == "rate-limited") return "rate-limited";
  return window.added > 0 ? "qualification-pending" : "discovery-pending";
}

inline bool CaptureSampleStalled(int64_t startedMillis, int64_t sampledMillis,
                                 int64_t nowMillis, const CaptureReadinessSettings& settings) {
  const int64_t last = sampledMillis >= startedMillis ? sampledMillis : startedMillis;
  return nowMillis - last >= settings.sampleStallMillis;
}

// All methods are thread-safe. A fresh object is required for every session;
// cancellation is permanent, including when an old SDK callback arrives late.
class CaptureReadiness {
 public:
  // The service's: the steady clock, and the watchdog's capture settings.
  explicit CaptureReadiness(CaptureReadinessSettings settings = {})
      : CaptureReadiness(SteadyMillis, settings) {}
  // The clock is injected by deterministic tests. A ticket cannot survive a
  // suspended evaluator or a long blocked activation merely because no callback
  // ran to invalidate it (CaptureReadinessSettings::freshnessMillis). Regular
  // samples issue fresh tickets, never renew old ones.
  explicit CaptureReadiness(std::function<int64_t()> now,
                            CaptureReadinessSettings settings = {})
      : now_(std::move(now)), settings_(settings) {}

  void Invalidate(int64_t generation = 0) {
    std::scoped_lock lock(mutex_);
    if (generation < generation_) return;
    generation_ = generation;
    ++revision_;
    proven_ = false;
  }

  void NetworkChanged(int64_t nowMillis) {
    std::scoped_lock lock(mutex_);
    if (proofSinceMillis_ < nowMillis) proofSinceMillis_ = nowMillis;
    ++networkRevision_;
    ++revision_;
    proven_ = false;
  }

  // NetworkChanged's proof barrier without the network change: proof sampled
  // before `nowMillis` no longer qualifies, and nothing is queued for the
  // device. For a stall the watchdog's evaluator measured that was too short to
  // be a sleep (FreezeResponseFor): what was proven before it is not current,
  // and the network did not move.
  void ProofBarrier(int64_t nowMillis) {
    std::scoped_lock lock(mutex_);
    if (proofSinceMillis_ < nowMillis) proofSinceMillis_ = nowMillis;
    ++revision_;
    proven_ = false;
  }

  // Event adapters use this session's steady clock, never the controller's
  // wall-clock uptime. The injected clock also drives deterministic delivery tests.
  int64_t ClockMillis() const { return now_(); }

  int64_t ProofSinceMillis() const {
    std::scoped_lock lock(mutex_);
    return proofSinceMillis_;
  }

  std::optional<CaptureNetworkEpoch> PendingNetworkEvent() const {
    std::scoped_lock lock(mutex_);
    if (cancelled_ || networkRevision_ == handledNetworkRevision_) return std::nullopt;
    return CaptureNetworkEpoch{.revision = networkRevision_, .millis = proofSinceMillis_};
  }

  // A completed SDK notification covers only the event sampled before its call.
  // Newer events, even in the same clock millisecond, remain pending for replay.
  void NetworkEventHandled(CaptureNetworkEpoch event) {
    std::scoped_lock lock(mutex_);
    if (handledNetworkRevision_ < event.revision && event.revision <= networkRevision_)
      handledNetworkRevision_ = event.revision;
  }

  CaptureSample BeginSample() const {
    std::scoped_lock lock(mutex_);
    return {.revision = revision_, .proofSinceMillis = proofSinceMillis_,
            .startedMillis = now_()};
  }

  // MinSatisfied is deliberately absent: one usable proven provider carries
  // traffic even when pool redundancy or another address family is still forming.
  void CompleteSample(CaptureSample sample, CaptureWindow before,
                      CaptureWindow after, int64_t usableProven) {
    std::scoped_lock lock(mutex_);
    if (cancelled_ || active_ || sample.revision != revision_) return;
    const bool proven = before.generation == after.generation &&
                        after.generation >= generation_ && after.generation > 0 &&
                        before.added > 0 && after.added > 0 && usableProven > 0 &&
                        FreshWithLock(sample.startedMillis);
    if (generation_ != after.generation || proven_ != proven) ++revision_;
    if (generation_ < before.generation) generation_ = before.generation;
    if (generation_ < after.generation) generation_ = after.generation;
    proven_ = proven;
    sampledMillis_ = sample.startedMillis;
  }

  std::optional<CaptureTicket> Ready() const {
    std::scoped_lock lock(mutex_);
    if (cancelled_ || active_ || !proven_ || !FreshWithLock(sampledMillis_))
      return std::nullopt;
    return CaptureTicket{.revision = revision_, .generation = generation_,
                         .sampledMillis = sampledMillis_};
  }

  bool Current(CaptureTicket ticket) const {
    std::scoped_lock lock(mutex_);
    return CurrentWithLock(ticket);
  }

  bool Commit(CaptureTicket ticket) {
    std::scoped_lock lock(mutex_);
    if (!CurrentWithLock(ticket)) return false;
    active_ = true;
    return true;
  }

  void Cancel() {
    std::scoped_lock lock(mutex_);
    cancelled_ = true;
    proven_ = false;
    ++revision_;
  }

  bool Cancelled() const {
    std::scoped_lock lock(mutex_);
    return cancelled_;
  }

 private:
  bool CurrentWithLock(CaptureTicket ticket) const {
    return !cancelled_ && !active_ && proven_ &&
           ticket.revision == revision_ && ticket.generation == generation_ &&
           FreshWithLock(ticket.sampledMillis);
  }

  bool FreshWithLock(int64_t sampledMillis) const {
    const int64_t age = now_() - sampledMillis;
    return 0 <= age && age <= settings_.freshnessMillis;
  }

  static int64_t SteadyMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  mutable std::mutex mutex_;
  uint64_t revision_ = 0;
  int64_t generation_ = 0;
  int64_t proofSinceMillis_ = -1;
  uint64_t networkRevision_ = 0;
  uint64_t handledNetworkRevision_ = 0;
  int64_t sampledMillis_ = 0;
  const std::function<int64_t()> now_;
  const CaptureReadinessSettings settings_;
  bool proven_ = false;
  bool cancelled_ = false;
  bool active_ = false;
};

// Egress callbacks own the session, not its replaceable watchdog channel.
// Record proof invalidation even when no listener exists during replacement.
template <class Notify>
auto CaptureNetworkEventHandler(std::shared_ptr<CaptureReadiness> session, Notify notify) {
  return [session = std::move(session), notify = std::move(notify)] {
    if (!session || session->Cancelled()) return;
    const int64_t eventMillis = session->ClockMillis();
    session->NetworkChanged(eventMillis);
    notify(session, eventMillis);
  };
}

inline bool CaptureNetworkEventMatchesSession(
    const std::shared_ptr<CaptureReadiness>& listenerSession,
    const std::shared_ptr<CaptureReadiness>& eventSession) {
  return listenerSession && listenerSession == eventSession && !eventSession->Cancelled();
}

// Publish the listener before replaying this level. A change in the attachment
// gap is then either replayed here or delivered normally; only one epoch is kept.
template <class Notify>
void ReplayCaptureNetworkEvent(const std::shared_ptr<CaptureReadiness>& session,
                               Notify notify) {
  if (!session || session->Cancelled()) return;
  if (const auto event = session->PendingNetworkEvent()) notify(session, event->millis);
}

enum class CaptureStage { Prepare, Firewall, Network, Connected };
enum class CaptureResult { Waiting, Active, Halted };

// Prepare starts the pump and configures split routing before any capture.
// Each external step is followed by a freshness check; cancellation or health
// churn during an OS call rolls back the whole machine transaction. Exceptions
// also roll back before the caller tears down SDK objects on its stop budget.
template <class Apply, class Rollback>
CaptureResult ApplyCapture(CaptureReadiness& readiness, CaptureTicket ticket,
                           Apply apply, Rollback rollback) {
  if (!readiness.Current(ticket)) return CaptureResult::Waiting;
  try {
    for (const auto stage : {CaptureStage::Prepare, CaptureStage::Firewall,
                             CaptureStage::Network, CaptureStage::Connected}) {
      if (!readiness.Current(ticket)) {
        rollback();
        return CaptureResult::Waiting;
      }
      if (!apply(stage)) {
        rollback();
        return CaptureResult::Halted;
      }
    }
    if (readiness.Commit(ticket)) return CaptureResult::Active;
    rollback();
    return CaptureResult::Waiting;
  } catch (...) {
    rollback();
    throw;
  }
}

}  // namespace urnw
