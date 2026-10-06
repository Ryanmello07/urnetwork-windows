// How a launch of the tray app reaches the one instance that runs, and how
// that instance takes it, over the instance's whole life: while it starts,
// while it runs and while it exits (the last open item of the tray's Quit,
// AppLifetime.h).
//
// The defect. Single-instancing is the Windows App SDK's (main.cpp): the
// first launch registers the key, and every later launch redirects its
// activation to the key's holder and exits. Quit holds the holder's UI thread
// while it stops the service (normally under ~1.5 s, bounded by the pipe and
// StopBudget.h), and the holder kept the key throughout. A launch in that
// window was redirected to it: the App SDK raised AppInstance::Activated on a
// threadpool thread, the handler queued "show the window" for a UI thread that
// was quitting and never ran it, and the launch ended as handed over, so
// nothing started. A launch that reached the holder after its listener had
// gone waited on an event nobody would set (the App SDK's redirect waits on
// that event alone), and after 15 s said that URnetwork was already running
// but did not respond.
//
// The hand-over has a half in each process.
//   The holder (ActivationGate). Every launch redirected to it goes through
//   the gate from the Activated handler, and the handler returns only once the
//   instance has decided: served (the UI thread took it) or refused (the
//   instance is exiting). Before the UI is up the gate holds launches, and the
//   UI serves them when it opens the gate. Exiting closes the gate, which
//   raises the instance's exiting signal before it refuses anything: every
//   launch not yet served, and every later one. So a refused launch always
//   finds the signal raised, and no launch is lost between the handler and a
//   UI thread that will not run again.
//   The launch (Launch). Before it redirects, it watches the holder: its
//   process, and its exiting signal, held open so that it still reads as
//   raised after that process has gone. A redirect that ends with the holder
//   exiting (refused, or served by an instance that began to quit right after)
//   waits, bounded, for the holder's process to end, then registers again and
//   starts as the new holder. A redirect into a holder whose process ends
//   first registers again at once. Only a holder that still runs, is not
//   exiting and did not take the launch earns one of the "already running"
//   messages.
//
// What a launch shows (owner decision, 2026-10-05: "autostart on system start
// should launch only the tray icon"). Every launch the user starts opens the
// window, whether it starts the app or reaches the instance that runs, and so
// does one that starts the app after waiting out a quitting instance. An
// autostart (kAutostartArgument) shows only the tray icon, wherever it lands.
// A deep link is routed, and its handling brings the window forward.
//
// No launch starts the app while an update installs (owner decision,
// 2026-10-05). Every round of a launch asks first whether the updater's
// installer still runs (UpdateMarker.h), and if it does, the launch exits with
// a short notice instead of redirecting, waiting or starting.
//
// Pure, header-only and free of Windows headers, like AppLifetime.h:
// tools/instance-handover-tests.cpp runs it on any host, App/SingleInstance.cpp
// binds the exiting signal and main.cpp the App SDK.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace urnw::instance {

// How long the holder keeps a redirect open for its UI thread to take the
// launch. The UI takes it within a frame unless it is busy (creating the
// window for the first time, a modal loop); past this the handler returns with
// the launch still queued, and the UI serves it when it gets to it.
inline constexpr std::chrono::milliseconds kServeBudget{10000};

// How long a launch waits for the holder to take its redirect before it says
// the holder did not respond. This wait used to be infinite, and a launch
// that hangs with no window and no message is the very failure the startup
// path exists to make visible (Startup.h).
inline constexpr std::chrono::milliseconds kRedirectBudget{15000};

// A holder that is merely busy is never reported as not responding.
static_assert(kServeBudget < kRedirectBudget);

