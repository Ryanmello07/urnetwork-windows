// SPDX-License-Identifier: MPL-2.0
// Built without the precompiled header (App.vcxproj): plain Win32, no WinRT.
#include "LaunchAtStartup.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Ids.h"
#include "InstanceHandover.h"
#include "Log.h"
#include "StartupRegistration.h"

#pragma comment(lib, "advapi32.lib")

namespace urnw {
namespace {

// The user's Run value, when there is one.
std::optional<std::wstring> ReadRunCommand() {
  DWORD bytes = 0;
  const DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
  if (::RegGetValueW(HKEY_CURRENT_USER, ids::kStartupRunKey, ids::kStartupRunValue, flags,
                     nullptr, nullptr, &bytes) != ERROR_SUCCESS) {
    return std::nullopt;
  }
  std::wstring command(bytes / sizeof(wchar_t) + 1, L'\0');
  bytes = static_cast<DWORD>(command.size() * sizeof(wchar_t));
  if (::RegGetValueW(HKEY_CURRENT_USER, ids::kStartupRunKey, ids::kStartupRunValue, flags,
                     nullptr, command.data(), &bytes) != ERROR_SUCCESS) {
    return std::nullopt;
  }
  command.resize(bytes / sizeof(wchar_t));
  while (!command.empty() && command.back() == L'\0') command.pop_back();
  return command;
}

// The first byte of Task Manager's record of the Run value, when there is one.
std::optional<std::uint8_t> ReadApproval() {
  std::uint8_t data[12] = {};
  DWORD bytes = sizeof(data);
  const LSTATUS status = ::RegGetValueW(HKEY_CURRENT_USER, ids::kStartupApprovedKey,
                                        ids::kStartupRunValue, RRF_RT_REG_BINARY, nullptr, data,
                                        &bytes);
  if ((status != ERROR_SUCCESS && status != ERROR_MORE_DATA) || bytes == 0) return std::nullopt;
  return data[0];
}

startup::Registration Read() {
  return startup::Registration{.command = ReadRunCommand(), .approval = ReadApproval()};
}

// This exe's full path, or empty.
std::wstring ExePath() {
  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(),
                                              static_cast<DWORD>(buffer.size()));
    if (length == 0) return {};
    if (length < buffer.size()) return std::wstring(buffer.data(), length);
    buffer.resize(buffer.size() * 2);
  }
}

// The Run value's command line for this install: an autostart, so a sign-in
// shows only the tray icon (InstanceHandover.h ActionFor).
std::wstring Command() {
  const std::wstring exe = ExePath();
  return exe.empty() ? std::wstring() : startup::RunCommand(exe, instance::kAutostartArgument);
}

// Applies the plan; true when every write succeeded.
bool Apply(const startup::Plan& plan) {
  bool ok = true;
  if (plan.writeCommand) {
    const std::wstring& command = *plan.writeCommand;
    ok = ::RegSetKeyValueW(HKEY_CURRENT_USER, ids::kStartupRunKey, ids::kStartupRunValue, REG_SZ,
                           command.c_str(),
                           static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) ==
             ERROR_SUCCESS &&
         ok;
  }
  const auto deleteValue = [](const wchar_t* key) {
    const LSTATUS status = ::RegDeleteKeyValueW(HKEY_CURRENT_USER, key, ids::kStartupRunValue);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
  };
  if (plan.deleteCommand) ok = deleteValue(ids::kStartupRunKey) && ok;
  if (plan.deleteApproval) ok = deleteValue(ids::kStartupApprovedKey) && ok;
  return ok;
}

}  // namespace

bool LaunchAtStartupEnabled() { return startup::Enabled(Read()); }

bool SetLaunchAtStartup(bool enabled) {
  const std::wstring command = Command();
  if (enabled && command.empty()) {
    LogWarn("settings: launch on system startup not registered: this exe's path is unknown ({})",
            ::GetLastError());
    return false;
  }
  const bool applied = Apply(startup::PlanSet(enabled, Read(), command));
  const bool actual = LaunchAtStartupEnabled();
  LogInfo("settings: launch on system startup {} ({})", actual ? "on" : "off",
          applied && actual == enabled ? "as asked" : "the registry did not take the change");
  return applied && actual == enabled;
}

void RefreshLaunchAtStartup() {
  const std::wstring command = Command();
  if (command.empty()) return;
  const startup::Plan plan = startup::PlanRefresh(Read(), command);
  if (plan.Empty()) return;
  if (Apply(plan)) {
    LogInfo("settings: launch on system startup now starts this install with --autostart");
  } else {
    LogWarn("settings: launch on system startup could not be brought up to date");
  }
}

}  // namespace urnw
