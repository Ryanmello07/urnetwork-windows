// SPDX-License-Identifier: MPL-2.0
#include "Paths.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>

#include <cstring>
#include <format>
#include <limits>
#include <mutex>
#include <system_error>
#include <vector>

#include "Log.h"
#include "Strings.h"

#pragma comment(lib, "shell32.lib")

// WinBase.h defines these only for a _WIN32_WINNT that has them (Windows 10
// 1607 and later); the values are fixed.
#ifndef FILE_RENAME_FLAG_REPLACE_IF_EXISTS
#define FILE_RENAME_FLAG_REPLACE_IF_EXISTS 0x00000001
#endif
#ifndef FILE_RENAME_FLAG_POSIX_SEMANTICS
#define FILE_RENAME_FLAG_POSIX_SEMANTICS 0x00000002
#endif

namespace urnw {
namespace {

// app_prefs.json holds a handful of small keys; a file larger than this is not
// one this app wrote, and is read as empty rather than loaded.
constexpr LONGLONG kMaxPrefsBytes = 1024 * 1024;

// A save whose rename is blocked (ERROR_ACCESS_DENIED or
// ERROR_SHARING_VIOLATION while another handle on app_prefs.json lacks
// delete-sharing) retries this many times, this far apart: at most ~200 ms
// on the UI thread, then the save reports failure.
constexpr int kReplaceAttempts = 10;
constexpr DWORD kReplaceRetryMs = 20;

// Replace `target` with the file open as `source`, through that handle, with
// POSIX semantics: the rename succeeds while other handles on `target` share
// delete (LoadAppPrefs's do), and they keep reading the old bytes.
// MoveFileExW cannot do that: it fails with ERROR_ACCESS_DENIED while ANY
// handle on the target is open, delete-sharing or not (measured on Windows 11
// 10.0.26300, NTFS). A file system or Windows build without POSIX renames
// refuses the call as unsupported, and the caller falls back to MoveFileExW.
bool PosixReplace(HANDLE source, std::filesystem::path const& target) {
  const std::wstring name = target.wstring();
  std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO) +
                                    name.size() * sizeof(wchar_t));
  auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
  info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
  info->RootDirectory = nullptr;
  info->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
  std::memcpy(info->FileName, name.data(), info->FileNameLength);
  return ::SetFileInformationByHandle(source, FileRenameInfoEx, info,
                                     static_cast<DWORD>(buffer.size())) != FALSE;
}

bool RenameUnsupported(DWORD error) {
  return error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED ||
         error == ERROR_INVALID_FUNCTION;
}

struct FileHandle {
  HANDLE h = INVALID_HANDLE_VALUE;
  FileHandle() = default;
  explicit FileHandle(HANDLE handle) : h(handle) {}
  FileHandle(FileHandle const&) = delete;
  FileHandle& operator=(FileHandle const&) = delete;
  ~FileHandle() { Close(); }
  void Close() {
    if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
    h = INVALID_HANDLE_VALUE;
  }
};

std::filesystem::path KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR raw = nullptr;
  std::filesystem::path result;
  if (SUCCEEDED(::SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw))) {
    result = raw;
  }
  if (raw) ::CoTaskMemFree(raw);
  return result;
}

std::filesystem::path EnsureDir(std::filesystem::path p) {
  std::error_code ec;
  std::filesystem::create_directories(p, ec);
  return p;
}

}  // namespace

std::filesystem::path StorageRoot(bool isService) {
  // URNETWORK_APP_ROOT overrides the per-user app root. This exists because
  // several agents build and run this repo CONCURRENTLY from separate git
  // worktrees, and every one of them otherwise shares a single
  // %LOCALAPPDATA%\URnetwork\app: one SDK LocalState (the JWT and instance id),
  // one rpc_session.json and one log file, with two unsynchronised writers.
  // That is a state-corruption risk, not just noisy logs — and it silently
  // makes one agent's run appear in another agent's evidence.
  //
  // Point each worktree at its own root:
  //   $env:URNETWORK_APP_ROOT = 'C:\...\wt-p1\.localstate'
  //
  // Deliberately app-only. The service root is machine-wide by nature (it is
  // LocalSystem state and the control pipe is a single machine-wide instance),
  // so splitting it would give a false sense of isolation the service does not
  // actually have.
  if (!isService) {
    wchar_t buf[MAX_PATH];
    const DWORD n =
        ::GetEnvironmentVariableW(L"URNETWORK_APP_ROOT", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return EnsureDir(std::filesystem::path(buf, buf + n));
  }
  // FOLDERID_ProgramData -> C:\ProgramData (machine-wide, service)
  // FOLDERID_LocalAppData -> C:\Users\<u>\AppData\Local (per user, app)
  std::filesystem::path base =
      isService ? KnownFolder(FOLDERID_ProgramData)
                : KnownFolder(FOLDERID_LocalAppData);
  return EnsureDir(base / L"URnetwork" / (isService ? L"service" : L"app"));
}

std::filesystem::path SdkStorageDir(bool isService) {
  return EnsureDir(StorageRoot(isService) / L"storage");
}

std::filesystem::path LogDir(bool isService) {
  return EnsureDir(StorageRoot(isService) / L"logs");
}

std::filesystem::path RpcSessionFile() {
  return StorageRoot(/*isService=*/false) / L"rpc_session.json";
}

std::filesystem::path AppPrefsFile() {
  return StorageRoot(/*isService=*/false) / L"app_prefs.json";
}


nlohmann::json LoadAppPrefs() {
  try {
    // Every share flag, DELETE included: SaveAppPref replaces this file by
    // renaming over it, and a reader without delete-sharing blocks that
    // rename (ERROR_SHARING_VIOLATION) until it closes. (std::ifstream, the
    // reader this replaces, shares read and write only.)
    FileHandle file{::CreateFileW(
        AppPrefsFile().c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.h == INVALID_HANDLE_VALUE) return nlohmann::json::object();
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file.h, &size) || size.QuadPart < 0 ||
        size.QuadPart > kMaxPrefsBytes)
      return nlohmann::json::object();
    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD total = 0;
    while (total < bytes.size()) {
      DWORD read = 0;
      if (!::ReadFile(file.h, bytes.data() + total,
                      static_cast<DWORD>(bytes.size() - total), &read, nullptr))
        return nlohmann::json::object();
      if (read == 0) break;
      total += read;
    }
    bytes.resize(total);
    nlohmann::json j =
        nlohmann::json::parse(bytes, nullptr, /*allow_exceptions=*/false);
    if (j.is_object()) return j;
  } catch (...) {
  }
  return nlohmann::json::object();
}

