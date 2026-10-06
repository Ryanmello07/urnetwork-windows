// Executable spec for how a launch reaches the one running instance of the tray
// app, and how that instance takes it while it starts, runs and exits
// (Common/InstanceHandover.h). The defect it pins: a launch during the moment
// Quit spends stopping the service was redirected to the quitting instance and
// lost, or after 15 s was told that URnetwork was already running but did not
// respond. Run against the same header the app compiles, on any host with a
// C++20 compiler.
//
//   c++ -std=c++20 -pthread -I ../src/Common instance-handover-tests.cpp -o /tmp/instance-handover-tests && /tmp/instance-handover-tests
//
// The threads here are ordered by the gate's own seam (it reports when Take
// blocks, with its state lock still held) and by past deadlines, never by
// sleeps. Every wait for another thread is bounded, so a regression is a
// failed check, not a hang.
//
// SPDX-License-Identifier: MPL-2.0

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "InstanceHandover.h"

using namespace urnw::instance;
using Outcome = ActivationGate::Outcome;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << "\n";
  }
}

// How long any wait for another thread may take before it counts as a failed
// check. Generous: it bounds a regression, it does not order anything.
constexpr std::chrono::seconds kBound{5};

// A wallet callback, synthetic.
const std::string kDeepLink = "urnetwork://callback.example/wallet?address=example";

std::chrono::steady_clock::time_point Later() { return std::chrono::steady_clock::now() + kBound; }

// Counts events another thread reports, and waits, bounded, for a number of
// them.
class Counter {
 public:
  void Add() {
    {
      std::scoped_lock lock(stateLock_);
      ++count_;
    }
    changed_.notify_all();
  }

  // True once `count` events have been reported, false when the bound ran out.
  bool WaitFor(int count) {
    std::unique_lock lock(stateLock_);
    return changed_.wait_for(lock, kBound, [this, count] { return count_ >= count; });
  }

  int Count() {
    std::scoped_lock lock(stateLock_);
    return count_;
  }

 private:
  std::mutex stateLock_;
  std::condition_variable changed_;
  int count_ = 0;
};

// The UI thread, played by the test's own thread: posted work queues until
// the test runs it, and the launches it serves are recorded.
class FakeUi {
 public:
  ActivationGate::Post Post() {
    return [this](std::function<void()> work) {
      std::scoped_lock lock(stateLock_);
      if (!takesWork_) return false;
      queue_.push_back(std::move(work));
      return true;
    };
  }

  ActivationGate::Serve Serve() {
    return [this](const std::string& deepLink) {
      std::scoped_lock lock(stateLock_);
      served_.push_back(deepLink);
    };
  }

  // Runs everything posted so far, as the UI thread's queue would.
  void RunQueued() {
    std::vector<std::function<void()>> work;
    {
      std::scoped_lock lock(stateLock_);
      work.swap(queue_);
    }
    for (const auto& item : work) item();
  }

  std::vector<std::string> Served() {
    std::scoped_lock lock(stateLock_);
    return served_;
  }

  std::size_t Queued() {
    std::scoped_lock lock(stateLock_);
    return queue_.size();
  }

  // The UI thread's queue is shutting down.
  void StopTakingWork() {
    std::scoped_lock lock(stateLock_);
    takesWork_ = false;
  }

 private:
  std::mutex stateLock_;
  std::vector<std::function<void()>> queue_;
  std::vector<std::string> served_;
  bool takesWork_ = true;
};

// A UI thread that runs posted work at once.
ActivationGate::Post Inline() {
  return [](std::function<void()> work) {
    work();
    return true;
  };
}

void TestARunningInstanceServesALaunchThroughItsUiThread() {
  std::vector<std::string> served;
  int raised = 0;
  ActivationGate gate([&raised] { ++raised; });
  gate.Open(Inline(), [&served](const std::string& deepLink) { served.push_back(deepLink); });
  const Outcome plain = gate.Take("", Later());
  const Outcome callback = gate.Take(kDeepLink, Later());
  Check(plain == Outcome::Served,
        "running: Take returns once the UI thread has taken a plain launch (served)");
  Check(callback == Outcome::Served, "running: and a deep link (served)");
  Check(served == std::vector<std::string>{"", kDeepLink},
        "running: the UI acts on each launch once, in the order they came");
  Check(raised == 0, "running: nothing raises the exiting signal");
  gate.Close();
  gate.Close();
  Check(served.size() == 2, "running: a later exit undoes nothing that was served");
  Check(gate.Closed() && gate.Take("", Later()) == Outcome::Refused,
        "running: a second Close changes nothing");
}

