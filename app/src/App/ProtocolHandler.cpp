// SPDX-License-Identifier: MPL-2.0
// Built without the precompiled header (App.vcxproj): plain Win32, no WinRT.
#include "ProtocolHandler.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlwapi.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Log.h"
#include "ProtocolRegistration.h"
#include "Strings.h"
#include "Version.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace urnw {
namespace {

// Another file named URnetwork.exe in the MSI's shape is this app too: the owner's
// development machine has exactly such a key (unmarked, an earlier build's path), and a
// key like that has nothing to protect. Anything else that starts something else is left
// alone, and the attempt falls back to pasting a code.
constexpr protocol::Policy kPolicy{.adoptOtherUrnetworkExe = true};

// This exe's full path, or empty.
std::wstring ExePath() {
  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) return {};
    if (length < buffer.size()) return std::wstring(buffer.data(), length);
    buffer.resize(buffer.size() * 2);
  }
}

// The key under the user's classes, for a write's subkey ("" is the key itself).
std::wstring KeyPath(std::wstring_view subkey) {
  std::wstring path(protocol::kClassesKey);
  if (!subkey.empty()) {
    path.push_back(L'\\');
    path.append(subkey);
  }
  return path;
}

// Nothing there is not the same as not being able to look: only a key or a value that is not
// there is "absent", and everything else the registry can answer (denied, a value of another
// type, a failing hive) is "unreadable", which the caller must not mistake for an empty key.
bool IsAbsent(LONG status) { return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND; }

// A string value of the user's key; `name` null is the default value.
struct Text {
  bool readable = true;               // false: the registry would not say
  std::optional<std::wstring> value;  // empty: no such value
};

Text ReadString(std::wstring_view subkey, const wchar_t* name) {
  Text text;
  const std::wstring path = KeyPath(subkey);
  const DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
  DWORD bytes = 0;
  const LONG sized = ::RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name, flags, nullptr, nullptr, &bytes);
  if (IsAbsent(sized)) return text;
  if (sized != ERROR_SUCCESS) {
    text.readable = false;
    return text;
  }
  std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
  bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
  const LONG read = ::RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name, flags, nullptr, value.data(), &bytes);
  if (IsAbsent(read)) return text;
  if (read != ERROR_SUCCESS) {
    text.readable = false;
    return text;
  }
  value.resize(bytes / sizeof(wchar_t));
  while (!value.empty() && value.back() == L'\0') value.pop_back();
  text.value = std::move(value);
  return text;
}

// Whether the user's key has a value of that name, of any type: 1 it does, 0 it does not, -1 the
// registry would not say.
int HasValue(std::wstring_view subkey, const wchar_t* name) {
  const std::wstring path = KeyPath(subkey);
  const LONG found = ::RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name, RRF_RT_ANY | RRF_NOEXPAND, nullptr, nullptr, nullptr);
  if (found == ERROR_SUCCESS) return 1;
  return IsAbsent(found) ? 0 : -1;
}

// The user's key as read, or nothing when the registry would not say (the caller writes
// nothing it could not look at first). A key that is not there is a key that does not exist.
std::optional<protocol::UserKey> ReadUserKey() {
  protocol::UserKey key;
  HKEY handle = nullptr;
  const std::wstring path = KeyPath(L"");
  const LONG opened = ::RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &handle);
  if (IsAbsent(opened)) return key;
  if (opened != ERROR_SUCCESS) return std::nullopt;
  ::RegCloseKey(handle);
  key.exists = true;
  const int urlProtocol = HasValue(L"", std::wstring(protocol::kUrlProtocolValue).c_str());
  const int delegated = HasValue(protocol::kCommandSubkey, L"DelegateExecute");
  const Text description = ReadString(L"", nullptr);
  const Text owner = ReadString(L"", std::wstring(protocol::kOwnerValue).c_str());
  const Text command = ReadString(protocol::kCommandSubkey, nullptr);
  if (urlProtocol < 0 || delegated < 0 || !description.readable || !owner.readable || !command.readable) {
    return std::nullopt;
  }
  key.urlProtocol = urlProtocol == 1;
  key.delegateExecute = delegated == 1;
  key.description = description.value;
  key.owner = owner.value;
  key.command = command.value;
  return key;
}

// Applies the plan; true when every write reached the registry. The writes stop at the
// first that fails, and the marker is the first of them, so what is left is still ours.
bool Apply(const protocol::Plan& plan) {
  for (const protocol::Write& write : plan.writes) {
    const std::wstring path = KeyPath(write.subkey);
    const LONG written = ::RegSetKeyValueW(HKEY_CURRENT_USER, path.c_str(), write.name.empty() ? nullptr : write.name.c_str(),
                                           REG_SZ, write.value.c_str(),
                                           static_cast<DWORD>((write.value.size() + 1) * sizeof(wchar_t)));
    if (written != ERROR_SUCCESS) return false;
  }
  return true;
}

