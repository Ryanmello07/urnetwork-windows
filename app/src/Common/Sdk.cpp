// SPDX-License-Identifier: MPL-2.0
#include "Sdk.h"

#include <format>
#include <limits>
#include <system_error>

// WIN32_LEAN_AND_MEAN is already on this project's command line; redefining it
// here only produces a C4005.
#include <windows.h>

#include "Log.h"
#include "Paths.h"
#include "Strings.h"

namespace urnw {
namespace {

// Two files, one generation of history. See the rotation note in Sdk.h for why
// one generation is the right number and not a compromise.
constexpr wchar_t kGoCrashFile[] = L"go-crash.log";
constexpr wchar_t kGoCrashPrevFile[] = L"go-crash.prev.log";

// The host measurement behind the memory tier, taken once.
//
// GlobalMemoryStatusEx is the whole probe: ullTotalPhys is physical RAM, which
// is what the tier gate asks about. This process is in no job object and has no
// working-set limit, so there is no smaller container bound to take the minimum
// with, the way the Linux daemon does with its cgroup.
//
// A failed call returns 0, and 0 takes the base tier: an unknown host is not a
// large host.
int64_t MeasureHostMemoryByteCount() {
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (!::GlobalMemoryStatusEx(&status)) {
    LogWarn("host memory could not be measured (error {}); taking the base memory tier",
            ::GetLastError());
    return 0;
  }
  // ullTotalPhys is a ULONGLONG. Anything that would not fit an int64_t is not
  // a real machine, and clamping such a value to 0 would read as "unknown"
  // rather than "enormous", so clamp to the int64 maximum instead.
  constexpr unsigned long long kMaxSigned =
      static_cast<unsigned long long>((std::numeric_limits<int64_t>::max)());
  return static_cast<int64_t>(status.ullTotalPhys > kMaxSigned ? kMaxSigned : status.ullTotalPhys);
}

}  // namespace

int64_t HostMemoryByteCount() {
  // Function-local static: measured once, thread-safe initialization.
  static const int64_t byteCount = MeasureHostMemoryByteCount();
  return byteCount;
}

int64_t ProcessMemoryBudgetByteCount() {
  return MemoryTierForHost(HostMemoryByteCount()).process_budget_byte_count;
}

int64_t DeviceMemoryTargetByteCount() {
  return MemoryTierForHost(HostMemoryByteCount()).device_target_byte_count;
}

void SdkInit(bool isService, int64_t memoryLimitBytes) {
  const std::string logDir = Narrow(LogDir(isService).wstring());
  urnet::setLogDir(logDir);
  urnet::setMemoryLimit(memoryLimitBytes);
  LogInfo("sdk initialized: version={} logDir={} memLimit={}MB hostMem={}MB deviceTarget={}MB",
          urnet::version(), logDir, memoryLimitBytes / (1024 * 1024),
          HostMemoryByteCount() / (1024 * 1024),
          DeviceMemoryTargetByteCount() / (1024 * 1024));
}

GoCrashCapture RedirectGoCrashOutput(const std::filesystem::path& dir) {
  GoCrashCapture out;
  out.path = dir / kGoCrashFile;

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);

  // Rotate only what is worth keeping. file_size() failing means the file is
  // not there, which is the ordinary first-ever-start case, not an error.
  const std::uintmax_t bytes = std::filesystem::file_size(out.path, ec);
  if (!ec && bytes > 0) {
    const std::filesystem::path prev = dir / kGoCrashPrevFile;
    std::error_code renameEc;
    std::filesystem::rename(out.path, prev, renameEc);
    if (!renameEc) {
      out.carried_over = prev;
      out.carried_over_bytes = bytes;
    }
    // A failed rename is survivable and deliberately not fatal: the append-mode
    // open below then adds this run's output after the last run's instead of
    // beside it. Ugly to read, but nothing is lost, and losing it is the only
    // outcome that would matter.
  }

  // THE HANDLE IS OPENED HERE RATHER THAN VIA freopen(stderr), AND THE SHARE
  // MODE IS WHY. The obvious implementation is _wfreopen_s(..., L"a", stderr):
  // shorter, and it carries the CRT's stderr along for free. It was written
  // that way first and the selftest rejected it — a second opener got
  // ERROR_SHARING_VIOLATION while the capture was live, because the CRT opens
  // with its own share mode and not one that admits an onlooker. That would
  // have shipped a crash file the owner could not read off a running service,
  // and "read the file, tell me what it says" is the entire user story. The
  // share mode is therefore part of the contract, so it is stated here instead
  // of inherited from the CRT.
  //
  // FILE_APPEND_DATA rather than GENERIC_WRITE: an append-only handle ignores
  // the file pointer and lands every write at end-of-file atomically. That
  // costs nothing and removes a whole class of question about what happens when
  // the Go runtime and anything else write at once, or about where the pointer
  // was left. It also means nothing here can truncate what is already on disk,
  // which matters most on the path where the rotation above failed.
  //
  // FILE_SHARE_DELETE alongside READ/WRITE so the file can be rotated or
  // cleaned up by another process while this one still holds it.
  //
  // OPEN_ALWAYS: create on the normal path (the rotation just moved the old one
  // away), open on the path where the rotation failed.
  const HANDLE file = ::CreateFileW(
      out.path.c_str(), FILE_APPEND_DATA | SYNCHRONIZE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    out.error = std::format("CreateFileW failed: {}", ::GetLastError());
    return out;
  }

  // THE LOAD-BEARING CALL. Everything above is bookkeeping; this is the line
  // that decides whether a Go fatal is recorded or lost. The runtime asks
  // GetStdHandle(STD_ERROR_HANDLE) for itself, on every write, and WriteFile()s
  // to whatever comes back.
  if (!::SetStdHandle(STD_ERROR_HANDLE, file)) {
    out.error = std::format("SetStdHandle failed: {}", ::GetLastError());
    ::CloseHandle(file);
    return out;
  }

  // `file` is deliberately not closed and not tracked. It has to outlive every
  // other thing in this process — the fatal it exists for can land during
  // shutdown — and the OS reclaims it at exit. There is no CRT buffer in this
  // path at all, so there is also nothing to flush: WriteFile is the whole
  // write, and it has returned before the runtime moves on to ExitProcess.
  out.armed = true;
  return out;
}

}  // namespace urnw