void TestTakeHoldsTheRedirectUntilTheUiThreadTakesTheLaunch() {
  FakeUi ui;
  Counter blocked;
  ActivationGate gate([] {}, [&blocked] { blocked.Add(); });
  gate.Open(ui.Post(), ui.Serve());
  Outcome outcome = Outcome::Undecided;
  std::thread launch([&gate, &outcome] { outcome = gate.Take(kDeepLink, Later()); });
  Check(blocked.WaitFor(1),
        "running: Take holds the redirect while the launch waits in the UI thread's queue");
  ui.RunQueued();
  launch.join();
  Check(outcome == Outcome::Served,
        "running: the UI thread taking the queued launch ends the wait (served)");
  Check(ui.Served() == std::vector<std::string>{kDeepLink},
        "running: the queued launch is acted on once");
}

void TestAQuittingInstanceRefusesTheLaunchItHadQueued() {
  FakeUi ui;
  Counter blocked;
  int raised = 0;
  bool raisedBeforeRefusing = false;
  ActivationGate* gateForSignal = nullptr;
  ActivationGate gate(
      [&] {
        ++raised;
        // whatever Close refuses, it refuses after this
        raisedBeforeRefusing = !gateForSignal->Closed();
      },
      [&blocked] { blocked.Add(); });
  gateForSignal = &gate;
  gate.Open(ui.Post(), ui.Serve());
  Outcome outcome = Outcome::Undecided;
  std::thread launch([&gate, &outcome] { outcome = gate.Take("", Later()); });
  Check(blocked.WaitFor(1), "quit: the launch is queued for the UI thread when the quit begins");
  // AppController::Shutdown's BeginExiting, on the UI thread that then holds
  // itself in SdkHost::Quit and never runs its queue again
  gate.Close();
  launch.join();
  Check(outcome == Outcome::Refused,
        "quit: a launch queued for a UI thread that is quitting is refused, so the launch starts "
        "the app itself (it used to be dropped)");
  Check(raised >= 1 && raisedBeforeRefusing,
        "quit: the exiting signal is raised before anything is refused");
  ui.RunQueued();
  Check(ui.Served().empty(), "quit: a refused launch is never acted on");
}

void TestALaunchAfterTheExitBeganIsRefusedAtOnce() {
  std::vector<std::string> served;
  int raised = 0;
  Counter blocked;
  ActivationGate gate([&raised] { ++raised; }, [&blocked] { blocked.Add(); });
  gate.Open(Inline(), [&served](const std::string& deepLink) { served.push_back(deepLink); });
  gate.Close();
  const Outcome outcome = gate.Take(kDeepLink, Later());
  Check(outcome == Outcome::Refused,
        "exiting: a launch that arrives after the exit began is refused");
  Check(blocked.Count() == 0, "exiting: it is refused at once, without holding the redirect");
  Check(raised >= 1, "exiting: the exiting signal was raised");
  Check(served.empty(), "exiting: nothing is acted on");
}

void TestLaunchesHeldWhileStartingAreServedWhenTheUiOpens() {
  FakeUi ui;
  Counter blocked;
  ActivationGate gate([] {}, [&blocked] { blocked.Add(); });
  Outcome first = Outcome::Undecided;
  Outcome second = Outcome::Undecided;
  std::thread a([&gate, &first] { first = gate.Take("", Later()); });
  Check(blocked.WaitFor(1), "starting: a launch that arrives before the UI is up is held");
  std::thread b([&gate, &second] { second = gate.Take(kDeepLink, Later()); });
  Check(blocked.WaitFor(2), "starting: and so is the next one");
  gate.Open(ui.Post(), ui.Serve());
  a.join();
  b.join();
  Check(first == Outcome::Served && second == Outcome::Served,
        "starting: a launch held while the instance started is served when the UI opens");
  Check(ui.Served() == std::vector<std::string>{"", kDeepLink},
        "starting: held launches are served in the order they came");
  Check(ui.Queued() == 0, "starting: held launches are served as the UI opens, not posted later");
}

