// Export logs: the app's log, copied to where the person asked for it, with every
// urnetwork: link in it reduced to its host and a length (authlink::RedactLogText).
//
// A build before the redaction wrote the whole command line of every protocol launch to
// the log, and that carries a sign-in return (a Google or Apple identity token, an auth
// code); the file a person exports is theirs to send to someone, so it is copied without
// them. The log itself is never opened for writing, renamed or removed.
//
// Pure standard C++ with no Windows header: tools/log-export-tests.cpp runs it on any host.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "AuthLink.h"

namespace urnw::logexport {

// The log, line by line, through authlink::RedactLogText, written to `destination`. False when
// either file could not be used.
//
// Nothing is opened for writing before the source has been found to be a regular file and read
// open, and `destination` has been found not to be the source: the picker suggests the log's own
// name, a file opened to be written is emptied first, and saving over the log would then empty the
// live log and say it had saved. A source that is not a regular file or cannot be opened leaves an
// existing destination as it was. That it opens does not make it a file: Linux opens a directory
// for reading, and only the read fails.
inline bool CopyWithoutLinks(const std::filesystem::path& source, const std::filesystem::path& destination) {
  std::error_code unknown;
  if (!std::filesystem::is_regular_file(source, unknown)) return false;
  std::ifstream in(source, std::ios::binary);
  if (!in) return false;
  if (std::filesystem::equivalent(source, destination, unknown)) return false;
  std::ofstream out(destination, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  std::string line;
  while (std::getline(in, line)) {
    out << authlink::RedactLogText(line);
    if (!in.eof()) out << '\n';  // the last line of a log may end without one
  }
  out.flush();
  return !in.bad() && out.good();
}

}  // namespace urnw::logexport