// How long a launch waits for an exiting holder's process to end before it
// says that URnetwork is still closing. Quit normally ends in under ~1.5 s,
// and its worst case with a healthy service is the stop's lock and sdk
// teardown budgets (StopBudget.h, 7 s together) plus reverting the machine
// state and retiring the provider, well inside this. Past it the service is
// wedged (each pipe call then takes its full 30 s timeout), and saying so
// beats a launch that waits without a word.
inline constexpr std::chrono::milliseconds kExitingHolderBudget{30000};

// Rounds of redirecting and registering one launch makes. The holder it waited
// out can be followed by another launch that registered first, which can be
// exiting in turn; a few rounds cover every real sequence, and the bound keeps
// a launch from cycling.
inline constexpr int kLaunchRounds = 4;

// The argument an autostart at sign-in passes. Nothing registers an autostart
// yet: the installer has no Run key, Startup shortcut or scheduled task, and
// the app has no launch-at-login setting (NEXTSTEPS.md, M5). A registration
// added later must pass it, and build_contract_test.go fails one that does
// not.
inline constexpr std::wstring_view kAutostartArgument = L"--autostart";

// The argument of the installer's relaunch after an update the update helper
// ran: the MSI starts URnetworkUpdate.exe, which starts the app with it once
// the update's files are in place (installer/Package.wxs). The helper still
// waits on msiexec then, and the update marker names the helper, so this
// launch waits for that update to end, at most kAfterUpdateBudget, before it
// asks like every launch (main.cpp). It changes nothing else: the launch opens
// the window, as the user's launch does.
inline constexpr std::wstring_view kAfterUpdateArgument = L"--after-update";

// How long the relaunch waits for the update to end: msiexec ends a moment
// after it starts the relaunch, and the helper a moment after msiexec.
inline constexpr std::chrono::milliseconds kAfterUpdateBudget{120000};

// Whether `argument` is one of the arguments on a command line, matched whole
// and unquoted, the program name included (it never equals an argument).
inline bool HasArgument(std::wstring_view commandLine, std::wstring_view argument) {
  std::wstring token;
  bool quoted = false;
  bool inToken = false;
  for (const wchar_t c : commandLine) {
    if (c == L'"') {
      quoted = !quoted;
      inToken = true;
      continue;
    }
    if (!quoted && (c == L' ' || c == L'\t')) {
      if (inToken && token == argument) return true;
      token.clear();
      inToken = false;
      continue;
    }
    token.push_back(c);
    inToken = true;
  }
  return inToken && token == argument;
}

// What a launch asks of the instance that takes it: the instance's own launch
// (App::OnLaunched) or one redirected to it (the gate).
struct LaunchRequest {
  // The urnetwork:// deep link it carries (a wallet callback, a campaign
  // email's link), or empty.
  std::string deepLink;
  // An autostart at sign-in (kAutostartArgument), not the user starting the
  // app.
  bool autostart = false;
};

// What the instance does for a launch.
enum class LaunchAction {
  // Route the deep link; handling it brings the window forward.
  HandleDeepLink,
  // Open the window.
  ShowWindow,
  // Nothing beyond the tray icon.
  TrayOnly,
};

// The action for a launch: a deep link is routed, the user's launch opens the
// window, and an autostart shows only the tray icon.
inline LaunchAction ActionFor(const LaunchRequest& request) {
  if (!request.deepLink.empty()) return LaunchAction::HandleDeepLink;
  return request.autostart ? LaunchAction::TrayOnly : LaunchAction::ShowWindow;
}

// For logs.
constexpr const char* ToString(LaunchAction action) {
  switch (action) {
    case LaunchAction::HandleDeepLink: return "route the deep link";
    case LaunchAction::ShowWindow: return "show the window";
    case LaunchAction::TrayOnly: return "the tray icon only";
  }
  return "unknown";
}

// Takes the launches redirected to this instance (see the file comment).
// Safe for concurrent use: Take runs on the App SDK's threadpool thread, Open
// and Close on the UI thread, and Close also on the threadpool thread when the
// UI thread no longer takes work.
class ActivationGate {
 public:
  enum class Outcome {
    // This instance acts on it: the UI thread took it.
    Served,
    // This instance is exiting and will not act on it. The exiting signal was
    // raised before the refusal.
    Refused,
    // The deadline passed first. It is still queued, and the UI thread serves
    // it unless the instance exits first.
    Undecided,
  };

