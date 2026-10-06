// "Launch URnetwork on system startup" on Windows (owner decision,
// 2026-10-05: like the macOS setting, with its default).
//
// macOS. Settings has the toggle (launch_urnetwork_on_system_startup, under
// "System"). It shows whether the app is a login item that will run
// (SMAppService.mainApp.status == .enabled), registers the app when turned on
// and unregisters it when turned off, and goes back to the old value when that
// fails. Nothing registers the app by itself, so it starts off, and the user
// can also switch the login item off in System Settings, which the toggle then
// shows.
//
// Windows, the same way. The registration is the user's own Run value
// (HKCU\Software\Microsoft\Windows\CurrentVersion\Run, ids::kStartupRunValue),
// whose command line starts the app with instance::kAutostartArgument, so a
// sign-in shows only the tray icon (InstanceHandover.h ActionFor) and, during
// an update, exits without a word (UpdateMarker.h). Task Manager's Startup
// apps page can switch it off without removing it: it then records the value
// as disabled under Explorer\StartupApproved\Run, and the toggle shows it as
// off, as macOS shows a login item switched off in System Settings.
//   * Turning it on writes the Run value and clears a Task Manager "disabled",
//     since the user has just asked for it here.
//   * Turning it off deletes the Run value and Task Manager's record of it.
//   * Nothing registers it by itself (kDefaultEnabled): a first launch leaves
//     the registry alone. The one other writer, at each launch, only brings an
//     existing value up to date (this exe, --autostart), for an install that
//     moved or a value written without the argument; it never creates one and
//     leaves a Task Manager "disabled" as the user set it.
//
// Pure, header-only and free of Windows headers: tools/startup-registration-tests.cpp
// runs it against a fake registry on any host, and App/LaunchAtStartup.cpp
// binds the real one.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace urnw::startup {

// Off until the user turns it on, as on macOS.
inline constexpr bool kDefaultEnabled = false;

// The two registry values, as read.
struct Registration {
  // The Run value's command line, when there is one.
  std::optional<std::wstring> command;
  // The first byte of Task Manager's StartupApproved record of it, when there
  // is one.
  std::optional<std::uint8_t> approval;
};

// Task Manager has switched the registration off. It writes an odd first byte
// for disabled (0x03) and an even one for enabled (0x02, 0x06).
inline bool DisabledInTaskManager(const Registration& registration) {
  return registration.approval && (*registration.approval & 1) != 0;
}

// What the toggle shows: registered, and not switched off in Task Manager.
inline bool Enabled(const Registration& registration) {
  return registration.command.has_value() && !DisabledInTaskManager(registration);
}

// The command line the Run value holds: the exe, quoted, then `argument`.
inline std::wstring RunCommand(std::wstring_view exePath, std::wstring_view argument) {
  std::wstring command = L"\"";
  command.append(exePath);
  command.append(L"\" ");
  command.append(argument);
  return command;
}

// The registry writes that make the registration what is wanted.
struct Plan {
  // Write the Run value with this command line.
  std::optional<std::wstring> writeCommand;
  // Delete the Run value.
  bool deleteCommand = false;
  // Delete Task Manager's record of it.
  bool deleteApproval = false;

  bool Empty() const { return !writeCommand && !deleteCommand && !deleteApproval; }
};

// The writes for the toggle turned on or off.
inline Plan PlanSet(bool enable, const Registration& registration, std::wstring_view command) {
  Plan plan;
  if (enable) {
    if (registration.command != command) plan.writeCommand = std::wstring(command);
    plan.deleteApproval = DisabledInTaskManager(registration);
  } else {
    plan.deleteCommand = registration.command.has_value();
    plan.deleteApproval = registration.approval.has_value();
  }
  return plan;
}

// The writes at a launch: an existing Run value is brought up to date, and
// nothing else is touched.
inline Plan PlanRefresh(const Registration& registration, std::wstring_view command) {
  Plan plan;
  if (registration.command && *registration.command != command) {
    plan.writeCommand = std::wstring(command);
  }
  return plan;
}

}  // namespace urnw::startup
