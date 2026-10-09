// SPDX-License-Identifier: MPL-2.0
// Built without the precompiled header (App.vcxproj): plain Win32 and COM, no
// WinRT.
#include "SingleInstance.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>  // CoWaitForMultipleObjects

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include "Ids.h"
#include "Log.h"
#include "Paths.h"

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

// A process's creation time as one number, or 0 when it cannot be read.
std::uint64_t CreationTime(HANDLE process) {
  FILETIME created{};
  FILETIME exited{};
  FILETIME kernel{};
  FILETIME user{};
  if (!::GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
  ULARGE_INTEGER time{};
  time.LowPart = created.dwLowDateTime;
  time.HighPart = created.dwHighDateTime;
  return time.QuadPart;
}

// How long the updating notice stays up unless dismissed.
constexpr DWORD kUpdatingNoticeMs = 8000;

// Closes the updating notice's box, the thread's one dialog
// (EnumThreadWindows' callback); COM's hidden window on the thread is left
// alone.
BOOL CALLBACK CloseNoticeBox(HWND window, LPARAM) {
  wchar_t name[16] = {};
  if (::GetClassNameW(window, name, 16) && std::wstring_view(name) == L"#32770") {
    ::PostMessageW(window, WM_CLOSE, 0, 0);
  }
  return TRUE;
}

// Seconds since the Unix epoch.
std::int64_t NowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// What became of the installer a marker names.
update::InstallerState ProbeInstaller(const update::UpdateMarker& marker) {
  HANDLE process = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 marker.installerProcessId);
  if (!process) {
    // no process has this id: the installer has ended
    return ::GetLastError() == ERROR_INVALID_PARAMETER ? update::InstallerState::Ended
                                                       : update::InstallerState::Unknown;
  }
  update::InstallerState state = update::InstallerState::Unknown;
  if (const std::uint64_t created = CreationTime(process); created != 0) {
    if (created != marker.installerCreationTime) {
      state = update::InstallerState::Ended;  // the id names another process now
    } else {
      state = ::WaitForSingleObject(process, 0) == WAIT_OBJECT_0
                  ? update::InstallerState::Ended
                  : update::InstallerState::Running;
    }
  }
  ::CloseHandle(process);
  return state;
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

void RecordUpdateInProgress(void* installerProcess) {
  const auto process = static_cast<HANDLE>(installerProcess);
  const update::UpdateMarker marker{.installerProcessId = ::GetProcessId(process),
                                    .installerCreationTime = CreationTime(process),
                                    .writtenAt = NowSeconds()};
  if (marker.installerProcessId == 0 || marker.installerCreationTime == 0) {
    LogWarn("update: the installer's process could not be read ({}); launches during the "
            "update are not refused",
            ::GetLastError());
    return;
  }
  // Written beside it and renamed into place, so a launch reading it at the
  // same moment finds the whole marker or none, never a part it would delete
  // as unreadable.
  const std::filesystem::path path = UpdateInProgressFile();
  std::filesystem::path written = path;
  written += L".new";
  std::ofstream file(written, std::ios::binary | std::ios::trunc);
  file << update::FormatUpdateMarker(marker);
  file.close();
  std::error_code error;
  if (file) std::filesystem::rename(written, path, error);
  if (!file || error) {
    std::filesystem::remove(written, error);
    LogWarn("update: the update marker could not be written; launches during the update are "
            "not refused");
    return;
  }
  LogInfo("update: launches are refused until installer process {} ends",
          marker.installerProcessId);
}

bool LaunchedAfterUpdate() {
  return instance::HasArgument(::GetCommandLineW(), instance::kAfterUpdateArgument);
}

void AwaitUpdateEnd() {
  LogInfo("update: relaunched after an update; waiting for it to end");
  const bool ended = instance::AwaitUpdateEnd(
      [] { return UpdateInProgress(); },
      [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch());
      },
      [](std::chrono::milliseconds pause) { ::Sleep(static_cast<DWORD>(pause.count())); });
  if (!ended) {
    LogWarn("update: the update had not ended after {} ms; this launch asks as any other",
            instance::kAfterUpdateBudget.count());
  }
}

bool UpdateInProgress() {
  const std::filesystem::path path = UpdateInProgressFile();
  std::optional<std::string> text;
  if (std::ifstream file(path, std::ios::binary); file) {
    text.emplace(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }
  const update::Verdict verdict = update::Check(text, ProbeInstaller, NowSeconds());
  if (verdict == update::Verdict::Stale) {
    std::error_code error;
    std::filesystem::remove(path, error);
    LogInfo("update: removed a stale update marker{}", error ? " (it could not be deleted)" : "");
  } else if (verdict == update::Verdict::Updating) {
    LogInfo("update: {}", update::ToString(verdict));
  }
  return verdict == update::Verdict::Updating;
}

void ShowUpdatingNotice() {
  LogInfo("update: telling this launch that an update is installing");
  const DWORD thread = ::GetCurrentThreadId();
  HANDLE dismissed = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::thread closer;
  if (dismissed) {
    closer = std::thread([thread, dismissed] {
      if (::WaitForSingleObject(dismissed, kUpdatingNoticeMs) == WAIT_TIMEOUT) {
        ::EnumThreadWindows(thread, CloseNoticeBox, 0);
      }
    });
  }
  ::MessageBoxW(nullptr,
                L"URnetwork is updating.\n\n"
                L"Start it again once the update has finished.",
                L"URnetwork", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
  if (closer.joinable()) {
    ::SetEvent(dismissed);
    closer.join();
  }
  if (dismissed) ::CloseHandle(dismissed);
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