void TestAnInstanceThatFailsToStartRefusesWhatItHeld() {
  FakeUi ui;
  Counter blocked;
  int raised = 0;
  ActivationGate gate([&raised] { ++raised; }, [&blocked] { blocked.Add(); });
  Outcome outcome = Outcome::Undecided;
  std::thread launch([&gate, &outcome] { outcome = gate.Take("", Later()); });
  Check(blocked.WaitFor(1), "failed start: the launch is held while the instance starts");
  // App::OnLaunched's failure exit
  gate.Close();
  launch.join();
  Check(outcome == Outcome::Refused,
        "failed start: a launch held by an instance that fails to start is refused");
  Check(raised >= 1, "failed start: the exiting signal was raised");
  gate.Open(ui.Post(), ui.Serve());
  Check(ui.Served().empty() && gate.Take("", Later()) == Outcome::Refused,
        "failed start: a closed gate never opens");
}

void TestAUiThreadThatTakesNoMoreWorkEndsTheInstance() {
  FakeUi ui;
  int raised = 0;
  ActivationGate gate([&raised] { ++raised; });
  gate.Open(ui.Post(), ui.Serve());
  ui.StopTakingWork();
  const Outcome outcome = gate.Take("", Later());
  Check(outcome == Outcome::Refused, "no ui: a launch the UI thread will not take is refused");
  Check(gate.Closed() && raised >= 1,
        "no ui: the instance counts as exiting from then on, with its signal raised");
}

void TestALaunchTheUiHadNotReachedByTheDeadlineIsStillServed() {
  // a deadline already passed: Take returns without waiting
  const auto passed = std::chrono::steady_clock::now();
  FakeUi ui;
  ActivationGate gate([] {});
  gate.Open(ui.Post(), ui.Serve());
  Check(gate.Take(kDeepLink, passed) == Outcome::Undecided,
        "busy ui: past the deadline the handler returns with the launch still queued");
  ui.RunQueued();
  Check(ui.Served() == std::vector<std::string>{kDeepLink},
        "busy ui: a launch the UI had not reached by the deadline is still served");

  FakeUi overtaken;
  ActivationGate exiting([] {});
  exiting.Open(overtaken.Post(), overtaken.Serve());
  Check(exiting.Take("", passed) == Outcome::Undecided,
        "busy ui: another launch still queued at its deadline");
  exiting.Close();
  overtaken.RunQueued();
  Check(overtaken.Served().empty(),
        "busy ui: a queued launch the exit overtakes is never acted on");
}

// One instance holding the key, as a launch meets it.
struct FakeHolder {
  // What redirecting to it ends as.
  RedirectAttempt redirect;
  // Whether its process ends within kExitingHolderBudget when awaited.
  bool endsInTime = true;
};

// The App SDK as Launch drives it: the holders a launch meets in turn. Once
// the last has gone, registering makes the launch the holder.
class FakeLauncher {
 public:
  explicit FakeLauncher(std::vector<FakeHolder> holders, bool registerThrows = false)
      : holders_(std::move(holders)), registerThrows_(registerThrows), owns_(holders_.empty()) {}

  bool OwnsKey() const { return owns_; }

  bool Register() {
    calls.push_back("register");
    if (registerThrows_) return false;
    ++current_;
    owns_ = current_ >= holders_.size();
    return true;
  }

  RedirectAttempt Redirect() {
    calls.push_back("redirect");
    return holders_.at(current_).redirect;
  }

  bool AwaitHolderExit() {
    calls.push_back("await");
    return holders_.at(current_).endsInTime;
  }

  // Every call Launch made, in order.
  std::vector<std::string> calls;

 private:
  std::vector<FakeHolder> holders_;
  bool registerThrows_ = false;
  std::size_t current_ = 0;
  bool owns_ = false;
};

using Calls = std::vector<std::string>;

RedirectAttempt Attempt(RedirectResult result, bool holderExiting) {
  return RedirectAttempt{.result = result, .holderExiting = holderExiting};
}

void TestALaunchThatReachesARunningInstanceHandsOver() {
  FakeLauncher launcher({FakeHolder{.redirect = Attempt(RedirectResult::Taken, false)}});
  Check(Launch(launcher) == LaunchResult::HandedOver,
        "running: a launch the running instance took is handed over");
  Check(launcher.calls == Calls{"redirect"}, "running: and this process just exits");
}

void TestALaunchRefusedByAQuittingInstanceStartsOnceItHasEnded() {
  FakeLauncher launcher({FakeHolder{.redirect = Attempt(RedirectResult::Taken, true)}});
  Check(Launch(launcher) == LaunchResult::Holder,
        "quit: a launch refused by a quitting instance starts the app once that instance has "
        "ended (it used to be handed over and lost)");
  Check(launcher.calls == Calls{"redirect", "await", "register"},
        "quit: the launch waits for the exiting holder's process before it registers again");
}