// What the shell runs for urnetwork://, which is what a browser's launch gets: the
// command of the "open" verb of the scheme, merged from the user's key and the machine's.
std::optional<std::wstring> ShellCommand() {
  DWORD chars = 0;
  const HRESULT sized = ::AssocQueryStringW(ASSOCF_IS_PROTOCOL, ASSOCSTR_COMMAND,
                                            std::wstring(protocol::kScheme).c_str(), L"open", nullptr, &chars);
  if (FAILED(sized) || chars == 0) return std::nullopt;
  std::wstring command(chars, L'\0');
  if (FAILED(::AssocQueryStringW(ASSOCF_IS_PROTOCOL, ASSOCSTR_COMMAND, std::wstring(protocol::kScheme).c_str(),
                                 L"open", command.data(), &chars))) {
    return std::nullopt;
  }
  while (!command.empty() && command.back() == L'\0') command.pop_back();
  return command;
}

bool ShellStartsThisExe(const std::wstring& exe) {
  const std::optional<std::wstring> command = ShellCommand();
  return command && protocol::EqualsIgnoreCase(*command, protocol::OpenCommand(exe));
}

}  // namespace

bool EnsureProtocolHandler() {
  const std::wstring exe = ExePath();
  const std::optional<protocol::UserKey> key = ReadUserKey();
  if (exe.empty() || !key) {
    LogWarn("protocol: the urnetwork:// handler could not be looked at");
    return false;
  }
  const protocol::Plan plan = protocol::PlanEnsure(*key, exe, Widen(version::kString), kPolicy);
  LogInfo("protocol: the urnetwork:// handler: {}", protocol::ToString(plan.decision));
  if (plan.decision == protocol::Decision::RefuseForeign || plan.decision == protocol::Decision::RefuseUnsafePath) {
    return false;
  }
  if (!Apply(plan)) {
    LogWarn("protocol: the urnetwork:// handler could not be written");
    return false;
  }
  // the shell's answer is the one that counts: a machine key, a policy or a shadowing key
  // elsewhere would leave the link going to some other program
  const bool ours = ShellStartsThisExe(exe);
  LogInfo("protocol: a browser launch of urnetwork:// {} this exe", ours ? "starts" : "does not start");
  return ours;
}

void RefreshProtocolHandler() {
  const std::wstring exe = ExePath();
  const std::optional<protocol::UserKey> key = ReadUserKey();
  if (exe.empty() || !key) return;
  const protocol::Plan plan = protocol::PlanRefresh(*key, exe, Widen(version::kString));
  if (plan.decision == protocol::Decision::RefuseUnsafePath) {
    LogWarn("protocol: the urnetwork:// handler is not brought up to date: this exe's path is unsafe");
    return;
  }
  if (plan.writes.empty()) return;
  if (Apply(plan)) {
    LogInfo("protocol: the urnetwork:// handler: {}", protocol::ToString(plan.decision));
  } else {
    LogWarn("protocol: the urnetwork:// handler could not be brought up to date");
  }
}

ProtocolHandlerState ProtocolHandlerStatus() {
  const std::wstring exe = ExePath();
  const std::optional<protocol::UserKey> key = ReadUserKey();
  if (exe.empty() || !key) return ProtocolHandlerState::Failed;
  // what an attempt's start would do with the key, without doing it
  const protocol::Plan plan = protocol::PlanEnsure(*key, exe, Widen(version::kString), kPolicy);
  switch (plan.decision) {
    case protocol::Decision::RefuseUnsafePath:
      return ProtocolHandlerState::Unsafe;
    case protocol::Decision::RefuseForeign:
      return ProtocolHandlerState::Foreign;
    case protocol::Decision::Create:
    case protocol::Decision::Absent:
      return ProtocolHandlerState::Absent;
    case protocol::Decision::Repoint:
    case protocol::Decision::Adopt:
      return ProtocolHandlerState::Ours;
    case protocol::Decision::Current:
      // nothing to write, so the shell's answer is all there is to go by: a key of ours that a
      // policy or a machine key shadows still sends the link to another program
      return ShellStartsThisExe(exe) ? ProtocolHandlerState::Ours : ProtocolHandlerState::Foreign;
  }
  return ProtocolHandlerState::Failed;
}

}  // namespace urnw
