// The per-user registration of the urnetwork:// scheme, decided as data.
//
// A browser sign-in comes back to the app as a urnetwork:// link, and a link only
// reaches this app if Windows has a handler for the scheme that starts it. The
// MSI registers one for the machine (installer/Package.wxs: HKLM, the quoted
// URnetwork.exe, then a quoted "%1"); a portable copy and a development build
// register nothing. So an attempt that is about to open the browser makes sure
// the current user has one that starts THIS exe, under HKEY_CURRENT_USER only
// (a per-user key shadows the machine's for that user and needs no elevation).
//
// The rules, the same as for "Launch URnetwork on system startup"
// (StartupRegistration.h):
//   * A key this app wrote carries a marker value (UrnwOwner: the exe and the
//     version it was written for). The marker is the FIRST write, so a key that
//     is half written, or written by an older version, is still known to be ours
//     and is completed, never taken for someone else's.
//   * Only a key we marked is refreshed (it follows an exe that moved or updated),
//     and a refresh never creates one: that is for an attempt to do, when the person
//     clicks. Nothing here removes a key: the zip's README says how to by hand.
//   * A key we did not write is left alone unless it is ours in all but the
//     marker: it already starts THIS exe the way the MSI's does (adopted: marked,
//     and from then on followed), it names no handler at all (a hollow key,
//     filled in), or the policy says a URnetwork.exe elsewhere on the machine is
//     the same app (the owner's development machine has exactly such a key:
//     unmarked, the MSI's shape, another path). Anything else that starts
//     something else is refused, and the caller falls back to pasting a code.
//   * The command is the MSI's: "<exe>" "%1". Both are quoted, because the link
//     is cut out of the command line at the first space, tab or quote
//     (AuthLink.h LinkInCommandLine) and a path with spaces must stay one argument.
//   * An exe path that cannot be put in that quoted command safely is refused
//     (SafeExePath).
//   * A command that Windows runs through a COM delegate (a DelegateExecute value
//     beside it) is not one a command line can rewrite, so a key that has one is
//     someone else's whoever wrote the rest of it.
//
// Pure, header-only and free of Windows headers: tools/protocol-registration-tests.cpp
// runs it against a fake registry on any host, and App/ProtocolHandler.cpp binds
// the real one.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace urnw::protocol {

// The scheme (Ids.h kUriScheme) and where its user key is under HKEY_CURRENT_USER.
inline constexpr std::wstring_view kScheme = L"urnetwork";
inline constexpr std::wstring_view kClassesKey = L"Software\\Classes\\urnetwork";
// The MSI's values (Package.wxs): the description, and the empty "URL Protocol" value.
inline constexpr std::wstring_view kDescription = L"URL:URnetwork Protocol";
inline constexpr std::wstring_view kUrlProtocolValue = L"URL Protocol";
// The marker: says this app wrote the key, for which exe and which version.
inline constexpr std::wstring_view kOwnerValue = L"UrnwOwner";
// The subkey whose default value is the command (under kClassesKey).
inline constexpr std::wstring_view kCommandSubkey = L"shell\\open\\command";
// The file name the MSI and every build give the app.
inline constexpr std::wstring_view kExeName = L"URnetwork.exe";

// The user's own key, as read.
struct UserKey {
  bool exists = false;
  std::optional<std::wstring> description;  // the key's default value
  bool urlProtocol = false;                 // the "URL Protocol" value is there
  std::optional<std::wstring> command;      // shell\open\command, default value
  std::optional<std::wstring> owner;        // the marker, when this app wrote it
  bool delegateExecute = false;             // shell\open\command has a DelegateExecute value
};

enum class Decision {
  Absent,            // no key of ours or anyone's: nothing to refresh or remove
  Current,           // ours and up to date: nothing to write
  Create,            // no key: write it
  Repoint,           // ours, stale (the exe moved, a new version, a missing value): write it again
  Adopt,             // not marked, but ours in all but the marker: write it again, marker first
  RefuseForeign,     // someone else's: leave it, the paste fallback stays
  RefuseUnsafePath,  // this exe's path cannot be put in a quoted command
};

// The tokens for the log: closed, lowercase and hyphenated or plain words that are
// not the id of a string resource.
constexpr std::string_view ToString(Decision decision) {
  switch (decision) {
    case Decision::Absent: return "absent";
    case Decision::Current: return "unchanged";
    case Decision::Create: return "created";
    case Decision::Repoint: return "repointed";
    case Decision::Adopt: return "adopted";
    case Decision::RefuseForeign: return "refused-foreign";
    case Decision::RefuseUnsafePath: return "unsafe-path";
  }
  return "unknown-decision";
}

struct Write {
  std::wstring subkey;  // under kClassesKey; empty is the key itself
  std::wstring name;    // empty is the (Default) value
  std::wstring value;   // a REG_SZ
};

struct Plan {
  Decision decision = Decision::Absent;
  std::vector<Write> writes;  // in this order: the marker first, the command last
};

struct Policy {
  // Treat an unmarked key whose command is the MSI's shape around ANOTHER file named
  // URnetwork.exe as ours. Off unless the caller turns it on: a plain library user
  // must not take over a key by its file name.
  bool adoptOtherUrnetworkExe = false;
};

// Package.wxs: "<exe>" "%1" - the quoted exe path, a space, a quoted %1.
inline std::wstring OpenCommand(std::wstring_view exe) {
  std::wstring command = L"\"";
  command.append(exe);
  command.append(L"\" \"%1\"");
  return command;
}