void TestALaunchWhoseHolderEndedRegistersAgainAtOnce() {
  FakeLauncher during({FakeHolder{.redirect = Attempt(RedirectResult::HolderGone, true)}});
  Check(Launch(during) == LaunchResult::Holder,
        "gone: a launch whose holder ended before taking it registers again at once (it used to "
        "wait 15 s and say URnetwork was already running)");
  Check(during.calls == Calls{"redirect", "register"}, "gone: with nothing to wait for");
}

void TestAnExitingHolderThatDoesNotEndIsStillClosingNotRunning() {
  FakeLauncher launcher(
      {FakeHolder{.redirect = Attempt(RedirectResult::Taken, true), .endsInTime = false}});
  Check(Launch(launcher) == LaunchResult::StillClosing,
        "wedged quit: an exiting holder that does not end in time is reported as still closing, "
        "never as already running");
  Check(launcher.calls == Calls{"redirect", "await"}, "wedged quit: after one bounded wait");
}

void TestAnExitingHolderWhoseListenerHasGoneIsWaitedOut() {
  // the redirect reached a holder past its Activated listener (its teardown)
  FakeLauncher launcher({FakeHolder{.redirect = Attempt(RedirectResult::TimedOut, true)}});
  Check(Launch(launcher) == LaunchResult::Holder,
        "teardown: a redirect that timed out on an exiting holder starts the app once that "
        "holder has ended");
}

void TestOnlyARunningInstanceThatIsNotExitingIsReported() {
  struct Case {
    RedirectResult result;
    LaunchResult report;
    const char* what;
  };
  const Case cases[] = {
      {.result = RedirectResult::TimedOut, .report = LaunchResult::NoResponse, .what = "timed out"},
      {.result = RedirectResult::Failed, .report = LaunchResult::Refused, .what = "failed"},
      {.result = RedirectResult::WaitFailed, .report = LaunchResult::Unreachable,
       .what = "the wait failed"},
      {.result = RedirectResult::NotStarted, .report = LaunchResult::NotStarted,
       .what = "not started"},
  };
  for (const Case& c : cases) {
    FakeLauncher running({FakeHolder{.redirect = Attempt(c.result, false)}});
    Check(Launch(running) == c.report && running.calls == Calls{"redirect"},
          std::string("running, ") + c.what + ": reported at once, as before");
    FakeLauncher exiting({FakeHolder{.redirect = Attempt(c.result, true)}});
    Check(Launch(exiting) == LaunchResult::Holder &&
              exiting.calls == Calls{"redirect", "await", "register"},
          std::string("exiting, ") + c.what + ": waited out, then the app starts");
  }
}

void TestALaunchThatFindsANewerHolderHandsOverToIt() {
  // another launch registered first once the quitting instance had gone
  FakeLauncher launcher({FakeHolder{.redirect = Attempt(RedirectResult::Taken, true)},
                         FakeHolder{.redirect = Attempt(RedirectResult::Taken, false)}});
  Check(Launch(launcher) == LaunchResult::HandedOver,
        "newer holder: a launch that finds a newer holder after waiting hands over to it");
  Check(launcher.calls == Calls{"redirect", "await", "register", "redirect"},
        "newer holder: one wait, one registration, one more redirect");
}

void TestALaunchGivesUpAfterBoundedRounds() {
  std::vector<FakeHolder> holders;
  for (int i = 0; i < kLaunchRounds + 2; ++i) {
    holders.push_back(FakeHolder{.redirect = Attempt(RedirectResult::HolderGone, true)});
  }
  FakeLauncher launcher(holders);
  int redirects = 0;
  const LaunchResult result = Launch(launcher);
  for (const std::string& call : launcher.calls) {
    if (call == "redirect") ++redirects;
  }
  Check(result == LaunchResult::StillClosing && redirects == kLaunchRounds,
        "rounds: a launch gives up after kLaunchRounds rounds when the key keeps changing hands "
        "(got " + std::to_string(redirects) + " redirects)");
}

void TestARegistrationThatThrowsIsReported() {
  FakeLauncher launcher({FakeHolder{.redirect = Attempt(RedirectResult::HolderGone, true)}},
                        /*registerThrows=*/true);
  Check(Launch(launcher) == LaunchResult::RegistrationFailed,
        "registration: a registration that throws is reported");
}

