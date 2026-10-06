// URnetworkUpdate.exe: the elevated update helper, and the installer's
// relaunch of the app once an update is in place. Its arguments pick the job:
//
//   --apply-update <tag>   The elevated helper (ApplyUpdate.h). The tray app
//                          starts it with "runas" from the admin-only install
//                          folder, and it fetches, checks and installs the
//                          release itself.
//   (none)                 The relaunch. After an update the helper ran, the
//                          MSI starts this unelevated, with no arguments
//                          (WixUnelevatedShellExec passes none), once its files
//                          are in place (installer/Package.wxs). It starts
//                          URnetwork.exe beside it with --after-update, which
//                          waits for the update to finish instead of being
//                          refused by it (Common/UpdateMarker.h), and ends.
// Anything else exits with Refusal::BadArguments.
//
// Its own small program rather than a mode of URnetwork.exe: URnetwork.exe
// loads URnetworkSdk.dll (whose Go runtime starts as it loads), the VC++
// runtime and the Windows App Runtime from its folder before its entry point
// runs, so a mode of it would start that code elevated, and would hold those
// files open while msiexec replaces them. This one links the C runtime
// statically and imports System32 libraries only, searched in System32 alone
// (Updater.vcxproj /DEPENDENTLOADFLAG).
//
// SPDX-License-Identifier: MPL-2.0
#include <windows.h>
#include <shellapi.h>

#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "ApplyUpdate.h"
#include "InstallLocationWin32.h"
#include "UpdateResult.h"

namespace {

namespace fs = std::filesystem;

// Drops every URNETWORK_* variable from this process's environment, so
// nothing here, and nothing it starts, can read one: those overrides belong
// to the user's app and say nothing to an elevated process.
void DropAppOverrides() {
  wchar_t* block = ::GetEnvironmentStringsW();
  if (!block) return;
  std::vector<std::wstring> names;
  for (const wchar_t* entry = block; *entry; entry += std::wcslen(entry) + 1) {
    const std::wstring_view text(entry);
    const std::size_t equals = text.find(L'=', 1);
    if (equals == std::wstring_view::npos) continue;
    std::wstring name(text.substr(0, equals));
    std::wstring upper = name;
    for (wchar_t& c : upper) c = static_cast<wchar_t>(std::towupper(c));
    if (upper.rfind(L"URNETWORK_", 0) == 0) names.push_back(std::move(name));
  }
  ::FreeEnvironmentStringsW(block);
  for (const std::wstring& name : names) ::SetEnvironmentVariableW(name.c_str(), nullptr);
}

// The elevated half of a split token: what UAC's consent produces. The
// relaunch never starts the app with it.
bool IsFullyElevated() {
  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return true;
  TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
  DWORD size = 0;
  const BOOL read = ::GetTokenInformation(token, TokenElevationType, &type, sizeof(type), &size);
  ::CloseHandle(token);
  return !read || type == TokenElevationTypeFull;
}

// The relaunch: URnetwork.exe --after-update from this folder.
int RelaunchApp() {
  if (IsFullyElevated()) return 0;
  const fs::path folder = urnw::install::OwnExecutablePath().parent_path();
  if (folder.empty()) return 0;
  const fs::path app = folder / L"URnetwork.exe";
  std::wstring command = L"\"" + app.native() + L"\" --after-update";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (::CreateProcessW(app.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                       folder.c_str(), &startup, &process)) {
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
  }
  return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  // Every library loaded from here on comes from System32 or this folder.
  ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
  DropAppOverrides();

  int argc = 0;
  wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  if (!argv) return static_cast<int>(urnw::update::Refusal::BadArguments);
  std::vector<std::wstring> args(argv, argv + argc);
  ::LocalFree(argv);

  if (args.size() == 3 && args[1] == L"--apply-update") return urnw::updater::ApplyUpdate(args[2]);
  if (args.size() == 1) return RelaunchApp();
  return static_cast<int>(urnw::update::Refusal::BadArguments);
}
