// SPDX-License-Identifier: MPL-2.0
// Built without the precompiled header (App.vcxproj): plain Win32 and COM, no
// WinRT.
#include "SingleInstance.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>  // CoWaitForMultipleObjects

#include <format>
#include <string>

#include "Ids.h"
#include "Log.h"

namespace urnw {
namespace {

// This process's exiting signal: null until CreateExitingSignal, and when it
// could not be created. Written once, on the launch thread, before the App
// SDK can raise anything that reads it.
HANDLE g_exitingSignal = nullptr;

// The one name the instance creates and a launch opens.
std::wstring ExitingSignalName(std::uint32_t processId) {
  return std::format(L"{}{}", ids::kExitingSignalPrefix, processId);
}

// Whether a waitable handle is signalled now.
bool Signalled(void* handle) {
  return handle && ::WaitForSingleObject(static_cast<HANDLE>(handle), 0) == WAIT_OBJECT_0;
}

// Waits, pumping COM, for any of `handles`, at most `budget`.
HRESULT AwaitAny(HANDLE* handles, ULONG count, std::chrono::milliseconds budget, DWORD* index) {
  return ::CoWaitForMultipleObjects(CWMO_DEFAULT, static_cast<DWORD>(budget.count()), count,
                                    handles, index);
}

}  // namespace

void CreateExitingSignal() {
  const std::wstring name = ExitingSignalName(::GetCurrentProcessId());
  g_exitingSignal = ::CreateEventW(nullptr, /*bManualReset=*/TRUE, /*bInitialState=*/FALSE,
                                   name.c_str());
  if (!g_exitingSignal) {
    LogWarn("startup: single instance: no exiting signal (CreateEvent failed: {}); a launch "
            "during this instance's exit waits for its process instead",
            ::GetLastError());
    return;
  }
  // An event of this name outlives an earlier process with this id while a
  // launch that watched that process still holds it, raised. This instance is
  // not exiting.
  if (::GetLastError() == ERROR_ALREADY_EXISTS) ::ResetEvent(g_exitingSignal);
}

instance::ActivationGate& Activations() {
  // Never destroyed: a threadpool callback may still be inside Take when
  // static destruction runs.
  static instance::ActivationGate* const gate = new instance::ActivationGate([] {
    if (g_exitingSignal) ::SetEvent(g_exitingSignal);
  });
  return *gate;
}

void BeginExiting() {
  if (!Activations().Closed()) {
    LogInfo("app: single instance: exiting; a launch from here on starts the app once this "
            "process has ended");
  }
  Activations().Close();
}

bool LaunchedByAutostart() {
  return instance::HasArgument(::GetCommandLineW(), instance::kAutostartArgument);
}

HolderWatch::HolderWatch(std::uint32_t processId) {
  process_ = ::OpenProcess(SYNCHRONIZE, FALSE, processId);
  // no process has this id any more: the holder ended after the App SDK found it
  if (!process_) gone_ = ::GetLastError() == ERROR_INVALID_PARAMETER;
  exitingSignal_ = ::OpenEventW(SYNCHRONIZE, FALSE, ExitingSignalName(processId).c_str());
}

HolderWatch::~HolderWatch() {
  if (process_) ::CloseHandle(static_cast<HANDLE>(process_));
  if (exitingSignal_) ::CloseHandle(static_cast<HANDLE>(exitingSignal_));
}

HolderWatch::RedirectWait HolderWatch::AwaitRedirect(void* done, std::chrono::milliseconds budget,
                                                     long* failure) const {
  HANDLE handles[] = {static_cast<HANDLE>(done), static_cast<HANDLE>(process_)};
  DWORD index = 0;
  const HRESULT hr = AwaitAny(handles, process_ ? 2 : 1, budget, &index);
  if (SUCCEEDED(hr)) return index == 0 ? RedirectWait::Done : RedirectWait::HolderGone;
  if (hr == RPC_S_CALLPENDING) return RedirectWait::TimedOut;
  if (failure) *failure = hr;
  return RedirectWait::Failed;
}

bool HolderWatch::Exiting() const { return Signalled(exitingSignal_) || Signalled(process_); }

bool HolderWatch::AwaitExit(std::chrono::milliseconds budget) const {
  // Without a handle there is nothing to wait on: true only when it has gone.
  if (!process_) return gone_;
  HANDLE handles[] = {static_cast<HANDLE>(process_)};
  DWORD index = 0;
  return SUCCEEDED(AwaitAny(handles, 1, budget, &index));
}

}  // namespace urnw
