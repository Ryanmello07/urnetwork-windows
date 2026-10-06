// The Win32 half of the single instance; the decisions are
// Common/InstanceHandover.h and Common/UpdateMarker.h. This instance's exiting
// signal and the gate that takes launches redirected to it, a launch's watch
// over the instance that holds the key, whether this launch is an autostart,
// and the marker that refuses launches while an update installs.
//
// The exiting signal is a manual-reset event named for the process id
// (ids::kExitingSignalPrefix). Every launch creates its own before it
// registers for the key, so it exists by the time any later launch can find
// this process holding the key, and closing the gate raises it. A launch opens
// the holder's signal by name before it redirects; its handle keeps the event,
// and whether it was raised, readable after the holder's process has gone.
//
// Plain Win32 and COM, no WinRT, so the .cpp builds without the precompiled
// header (App.vcxproj).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <cstdint>

#include "InstanceHandover.h"
#include "UpdateMarker.h"

namespace urnw {

// Creates this process's exiting signal. wWinMain, before the key.
void CreateExitingSignal();

// The gate that takes the launches redirected to this instance. It lives as
// long as the process: the App SDK's threadpool thread can reach it until the
// very end.
instance::ActivationGate& Activations();

// This instance is exiting: raises its exiting signal, then refuses every
// launch not yet served and every later one, so a launch from here on starts
// the app once this process has ended. The first step of every ending, and of
// a launch that fails before its window can exist. Idempotent; UI thread.
void BeginExiting();

// This process was started by an autostart at sign-in: its command line has
// instance::kAutostartArgument.
bool LaunchedByAutostart();

// This app is exiting while `installerProcess`, the update helper it started
// (a process handle with PROCESS_QUERY_LIMITED_INFORMATION), still runs: the
// installer is closing it. Until the helper ends, a launch exits instead of
// starting the app over the files the installer replaces. Before this app
// exits, a launch reaches it as any launch does.
void RecordUpdateInProgress(void* installerProcess);

// This process is the installer's relaunch after an update: its command line
// has instance::kAfterUpdateArgument.
bool LaunchedAfterUpdate();

// Waits, at most instance::kAfterUpdateBudget, for the update in progress to
// end (UpdateInProgress, which deletes the marker once its helper has ended;
// the wait itself is InstanceHandover.h's AwaitUpdateEnd). The relaunch calls
// it before it asks, so the update that started it does not turn it away.
void AwaitUpdateEnd();

// The updater's installer still runs. A stale marker (its installer ended, it
// is too old, it does not parse) is deleted on the way, so it never refuses a
// later launch.
bool UpdateInProgress();

// Tells the user that the app is updating and this launch will not start it.
// Blocks until dismissed, or at most kUpdatingNoticeMs: this process runs
// URnetwork.exe from the folder the installer is replacing, and every second
// it stays it holds those files open.
void ShowUpdatingNotice();

// A launch's watch over the instance that holds the key, opened before the
// launch redirects to it. Not safe for concurrent use; one launch thread owns
// it.
class HolderWatch {
 public:
  explicit HolderWatch(std::uint32_t processId);
  ~HolderWatch();
  HolderWatch(const HolderWatch&) = delete;
  HolderWatch& operator=(const HolderWatch&) = delete;

  // The holder's process had already ended when the watch was opened.
  bool Gone() const { return gone_; }

  // How a wait for a redirect ended.
  enum class RedirectWait {
    // `done` was set: the redirect returned (the caller knows whether it
    // succeeded).
    Done,
    // The holder's process ended first.
    HolderGone,
    // The budget passed first.
    TimedOut,
    // The wait itself failed, with the HRESULT in `*failure`.
    Failed,
  };

  // Waits, pumping COM, for `done` (an event) or the holder's process to end,
  // at most `budget`.
  RedirectWait AwaitRedirect(void* done, std::chrono::milliseconds budget, long* failure) const;

  // The holder has raised its exiting signal, or its process has ended.
  bool Exiting() const;

  // Waits, pumping COM, at most `budget` for the holder's process to end; true
  // when it has.
  bool AwaitExit(std::chrono::milliseconds budget) const;

 private:
  void* process_ = nullptr;
  void* exitingSignal_ = nullptr;
  bool gone_ = false;
};

}  // namespace urnw