  // Hands work to the UI thread; false when that thread takes no more.
  using Post = std::function<bool(std::function<void()>)>;
  // Acts on a launch, on the UI thread (ActionFor says what it asks).
  using Serve = std::function<void(const LaunchRequest& request)>;

  // `raiseExitingSignal` runs at the start of every Close, before anything is
  // refused, and must be idempotent. `blocking` is a test seam: it runs when
  // Take is about to wait, while the state lock is still held, so a test that
  // waits for it knows Take is blocked by the time the test can take the lock.
  explicit ActivationGate(std::function<void()> raiseExitingSignal,
                          std::function<void()> blocking = {})
      : raiseExitingSignal_(std::move(raiseExitingSignal)), blocking_(std::move(blocking)) {}

  // A launch that reached this instance. Returns once the instance has served
  // or refused it, or at the deadline with the launch still queued.
  Outcome Take(LaunchRequest request, std::chrono::steady_clock::time_point deadline) {
    std::uint64_t id = 0;
    Post post;
    {
      std::scoped_lock lock(stateLock_);
      if (state_ == State::Closed) return Outcome::Refused;
      id = ++lastId_;
      if (state_ == State::Open) post = post_;
      launches_.emplace(id,
                        Launch{.request = std::move(request), .posted = static_cast<bool>(post)});
    }
    // Outside the lock: the UI thread's queue is not the gate's to hold up.
    if (post && !post([this, id] { ServePosted(id); })) {
      // The UI thread takes no more work, so this instance is ending.
      Close();
    }
    return Await(id, deadline);
  }

  // The UI thread is up. Serves every launch held so far, in the order they
  // arrived, then posts each later one to the UI thread. Once; nothing after a
  // Close.
  void Open(Post post, Serve serve) {
    std::vector<LaunchRequest> heldRequests;
    {
      std::scoped_lock lock(stateLock_);
      if (state_ != State::Starting) return;
      state_ = State::Open;
      post_ = std::move(post);
      serve_ = serve;
      for (auto it = launches_.begin(); it != launches_.end();) {
        heldRequests.push_back(it->second.request);
        it->second.outcome = Outcome::Served;
        it = it->second.abandoned ? launches_.erase(it) : std::next(it);
      }
    }
    decided_.notify_all();
    for (const LaunchRequest& request : heldRequests) serve(request);
  }

  // The instance is exiting. Raises the exiting signal first, then refuses
  // every launch not yet served and every later one. Idempotent.
  void Close() {
    if (raiseExitingSignal_) raiseExitingSignal_();
    {
      std::scoped_lock lock(stateLock_);
      if (state_ == State::Closed) return;
      state_ = State::Closed;
      for (auto it = launches_.begin(); it != launches_.end();) {
        if (it->second.outcome == Outcome::Undecided) it->second.outcome = Outcome::Refused;
        // an abandoned launch that was posted goes when its posted work runs
        it = it->second.abandoned && !it->second.posted ? launches_.erase(it) : std::next(it);
      }
    }
    decided_.notify_all();
  }

  // Whether Close has run.
  bool Closed() const {
    std::scoped_lock lock(stateLock_);
    return state_ == State::Closed;
  }

 private:
  // Starting until the UI opens the gate, Open until the instance exits.
  enum class State { Starting, Open, Closed };

  // One launch that reached this instance.
  struct Launch {
    LaunchRequest request;
    // Handed to the UI thread's queue, which will run ServePosted for it.
    bool posted = false;
    Outcome outcome = Outcome::Undecided;
    // Take gave up at its deadline, so nobody reads the outcome: whoever is
    // last to touch it drops it.
    bool abandoned = false;
  };

