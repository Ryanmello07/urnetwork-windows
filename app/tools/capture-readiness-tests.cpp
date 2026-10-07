// Deterministic regression tests for the production capture transaction.
// All machine effects are injected; this executable never changes networking.
// SPDX-License-Identifier: MPL-2.0
#include "CaptureReadiness.h"
#include "ConnectionHealth.h"

#include <barrier>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace urnw;

namespace {

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void Qualify(CaptureReadiness& readiness, int64_t generation = 1,
             int64_t added = 1, int64_t proven = 1) {
  const CaptureWindow window{.generation = generation, .added = added};
  readiness.CompleteSample(readiness.BeginSample(), window, window, proven);
}

// The production transaction calls these effects in the same order as the
// native service. Writes are individually observable, including partial failure.
struct Machine {
  bool killSwitch = false;
  bool pump = false;
  bool routes = false;
  std::string dns = "native";
  std::string firewall = "off";
  std::vector<CaptureStage> stages;
  int rollbackCount = 0;
  std::function<void(CaptureStage)> after;

  bool Apply(CaptureStage stage) {
    stages.push_back(stage);
    if (stage == CaptureStage::Prepare) pump = true;
    else {
      Check(pump, "a capture effect ran before the packet consumer was ready");
      if (stage == CaptureStage::Firewall) firewall = "connecting";
      if (stage == CaptureStage::Network) {
        routes = true;
        dns = "192.0.2.53";
      }
      if (stage == CaptureStage::Connected) firewall = "connected";
    }
    if (after) after(stage);
    return true;
  }

  void Rollback() {
    ++rollbackCount;
    routes = false;
    dns = "native";
    firewall = killSwitch ? "armed" : "off";
  }

  CaptureResult Run(CaptureReadiness& readiness, CaptureTicket ticket) {
    return ApplyCapture(readiness, ticket,
        [&](CaptureStage stage) { return Apply(stage); }, [&] { Rollback(); });
  }

