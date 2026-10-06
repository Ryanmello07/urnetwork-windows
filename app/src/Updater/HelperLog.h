// The update helper's log: update-helper.log in the release's download
// folder under the admin-only install folder, beside msiexec's install.log.
// Lines written before the folder exists are kept and written when it opens;
// every line also goes to the debugger. Not safe for concurrent use: the
// helper is one thread.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace urnw::updater {

class HelperLog {
 public:
  HelperLog() = default;
  ~HelperLog();
  HelperLog(const HelperLog&) = delete;
  HelperLog& operator=(const HelperLog&) = delete;

  // Starts the file, replacing an earlier run's, created new with
  // `security` (a SECURITY_ATTRIBUTES*: ApplyUpdate.cpp's admin-only one),
  // and writes what was kept.
  void Open(const std::filesystem::path& file, void* security);

  void Write(std::string_view line);

  template <class... Args>
  void Line(std::format_string<Args...> format, Args&&... args) {
    Write(std::format(format, std::forward<Args>(args)...));
  }

 private:
  void* file_ = nullptr;
  std::string kept_;
};

}  // namespace urnw::updater