  // A launch posted to the UI thread, on that thread.
  void ServePosted(std::uint64_t id) {
    LaunchRequest request;
    Serve serve;
    {
      std::scoped_lock lock(stateLock_);
      const auto it = launches_.find(id);
      if (it == launches_.end()) return;
      const bool refused = it->second.outcome != Outcome::Undecided;
      if (!refused) {
        it->second.outcome = Outcome::Served;
        request = it->second.request;
        serve = serve_;
      }
      if (it->second.abandoned) launches_.erase(it);
      // refused before the UI thread got here
      if (refused) return;
    }
    decided_.notify_all();
    serve(request);
  }

  // Waits for the launch's outcome until the deadline.
  Outcome Await(std::uint64_t id, std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(stateLock_);
    const auto decided = [this, id] {
      return launches_.at(id).outcome != Outcome::Undecided;
    };
    if (!decided() && blocking_) blocking_();
    decided_.wait_until(lock, deadline, decided);
    const auto it = launches_.find(id);
    const Outcome outcome = it->second.outcome;
    if (outcome == Outcome::Undecided) {
      it->second.abandoned = true;
    } else {
      launches_.erase(it);
    }
    return outcome;
  }

  std::function<void()> raiseExitingSignal_;
  std::function<void()> blocking_;
  mutable std::mutex stateLock_;
  std::condition_variable decided_;
  State state_ = State::Starting;
  Post post_;
  Serve serve_;
  std::uint64_t lastId_ = 0;
  std::map<std::uint64_t, Launch> launches_;
};

// For logs.
constexpr const char* ToString(ActivationGate::Outcome outcome) {
  switch (outcome) {
    case ActivationGate::Outcome::Served: return "served";
    case ActivationGate::Outcome::Refused: return "refused: this instance is exiting";
    case ActivationGate::Outcome::Undecided: return "still queued for the UI thread";
  }
  return "unknown";
}

// How a launch's redirect to the key's holder ended, as the launch saw it.
enum class RedirectResult {
  // The holder's Activated handler returned: it served the launch, or refused
  // it because it is exiting.
  Taken,
  // The App SDK reported that the redirect failed.
  Failed,
  // The holder had not taken it within kRedirectBudget.
  TimedOut,
  // Waiting for the redirect failed.
  WaitFailed,
  // The holder's process ended before it took the redirect, or had already
  // ended when the launch looked for it.
  HolderGone,
  // The redirect could not be started.
  NotStarted,
};

// For logs.
constexpr const char* ToString(RedirectResult result) {
  switch (result) {
    case RedirectResult::Taken: return "taken";
    case RedirectResult::Failed: return "failed";
    case RedirectResult::TimedOut: return "timed out";
    case RedirectResult::WaitFailed: return "the wait failed";
    case RedirectResult::HolderGone: return "the holder's process ended";
    case RedirectResult::NotStarted: return "not started";
  }
  return "unknown";
}

// A redirect as the launch saw it end.
struct RedirectAttempt {
  RedirectResult result = RedirectResult::NotStarted;
  // The holder had raised its exiting signal, or its process had ended, by the
  // time the redirect ended.
  bool holderExiting = false;
};

// What a launch does after a redirect.
enum class Step {
  // The running instance has the launch: this process exits.
  Exit,
  // Register for the key again now: its holder has gone.
  RegisterAgain,
  // Wait for the exiting holder's process to end, then register again.
  AwaitHolderExitThenRegister,
  // Tell the user what failed.
  Report,
};

// The step after a redirect. Only a holder that runs, is not exiting and took
// the launch ends it; only one that runs and is not exiting is reported.
constexpr Step AfterRedirect(RedirectAttempt attempt) {
  if (attempt.result == RedirectResult::HolderGone) return Step::RegisterAgain;
  // Refused because it is exiting, or served by an instance that began to
  // quit at once: either way the launch starts after it.
  if (attempt.holderExiting) return Step::AwaitHolderExitThenRegister;
  if (attempt.result == RedirectResult::Taken) return Step::Exit;
  return Step::Report;
}