bool SaveAppPref(const char* key, const nlohmann::json& value) {
  // One writer at a time in this process: every save is a whole-object
  // read-modify-write, and two interleaved ones would each drop the other's
  // key.
  static std::mutex writer;
  static unsigned long long sequence = 0;  // guarded by writer
  std::lock_guard lock(writer);

  std::filesystem::path temp;
  try {
    const std::filesystem::path target = AppPrefsFile();
    nlohmann::json prefs = LoadAppPrefs();
    prefs[key] = value;
    // replace, not throw, on a string value that is not valid UTF-8
    const std::string bytes =
        prefs.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);

    // A name unique to this process and this save, so two writers (two
    // processes over one root) never write into each other's temp file.
    // DELETE access lets the rename below go through this same handle.
    FileHandle file;
    for (int attempt = 0; attempt < 8 && file.h == INVALID_HANDLE_VALUE;
         ++attempt) {
      temp = target;
      temp += std::format(L".{}-{}.tmp", ::GetCurrentProcessId(), ++sequence);
      file.h = ::CreateFileW(temp.c_str(), GENERIC_WRITE | DELETE, 0, nullptr,
                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file.h == INVALID_HANDLE_VALUE && ::GetLastError() != ERROR_FILE_EXISTS)
        break;
    }
    if (file.h == INVALID_HANDLE_VALUE) {
      LogWarn("prefs: could not save '{}': creating {} failed ({})", key,
              Narrow(temp.wstring()), ::GetLastError());
      return false;
    }

    // On disk before the rename: without the flush, a power loss after the
    // rename can leave app_prefs.json empty or zeroed.
    DWORD written = 0;
    const bool flushed =
        ::WriteFile(file.h, bytes.data(), static_cast<DWORD>(bytes.size()),
                    &written, nullptr) &&
        written == bytes.size() && ::FlushFileBuffers(file.h);
    if (!flushed) {
      const DWORD writeError = ::GetLastError();
      file.Close();
      ::DeleteFileW(temp.c_str());
      LogWarn("prefs: could not save '{}': writing {} failed ({})", key,
              Narrow(temp.wstring()), writeError);
      return false;
    }

    // Replace app_prefs.json. First the POSIX rename through the temp's
    // handle, flushed again so the rename is on disk too. Where that is
    // unsupported, MoveFileExW with WRITE_THROUGH. Either one retries while
    // a handle without delete-sharing blocks it.
    bool posix = true;
    for (int attempt = 1;; ++attempt) {
      DWORD error = ERROR_SUCCESS;
      if (posix) {
        if (PosixReplace(file.h, target)) {
          ::FlushFileBuffers(file.h);
          return true;  // `file` now names app_prefs.json and closes here
        }
        error = ::GetLastError();
        if (RenameUnsupported(error)) {
          posix = false;
          file.Close();
          continue;
        }
      } else {
        if (::MoveFileExW(temp.c_str(), target.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
          return true;
        error = ::GetLastError();
      }
      const bool blocked =
          error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION;
      if (!blocked || attempt >= kReplaceAttempts) {
        file.Close();
        ::DeleteFileW(temp.c_str());
        LogWarn("prefs: could not save '{}': replacing {} failed ({}) after {} "
                "attempt(s){}",
                key, Narrow(target.wstring()), error, attempt,
                posix ? "" : " (MoveFileExW fallback)");
        return false;
      }
      ::Sleep(kReplaceRetryMs);
    }
  } catch (std::exception const& e) {
    if (!temp.empty()) ::DeleteFileW(temp.c_str());
    LogWarn("prefs: could not save '{}': {}", key, e.what());
    return false;
  }
}

bool AppPrefBool(nlohmann::json const& prefs, std::string const& key,
                 bool fallback) noexcept {
  if (!prefs.is_object()) return fallback;
  const auto it = prefs.find(key);
  if (it == prefs.end() || !it->is_boolean()) return fallback;
  return it->get<bool>();
}

std::optional<std::int64_t> AppPrefInt64(nlohmann::json const& prefs,
                                         std::string const& key) noexcept {
  if (!prefs.is_object()) return std::nullopt;
  const auto it = prefs.find(key);
  if (it == prefs.end()) return std::nullopt;
  if (it->is_number_unsigned()) {
    const auto value = it->get<std::uint64_t>();
    if (value > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
      return std::nullopt;
    return static_cast<std::int64_t>(value);
  }
  if (it->is_number_integer()) return it->get<std::int64_t>();
  return std::nullopt;
}

}  // namespace urnw