  void CheckNative() const {
    Check(!routes && dns == "native", "pending/failed capture changed native route or DNS");
    Check(firewall == (killSwitch ? "armed" : "off"), "wrong rollback firewall policy");
  }
};

void TestBootstrapAndEmptyWindowPreserveNativeNetwork() {
  CaptureReadiness readiness;
  Machine machine;
  Check(!readiness.Ready(), "RPC/adapter construction is not provider proof");
  Check(machine.Run(readiness, {}) == CaptureResult::Waiting, "blocked bootstrap captured");
  Qualify(readiness, 1, 0, 0);
  Check(!readiness.Ready(), "no providers became ready");
  Qualify(readiness, 1, 1, 0);
  Check(!readiness.Ready(), "discovered/Added alone became ready");
  Check(machine.stages.empty(), "an unproven start touched machine configuration");
  machine.CheckNative();
}

void TestQualifiedPartialWindowActivatesPumpBeforeCapture() {
  CaptureReadiness readiness;
  Machine machine;
  // Pool MinSatisfied and IPv6 availability are deliberately not prerequisites:
  // a single usable IPv4 provider is sufficient even while both are false.
  Qualify(readiness, 4, 1, 1);
  Check(readiness.Ready().has_value(), "a usable provider in a partial window was rejected");
  Check(machine.Run(readiness, *readiness.Ready()) == CaptureResult::Active,
        "proved provider did not activate");
  Check(machine.stages == std::vector<CaptureStage>{CaptureStage::Prepare,
        CaptureStage::Firewall, CaptureStage::Network, CaptureStage::Connected},
        "activation effects ran out of order");
  Check(machine.pump && machine.routes && machine.dns == "192.0.2.53" &&
        machine.firewall == "connected", "capture did not commit all effects");
  Check(!readiness.Ready(), "committed capture can activate twice");
}

void TestOnlyUsableExitProofCounts() {
  const CaptureExit healthy{.proven = true, .probeAgeSeconds = 0};
  Check(CaptureExitIsUsable(healthy, 12000, 10000), "usable exit rejected");
  std::vector<CaptureExit> excluded{
      {.proven = false, .probeAgeSeconds = 0},
      {.proven = true, .done = true, .probeAgeSeconds = 0},
      {.proven = true, .quarantined = true, .probeAgeSeconds = 0},
      {.proven = true, .warning = true, .probeAgeSeconds = 0},
      {.proven = true, .probeAgeSeconds = -1},
      {.proven = true, .probeAgeSeconds = 20}};
  for (const auto exit : excluded)
    Check(!CaptureExitIsUsable(exit, 12000, 10000), "unusable exit authorized capture");
  Check(!CaptureExitIsUsable({.proven = true, .probeAgeSeconds = -1}, 12000, -1),
        "negative proof age qualified before the first network event");
}

void TestKillSwitchPendingAndFailureRemainProtected() {
  for (bool killSwitch : {false, true}) {
    CaptureReadiness readiness;
    Machine machine;
    machine.killSwitch = killSwitch;
    machine.firewall = killSwitch ? "armed" : "off";
    Check(machine.Run(readiness, {}) == CaptureResult::Waiting, "unproven capture ran");
    machine.CheckNative();
    Qualify(readiness);
    machine.after = [](CaptureStage stage) {
      if (stage == CaptureStage::Network) throw std::runtime_error("synthetic DNS failure");
    };
    bool threw = false;
    try { machine.Run(readiness, *readiness.Ready()); }
    catch (const std::runtime_error&) { threw = true; }
    Check(threw && machine.rollbackCount == 1, "partial activation failure was not rolled back");
    machine.CheckNative();
  }
}

void TestStaleAndAmbiguousDestinationSamplesRevokeProof() {
  CaptureReadiness readiness;
  Qualify(readiness, 1);
  const auto first = *readiness.Ready();
  readiness.CompleteSample(readiness.BeginSample(), {.generation = 1, .added = 1},
                          {.generation = 2, .added = 1}, 1);
  Check(!readiness.Ready() && !readiness.Current(first), "mixed-generation sample kept old proof");
  Qualify(readiness, 1);
  Check(!readiness.Ready(), "retired generation was revived");
  Qualify(readiness, 2);
  Check(readiness.Ready().has_value(), "new generation cannot qualify");
  const auto current = *readiness.Ready();
  readiness.Invalidate(1);
  Check(readiness.Current(current), "old callback invalidated a newer generation");
  readiness.CompleteSample(readiness.BeginSample(), {}, {}, 0);
  Check(!readiness.Ready(), "missing authoritative window retained earlier proof");
}

void TestHealthChurnCannotReviveAnOldTicket() {
  CaptureReadiness readiness;
  Qualify(readiness);
  const auto first = *readiness.Ready();
  const auto inFlight = readiness.BeginSample();
  readiness.Invalidate(1);
  readiness.CompleteSample(inFlight, {.generation = 1, .added = 1},
                          {.generation = 1, .added = 1}, 1);
  Check(!readiness.Ready(), "sample raced a health callback and still qualified");
  Qualify(readiness);
  Check(!readiness.Current(first), "same-generation recovery revived old permission");
  const auto recovered = *readiness.Ready();
  Qualify(readiness, 1, 1, 0);
  Qualify(readiness);
  Check(!readiness.Current(recovered), "negative then positive samples revived old permission");
}

void TestCancellationAndReplacementAtEveryCaptureStage() {
  for (bool stop : {false, true}) {
    for (auto changedAt : {CaptureStage::Prepare, CaptureStage::Firewall,
                           CaptureStage::Network, CaptureStage::Connected}) {
      CaptureReadiness readiness;
      Machine machine;
      Qualify(readiness);
      const auto ticket = *readiness.Ready();
      machine.after = [&](CaptureStage stage) {
        if (stage == changedAt) {
          if (stop) readiness.Cancel();
          else readiness.Invalidate(2);
        }
      };
      Check(machine.Run(readiness, ticket) == CaptureResult::Waiting,
            "superseded/cancelled transaction committed capture");
      Check(machine.rollbackCount == 1, "superseded transaction did not roll back");
      machine.CheckNative();
      Qualify(readiness, 2);
      Check(readiness.Ready().has_value() != stop, "cancellation was reversible or new proof refused");
    }
  }
}

void TestPartialFailureAndHaltAtEveryCaptureStage() {
  for (auto failAt : {CaptureStage::Prepare, CaptureStage::Firewall,
                      CaptureStage::Network, CaptureStage::Connected}) {
    for (bool throws : {false, true}) {
      CaptureReadiness readiness;
      Machine machine;
      Qualify(readiness);
      bool caught = false;
      try {
        const auto result = ApplyCapture(readiness, *readiness.Ready(),
            [&](CaptureStage stage) {
              machine.Apply(stage);
              if (stage != failAt) return true;
              if (throws) throw std::runtime_error("synthetic activation failure");
              return false;
            }, [&] { machine.Rollback(); });
        Check(result == CaptureResult::Halted, "explicit staged halt was ignored");
      } catch (const std::runtime_error&) { caught = true; }
      Check(caught == throws && machine.rollbackCount == 1, "failure/halt did not unwind exactly once");
      machine.CheckNative();
    }
  }
}

void TestStopDuringBlockedSampleAndOldSessionCallback() {
  CaptureReadiness oldSession;
  CaptureReadiness newSession;
  std::barrier barrier(2);
  std::thread worker([&] {
    const auto sample = oldSession.BeginSample();
    barrier.arrive_and_wait();
    barrier.arrive_and_wait();
    oldSession.CompleteSample(sample, {.generation = 7, .added = 1},
                             {.generation = 7, .added = 1}, 1);
  });
  barrier.arrive_and_wait();
  oldSession.Cancel();
  Qualify(newSession, 1);
  barrier.arrive_and_wait();
  worker.join();
  Check(!oldSession.Ready() && newSession.Ready().has_value(),
        "late old-session proof crossed the cancellation/session boundary");
}

void TestNetworkChangeAndResumeRequireFreshProof() {
  CaptureReadiness readiness;
  Qualify(readiness);
  const auto beforeSleep = *readiness.Ready();
  const auto oldSample = readiness.BeginSample();
  readiness.NetworkChanged(10000);  // resume/network event is an explicit barrier
  readiness.NetworkChanged(9000);  // an earlier event's callback arrived later
  Check(readiness.ProofSinceMillis() == 10000, "late notification moved the proof epoch backwards");
  Check(!readiness.Current(beforeSleep), "resume accepted pre-sleep readiness");
  readiness.CompleteSample(oldSample, {.generation = 1, .added = 1},
                          {.generation = 1, .added = 1}, 1);
  Check(!readiness.Ready(), "late callback restored a pre-network proof");
  Check(!CaptureProofAfterNetworkChange(20, 12000, 10000), "historical proof accepted");
  Check(!CaptureProofAfterNetworkChange(0, 10999, 10000), "ambiguous rounded age accepted");
  Check(!CaptureProofAfterNetworkChange(-1, 12000, 10000), "never-proven exit accepted");
  Check(CaptureProofAfterNetworkChange(0, 12000, 10000), "post-event proof rejected");
  Check(readiness.BeginSample().proofSinceMillis == 10000, "proof epoch lost on sample");
  Qualify(readiness);
  Check(readiness.Ready().has_value(), "fresh network proof cannot recover");
}

void TestPrivacySafeWaitingDiagnostics() {
  Check(std::string(CaptureWaitReason({}, 0, "")) == "waiting-selection", "selection stage missing");
  Check(std::string(CaptureWaitReason({.generation = 1}, 0,
        "https://fixture.example/?token=synthetic-token")) == "discovery-pending",
        "untrusted SDK reason leaked into diagnostic");
  Check(std::string(CaptureWaitReason({.generation = 1, .added = 1}, 0, "")) ==
        "qualification-pending", "provider presence was reported as proof");
  Check(std::string(CaptureWaitReason({.generation = 1}, 0, "platform-unreachable")) ==
        "platform-unreachable", "finite platform diagnosis not preserved");
  Check(!CaptureSampleStalled(1000, -1, 30999) && CaptureSampleStalled(1000, -1, 31000),
        "sample stall deadline is not based on session time");
  Check(!CaptureSampleStalled(31000, 2000, 31000), "suspended time was diagnosed as a stall");
}

void TestSuspendedActivationCannotCommitAnExpiredTicket() {
  int64_t now = 1000;
  CaptureReadiness readiness([&] { return now; });
  Qualify(readiness);
  const auto ticket = *readiness.Ready();
  Machine machine;
  machine.after = [&](CaptureStage stage) {
    if (stage == CaptureStage::Network) {
      now += 9000;  // suspended between the route write and final commit
      Qualify(readiness);  // a resumed sampler cannot renew the already-issued ticket
    }
  };
  Check(machine.Run(readiness, ticket) == CaptureResult::Waiting,
        "activation used a ticket from before a suspension");
  machine.CheckNative();
  Check(readiness.Ready().has_value(), "fresh sample cannot retry expired activation");
}

void TestNetworkEventSurvivesWatcherReplacement() {
  int64_t now = 10000;
  auto session = std::make_shared<CaptureReadiness>([&] { return now; });
  std::shared_ptr<CaptureReadiness> listenerSession = session;
  std::vector<int64_t> notifications;
  const auto notify = [&](const auto& source, int64_t eventMillis) {
    if (!CaptureNetworkEventMatchesSession(listenerSession, source)) return false;
    notifications.push_back(eventMillis);
    return true;
  };
  const auto onNetworkEvent = CaptureNetworkEventHandler(session, notify);
  Qualify(*session);
  const auto oldTicket = *session->Ready();
  ReplayCaptureNetworkEvent(session, notify);
  Check(notifications.empty() && session->Current(oldTicket),
        "watcher attachment invented a network change or revoked healthy proof");

  // Force Stop -> replacement subscription/read -> publish, with an OS event
  // in the absent-listener interval. This is the actual production callback seam.
  listenerSession.reset();
  now = 12000;
  onNetworkEvent();
  Check(!session->Ready() && !session->Current(oldTicket),
        "network event without a watcher retained pre-event proof");
  Check(session->ProofSinceMillis() == now && notifications.empty(),
        "listener absence lost the session event or queued SDK work");
  const CaptureWindow window{.generation = 1, .added = 1};
  now = 15000;
  const bool historical = CaptureExitIsUsable(
      {.proven = true, .probeAgeSeconds = 4}, now, session->ProofSinceMillis());
  session->CompleteSample(session->BeginSample(), window, window, historical ? 1 : 0);
  Check(!session->Ready(), "replacement watcher accepted pre-change qualification");

  listenerSession = session;
  ReplayCaptureNetworkEvent(session, notify);
  Check(notifications == std::vector<int64_t>{12000},
        "replacement watcher did not receive the retained event for a bounded SDK kick");
  session->NetworkEventHandled(*session->PendingNetworkEvent());
  ReplayCaptureNetworkEvent(session, notify);
  Check(notifications.size() == 1 && !session->PendingNetworkEvent(),
        "watcher replacement replayed an already completed SDK notification");

  now = 16000;
  onNetworkEvent();
  const auto notifying = *session->PendingNetworkEvent();
  onNetworkEvent();  // a newer event in the same clock millisecond
  session->NetworkEventHandled(notifying);
  Check(session->PendingNetworkEvent().has_value(),
        "completing an older SDK notification consumed a newer network event");
  const auto beforeReplay = notifications.size();
  ReplayCaptureNetworkEvent(session, notify);
  Check(notifications.size() == beforeReplay + 1 && notifications.back() == now,
        "newer network event was not retained across listener replacement");
  session->NetworkEventHandled(*session->PendingNetworkEvent());
  ReplayCaptureNetworkEvent(session, notify);
  Check(notifications.size() == beforeReplay + 1,
        "completed event remained in the replay queue");
  now = 18000;
  const bool fresh = CaptureExitIsUsable(
      {.proven = true, .probeAgeSeconds = 0}, now, session->ProofSinceMillis());
  session->CompleteSample(session->BeginSample(), window, window, fresh ? 1 : 0);
  Check(session->Ready().has_value(), "fresh post-change provider proof could not recover");
  Machine machine;
  Check(machine.Run(*session, *session->Ready()) == CaptureResult::Active,
        "fresh proof after watcher replacement could not activate capture");
}

void TestOldNetworkCallbackCannotNotifyReplacementSession() {
  int64_t now = 20000;
  auto oldSession = std::make_shared<CaptureReadiness>([&] { return now; });
  std::shared_ptr<CaptureReadiness> listenerSession = oldSession;
  std::barrier reachedListener(2);
  std::barrier resumeListener(2);
  int notifications = 0;
  auto onOldNetworkEvent = CaptureNetworkEventHandler(oldSession,
      [&](const auto& source, int64_t) {
        reachedListener.arrive_and_wait();
        resumeListener.arrive_and_wait();
        if (!CaptureNetworkEventMatchesSession(listenerSession, source)) return false;
        ++notifications;
        return true;
      });
  std::thread oldCallback(onOldNetworkEvent);
  reachedListener.arrive_and_wait();
  oldSession->Cancel();
  auto replacement = std::make_shared<CaptureReadiness>([&] { return now; });
  listenerSession = replacement;
  Qualify(*replacement);
  const auto replacementTicket = *replacement->Ready();
  resumeListener.arrive_and_wait();
  oldCallback.join();
  Check(notifications == 0 && replacement->Current(replacementTicket) &&
            replacement->ProofSinceMillis() == -1,
        "an in-flight old-session event contaminated the replacement watcher");
  // A callback invoked entirely after cancellation must not even reach notify.
  onOldNetworkEvent();
  Check(notifications == 0 && replacement->Current(replacementTicket),
        "cancelled session callback queued work or revoked replacement proof");
}

void TestServiceFailureClampsAStaleProvenProviderGrid() {
  using health::State;
  const auto staleProvider = State::Connected;
  Check(health::WithCapture(staleProvider, {.serviceConnected = true, .preparing = true}) ==
        State::Connecting, "provider proof advertised connected before capture");
  Check(health::WithCapture(staleProvider, {.serviceConnected = true, .failed = true}) ==
        State::Failed, "live pipe plus stale grid advertised connected after activation failed");
  Check(health::WithCapture(staleProvider, {.serviceConnected = true}) == State::Disconnected,
        "stopped service capture advertised connected");
  Check(health::WithCapture(staleProvider, {.serviceConnected = true, .active = true}) ==
        State::Connected, "active proven capture was not connected");
}

}  // namespace

int main() {
  try {
    TestBootstrapAndEmptyWindowPreserveNativeNetwork();
    TestQualifiedPartialWindowActivatesPumpBeforeCapture();
    TestOnlyUsableExitProofCounts();
    TestKillSwitchPendingAndFailureRemainProtected();
    TestStaleAndAmbiguousDestinationSamplesRevokeProof();
    TestHealthChurnCannotReviveAnOldTicket();
    TestCancellationAndReplacementAtEveryCaptureStage();
    TestPartialFailureAndHaltAtEveryCaptureStage();
    TestStopDuringBlockedSampleAndOldSessionCallback();
    TestNetworkChangeAndResumeRequireFreshProof();
    TestPrivacySafeWaitingDiagnostics();
    TestSuspendedActivationCannotCommitAnExpiredTicket();
    TestNetworkEventSurvivesWatcherReplacement();
    TestOldNetworkCallbackCannotNotifyReplacementSession();
    TestServiceFailureClampsAStaleProvenProviderGrid();
    std::cout << "15 capture lifecycle tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "capture lifecycle failure: " << error.what() << '\n';
    return 1;
  }
}