// What a launch ends as.
enum class LaunchResult {
  // This process holds the key and starts the app.
  Holder,
  // The running instance took the launch; this process exits.
  HandedOver,
  // A running instance that is not exiting did not take it within
  // kRedirectBudget.
  NoResponse,
  // The App SDK reported that the redirect failed.
  Refused,
  // Waiting for the redirect failed.
  Unreachable,
  // The redirect could not be started.
  NotStarted,
  // The holder is exiting and its process did not end within
  // kExitingHolderBudget, or the key kept changing hands for kLaunchRounds.
  StillClosing,
  // Registering for the key threw: the App SDK is not usable.
  RegistrationFailed,
  // The updater's installer runs (UpdateMarker.h): this process exits without
  // starting the app, with a short notice unless it is an autostart.
  Updating,
};

// For logs.
constexpr const char* ToString(LaunchResult result) {
  switch (result) {
    case LaunchResult::Holder: return "this process holds the key";
    case LaunchResult::HandedOver: return "handed over to the running instance";
    case LaunchResult::NoResponse: return "the running instance did not respond";
    case LaunchResult::Refused: return "the redirect failed";
    case LaunchResult::Unreachable: return "the redirect could not be waited for";
    case LaunchResult::NotStarted: return "the redirect could not be started";
    case LaunchResult::StillClosing: return "the running instance is still closing";
    case LaunchResult::RegistrationFailed: return "registering for the key failed";
    case LaunchResult::Updating: return "an update is installing";
  }
  return "unknown";
}

// The message for a redirect that failed while the holder still runs and is
// not exiting.
constexpr LaunchResult ReportFor(RedirectResult result) {
  switch (result) {
    case RedirectResult::Failed: return LaunchResult::Refused;
    case RedirectResult::TimedOut: return LaunchResult::NoResponse;
    case RedirectResult::WaitFailed: return LaunchResult::Unreachable;
    case RedirectResult::NotStarted: return LaunchResult::NotStarted;
    case RedirectResult::Taken:
    case RedirectResult::HolderGone: break;
  }
  return LaunchResult::NoResponse;
}

// Drives one launch to its end (see the file comment), the first launch
// included: it holds the key at once and starts, unless an update installs.
// `launcher` binds the App SDK (main.cpp):
//   bool UpdateInProgress();     the updater's installer still runs
//   bool OwnsKey() const;        this process holds the key
//   bool Register();             find or register for the key again; false
//                                when that threw
//   RedirectAttempt Redirect();  hand this launch to the key's holder
//   bool AwaitHolderExit();      wait, at most kExitingHolderBudget, for the
//                                holder's process to end; true when it did
template <class Launcher>
LaunchResult Launch(Launcher& launcher) {
  for (int round = 0;; ++round) {
    if (launcher.UpdateInProgress()) return LaunchResult::Updating;
    if (launcher.OwnsKey()) return LaunchResult::Holder;
    if (round == kLaunchRounds) return LaunchResult::StillClosing;
    const RedirectAttempt attempt = launcher.Redirect();
    switch (AfterRedirect(attempt)) {
      case Step::Exit: return LaunchResult::HandedOver;
      case Step::Report: return ReportFor(attempt.result);
      case Step::AwaitHolderExitThenRegister:
        // an instance exiting for the installer is not waited out: the launch
        // could only start over the files the installer is replacing
        if (launcher.UpdateInProgress()) return LaunchResult::Updating;
        if (!launcher.AwaitHolderExit()) return LaunchResult::StillClosing;
        break;
      case Step::RegisterAgain: break;
    }
    if (!launcher.Register()) return LaunchResult::RegistrationFailed;
  }
}

}  // namespace urnw::instance
