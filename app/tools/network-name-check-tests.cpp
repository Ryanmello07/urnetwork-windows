// Executable spec for the sign-up network-name check (App/NetworkNameCheck.h).
// The flow's timers are a recording fake, so the debounce and retry are
// stepped by hand: no clock, network or device is involved.
//
// Root causes pinned here (both kept Create disabled until the name was
// retyped):
//   - an errored availability check left the name not available and was
//     never retried;
//   - an api that was not ready when the debounce elapsed skipped the check
//     and left the name checking.
//
//   c++ -std=c++20 -I ../src/App network-name-check-tests.cpp -o /tmp/network-name-check-tests && /tmp/network-name-check-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>

#include "NetworkNameCheck.h"

using namespace urnw;

namespace {

int gFailures = 0;
std::string gCase;

void Expect(bool condition, const std::string& message) {
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL [" << gCase << "] " << message << "\n";
  }
}

// One-shot timers as flags: a started timer is pending until the test
// elapses it.
struct FakeTimers {
  bool debounce = false;
  bool retry = false;
  int retryStarts = 0;

  NetworkNameCheckTimers Make() {
    return NetworkNameCheckTimers{
        [this] { debounce = true; },
        [this] { debounce = false; },
        [this] { retry = true; ++retryStarts; },
        [this] { retry = false; },
    };
  }
};

// elapse a pending timer and fire the flow, as LoginPage's tick handler does
std::optional<uint32_t> Elapse(bool& pending, NetworkNameCheckFlow& flow, bool apiReady) {
  if (!pending) return std::nullopt;
  pending = false;
  return flow.Fire(/*checkable=*/true, apiReady);
}

void ErroredCheckIsNotTakenAndKeepsCreateUsable() {
  gCase = "errored check";
  FakeTimers timers;
  NetworkNameCheckFlow flow(timers.Make());
  flow.Edited(true);
  Expect(flow.state() == NetworkNameCheck::Checking, "an edit starts checking");
  Expect(!flow.AllowsCreate(), "Create waits for the check");
  auto generation = Elapse(timers.debounce, flow, /*apiReady=*/true);
  Expect(generation.has_value(), "the debounce runs the check");
  if (!generation) return;
  Expect(flow.Apply(*generation, /*ok=*/false, /*available=*/false), "the answer is current");
  Expect(flow.state() == NetworkNameCheck::Failed, "an errored check reads as failed, not taken");
  Expect(flow.AllowsCreate(), "an errored check leaves Create usable");
  Expect(timers.retry, "an errored check schedules a retry");
}

void RetryTurnsIntoARealAnswer() {
  gCase = "retry";
  FakeTimers timers;
  NetworkNameCheckFlow flow(timers.Make());
  flow.Edited(true);
  auto generation = Elapse(timers.debounce, flow, true);
  if (!generation) {
    Expect(false, "the debounce runs the check");
    return;
  }
  flow.Apply(*generation, false, false);
  auto retried = Elapse(timers.retry, flow, true);
  Expect(retried.has_value(), "the retry runs the check again");
  if (!retried) return;
  Expect(flow.Apply(*retried, true, true), "the retried answer is current");
  Expect(flow.state() == NetworkNameCheck::Available, "the retried answer is applied");
  Expect(!timers.retry, "an answer schedules no retry");
}

void RetriesAreBounded() {
  gCase = "bounded retries";
  FakeTimers timers;
  NetworkNameCheckFlow flow(timers.Make());
  flow.Edited(true);
  auto generation = Elapse(timers.debounce, flow, true);
  for (int i = 0; generation && i < 10; ++i) {
    flow.Apply(*generation, false, false);
    generation = Elapse(timers.retry, flow, true);
  }
  Expect(timers.retryStarts == kMaxNetworkNameCheckRetries,
         "retries stop after " + std::to_string(kMaxNetworkNameCheckRetries) + " (got " +
             std::to_string(timers.retryStarts) + ")");
  Expect(flow.AllowsCreate(), "Create stays usable after the retries run out");
  flow.Edited(true);
  generation = Elapse(timers.debounce, flow, true);
  if (generation) flow.Apply(*generation, false, false);
  Expect(timers.retry, "an edit grants a fresh set of retries");
}

void ApiNotReadyIsAFailedCheck() {
  gCase = "api not ready";
  FakeTimers timers;
  NetworkNameCheckFlow flow(timers.Make());
  flow.Edited(true);
  auto generation = Elapse(timers.debounce, flow, /*apiReady=*/false);
  Expect(!generation.has_value(), "no check is sent without an api");
  Expect(flow.state() == NetworkNameCheck::Failed, "the name does not stay checking");
  Expect(flow.AllowsCreate(), "Create is usable");
  Expect(timers.retry, "the check is retried");
  generation = Elapse(timers.retry, flow, /*apiReady=*/true);
  Expect(generation.has_value(), "the retry runs once the api is ready");
}

void TakenIsFinal() {
  gCase = "taken";
  FakeTimers timers;
  NetworkNameCheckFlow flow(timers.Make());
  flow.Edited(true);
  auto generation = Elapse(timers.debounce, flow, true);
  if (generation) flow.Apply(*generation, true, false);
  Expect(flow.state() == NetworkNameCheck::Taken, "an unavailable answer reads as taken");
  Expect(!flow.AllowsCreate(), "a taken name blocks Create");
  Expect(!timers.retry, "a taken name is not retried");
}

void EditSupersedesAnswerAndRetry() {
  gCase = "superseded";
  FakeTimers timers;
  NetworkNameCheckFlow flow(timers.Make());
  flow.Edited(true);
  auto generation = Elapse(timers.debounce, flow, true);
  if (generation) flow.Apply(*generation, false, false);
  flow.Edited(true);
  Expect(!timers.retry, "an edit cancels the pending retry");
  Expect(generation && !flow.Apply(*generation, true, true), "an old answer is dropped");
  Expect(flow.state() == NetworkNameCheck::Checking, "the new name is still checking");
  flow.Edited(false);
  Expect(flow.state() == NetworkNameCheck::TooShort && !flow.AllowsCreate(),
         "a short name blocks Create");
}

}  // namespace

int main() {
  ErroredCheckIsNotTakenAndKeepsCreateUsable();
  RetryTurnsIntoARealAnswer();
  RetriesAreBounded();
  ApiNotReadyIsAFailedCheck();
  TakenIsFinal();
  EditSupersedesAnswerAndRetry();
  if (gFailures != 0) {
    std::cout << gFailures << " network name check expectation(s) failed\n";
    return 1;
  }
  std::cout << "network name check: ok\n";
  return 0;
}