// What the marker holds: the exe and the version it was written for.
inline std::wstring OwnerTag(std::wstring_view exe, std::wstring_view version) {
  std::wstring tag(exe);
  tag.append(L"|");
  tag.append(version);
  return tag;
}

inline wchar_t Lower(wchar_t c) { return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c; }

// Windows paths compare without regard to case.
inline bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (Lower(a[i]) != Lower(b[i])) return false;
  }
  return true;
}

// An absolute drive path to an .exe with nothing in it that could end the quoted
// argument, add one, name a stream or a parent: no quote, '%', '<', '>', '|', '*',
// '?', control character, ':' after the drive, or "..". A UNC path and a relative one are
// refused too: a link handler that starts a program from another machine, or from
// wherever the shell happens to be, is not one to register.
inline bool SafeExePath(std::wstring_view exe) {
  if (exe.size() < 8 || exe.size() > 32000) return false;
  const bool drive = ((exe[0] >= L'A' && exe[0] <= L'Z') || (exe[0] >= L'a' && exe[0] <= L'z')) && exe[1] == L':' &&
                     exe[2] == L'\\';
  if (!drive) return false;
  for (std::size_t i = 0; i < exe.size(); ++i) {
    const wchar_t c = exe[i];
    if (c < 0x20 || c == L'"' || c == L'%' || c == L'<' || c == L'>' || c == L'|' || c == L'*' || c == L'?') return false;
    if (c == L':' && i != 1) return false;
  }
  if (exe.find(L"\\..\\") != std::wstring_view::npos || exe.find(L"\\.\\") != std::wstring_view::npos) return false;
  if (!EqualsIgnoreCase(exe.substr(exe.size() - 4), L".exe")) return false;
  const std::size_t slash = exe.rfind(L'\\');
  return exe.size() - slash - 1 > 4;  // a file name, not just the extension
}

// The exe of a command in the MSI's shape ("<exe>" "%1"), or nothing for any other.
inline std::optional<std::wstring> ExeOfCommand(std::wstring_view command) {
  constexpr std::wstring_view kTail = L"\" \"%1\"";
  if (command.size() <= kTail.size() + 1 || command.front() != L'"') return std::nullopt;
  if (command.substr(command.size() - kTail.size()) != kTail) return std::nullopt;
  const std::wstring_view exe = command.substr(1, command.size() - 1 - kTail.size());
  if (exe.find(L'"') != std::wstring_view::npos) return std::nullopt;
  return std::wstring(exe);
}

// The MSI's shape around a safe path to a file named URnetwork.exe, wherever it is.
inline bool NamesUrnetworkExe(std::wstring_view command) {
  const std::optional<std::wstring> exe = ExeOfCommand(command);
  if (!exe || !SafeExePath(*exe)) return false;
  const std::size_t slash = exe->rfind(L'\\');
  return EqualsIgnoreCase(std::wstring_view(*exe).substr(slash + 1), kExeName);
}

// The four values that make the key work, in the order they are written: the marker
// first, so whatever follows a power cut is already known to be ours, and the
// command last, because the scheme starts nothing until it is there.
inline std::vector<Write> AllWrites(std::wstring_view exe, std::wstring_view version) {
  return {
      {L"", std::wstring(kOwnerValue), OwnerTag(exe, version)},
      {L"", L"", std::wstring(kDescription)},
      {L"", std::wstring(kUrlProtocolValue), L""},
      {std::wstring(kCommandSubkey), L"", OpenCommand(exe)},
  };
}

// The writes that make the user's key start `exe`, for a launch that needs the
// scheme (an attempt starts, before the browser opens).
inline Plan PlanEnsure(const UserKey& key, std::wstring_view exe, std::wstring_view version,
                       const Policy& policy = {}) {
  Plan plan;
  if (!SafeExePath(exe)) {
    plan.decision = Decision::RefuseUnsafePath;
    return plan;
  }
  const std::wstring command = OpenCommand(exe);
  if (!key.exists) {
    plan.decision = Decision::Create;
    plan.writes = AllWrites(exe, version);
    return plan;
  }
  if (key.delegateExecute) {
    plan.decision = Decision::RefuseForeign;
    return plan;
  }
  if (key.owner) {
    const bool current = key.command && EqualsIgnoreCase(*key.command, command) && key.description &&
                         *key.description == kDescription && key.urlProtocol &&
                         *key.owner == OwnerTag(exe, version);
    if (current) {
      plan.decision = Decision::Current;
      return plan;
    }
    plan.decision = Decision::Repoint;
    plan.writes = AllWrites(exe, version);
    return plan;
  }
  const bool hollow = !key.command.has_value();
  const bool launchesThisExe = key.command && EqualsIgnoreCase(*key.command, command);
  const bool launchesAnotherUrnetwork = policy.adoptOtherUrnetworkExe && key.command && NamesUrnetworkExe(*key.command);
  if (hollow || launchesThisExe || launchesAnotherUrnetwork) {
    plan.decision = Decision::Adopt;
    plan.writes = AllWrites(exe, version);
    return plan;
  }
  plan.decision = Decision::RefuseForeign;
  return plan;
}

// At each launch: a key this app wrote follows the exe (it moved, it updated).
// Nothing is created, and a key we did not write is left exactly as it is, even one
// an attempt would adopt.
inline Plan PlanRefresh(const UserKey& key, std::wstring_view exe, std::wstring_view version) {
  Plan plan;
  if (!key.exists) return plan;
  if (!key.owner) {
    plan.decision = Decision::RefuseForeign;
    return plan;
  }
  return PlanEnsure(key, exe, version);
}

}  // namespace urnw::protocol