void TestTheFirstLaunchJustStarts() {
  FakeLauncher launcher({});
  Check(Launch(launcher) == LaunchResult::Holder && launcher.calls.empty(),
        "first launch: the holder starts without redirecting anything");
}

void TestAfterRedirect() {
  struct Case {
    RedirectResult result;
    bool holderExiting;
    Step step;
  };
  const Case cases[] = {
      {.result = RedirectResult::Taken, .holderExiting = false, .step = Step::Exit},
      {.result = RedirectResult::Taken, .holderExiting = true,
       .step = Step::AwaitHolderExitThenRegister},
      {.result = RedirectResult::HolderGone, .holderExiting = true, .step = Step::RegisterAgain},
      {.result = RedirectResult::HolderGone, .holderExiting = false, .step = Step::RegisterAgain},
      {.result = RedirectResult::TimedOut, .holderExiting = false, .step = Step::Report},
      {.result = RedirectResult::TimedOut, .holderExiting = true,
       .step = Step::AwaitHolderExitThenRegister},
      {.result = RedirectResult::Failed, .holderExiting = false, .step = Step::Report},
      {.result = RedirectResult::Failed, .holderExiting = true,
       .step = Step::AwaitHolderExitThenRegister},
      {.result = RedirectResult::WaitFailed, .holderExiting = false, .step = Step::Report},
      {.result = RedirectResult::WaitFailed, .holderExiting = true,
       .step = Step::AwaitHolderExitThenRegister},
      {.result = RedirectResult::NotStarted, .holderExiting = false, .step = Step::Report},
      {.result = RedirectResult::NotStarted, .holderExiting = true,
       .step = Step::AwaitHolderExitThenRegister},
  };
  for (const Case& c : cases) {
    Check(AfterRedirect(Attempt(c.result, c.holderExiting)) == c.step,
          std::string("after redirect: ") + ToString(c.result) +
              (c.holderExiting ? ", holder exiting" : ", holder running"));
  }
}

void TestNames() {
  for (Outcome outcome : {Outcome::Served, Outcome::Refused, Outcome::Undecided}) {
    Check(std::string_view(ToString(outcome)) != "unknown", "names: every outcome");
  }
  for (RedirectResult result :
       {RedirectResult::Taken, RedirectResult::Failed, RedirectResult::TimedOut,
        RedirectResult::WaitFailed, RedirectResult::HolderGone, RedirectResult::NotStarted}) {
    Check(std::string_view(ToString(result)) != "unknown", "names: every redirect result");
  }
  for (LaunchResult result :
       {LaunchResult::Holder, LaunchResult::HandedOver, LaunchResult::NoResponse,
        LaunchResult::Refused, LaunchResult::Unreachable, LaunchResult::NotStarted,
        LaunchResult::StillClosing, LaunchResult::RegistrationFailed}) {
    Check(std::string_view(ToString(result)) != "unknown", "names: every launch result");
  }
}

}  // namespace

int main() {
  TestARunningInstanceServesALaunchThroughItsUiThread();
  TestTakeHoldsTheRedirectUntilTheUiThreadTakesTheLaunch();
  TestAQuittingInstanceRefusesTheLaunchItHadQueued();
  TestALaunchAfterTheExitBeganIsRefusedAtOnce();
  TestLaunchesHeldWhileStartingAreServedWhenTheUiOpens();
  TestAnInstanceThatFailsToStartRefusesWhatItHeld();
  TestAUiThreadThatTakesNoMoreWorkEndsTheInstance();
  TestALaunchTheUiHadNotReachedByTheDeadlineIsStillServed();
  TestALaunchThatReachesARunningInstanceHandsOver();
  TestALaunchRefusedByAQuittingInstanceStartsOnceItHasEnded();
  TestALaunchWhoseHolderEndedRegistersAgainAtOnce();
  TestAnExitingHolderThatDoesNotEndIsStillClosingNotRunning();
  TestAnExitingHolderWhoseListenerHasGoneIsWaitedOut();
  TestOnlyARunningInstanceThatIsNotExitingIsReported();
  TestALaunchThatFindsANewerHolderHandsOverToIt();
  TestALaunchGivesUpAfterBoundedRounds();
  TestARegistrationThatThrowsIsReported();
  TestTheFirstLaunchJustStarts();
  TestAfterRedirect();
  TestNames();
  std::cout << (gCases - gFailures) << "/" << gCases << " instance handover checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
