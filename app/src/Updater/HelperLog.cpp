// SPDX-License-Identifier: MPL-2.0
#include "HelperLog.h"

#include <windows.h>

namespace urnw::updater {
namespace {

std::string Timestamp() {
  SYSTEMTIME now{};
  ::GetSystemTime(&now);
  return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z ", now.wYear, now.wMonth, now.wDay,
                     now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
}

bool WriteAll(HANDLE file, std::string_view text) {
  while (!text.empty()) {
    DWORD written = 0;
    if (!::WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ||
        written == 0) {
      return false;
    }
    text.remove_prefix(written);
  }
  return true;
}

}  // namespace

HelperLog::~HelperLog() {
  if (file_) ::CloseHandle(static_cast<HANDLE>(file_));
}

void HelperLog::Open(const std::filesystem::path& file, void* security) {
  if (file_) return;
  // Deleted and created new, never truncated: a file that is there keeps the
  // security it has, and this one's is the admin-only one it is given.
  ::DeleteFileW(file.c_str());
  HANDLE handle = ::CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                static_cast<SECURITY_ATTRIBUTES*>(security), CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return;
  file_ = handle;
  WriteAll(handle, kept_);
  kept_.clear();
}

void HelperLog::Write(std::string_view line) {
  const std::string text = Timestamp() + std::string(line) + "\r\n";
  ::OutputDebugStringA(text.c_str());
  if (file_) {
    WriteAll(static_cast<HANDLE>(file_), text);
  } else if (kept_.size() < 64 * 1024) {
    kept_ += text;
  }
}

}  // namespace urnw::updater
