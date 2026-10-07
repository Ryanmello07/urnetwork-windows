// Whether a running executable sits in an admin-only install location: the
// only place an elevated process may be started from, or may trust a file
// under.
//
// The update installs a per-machine MSI with a LocalSystem service and a
// kernel driver, so the bytes msiexec installs must be decided by nothing a
// user without admin rights can write. The tray app elevates the update helper
// next to its own exe only when this holds for that folder and that file, and
// the helper refuses to run unless it holds for its own. Otherwise the copy is
// portable or a dev build, and the update is offered as an installer the user
// runs.
//
// The location holds when all of these do:
//   - the folder is below Program Files, as the known folder resolves it
//     (FOLDERID_ProgramFiles, never the environment), comparing both final
//     paths;
//   - the folder's path resolves to itself: no junction, symbolic link, mount
//     point or substituted drive anywhere on it;
//   - neither the folder nor a folder between it and Program Files is a
//     reparse point;
//   - a token without admin rights gets no write, append, delete, attribute or
//     ownership right on the folder or on the executable (AccessCheck against
//     the user's token, or against it with Administrators made deny-only when
//     it is an elevated one).
//
// Pure and header-only: tools/update-release-tests.cpp runs it on any host
// against fixtures, and InstallLocation.cpp gathers the facts on Windows.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

namespace urnw::install {

// The access rights that let a holder change a folder or a file, or what is in
// it (winnt.h's values; InstallLocation.cpp checks they match).
inline constexpr std::uint32_t kWriteData = 0x0002;        // FILE_WRITE_DATA, FILE_ADD_FILE
inline constexpr std::uint32_t kAppendData = 0x0004;       // FILE_APPEND_DATA, FILE_ADD_SUBDIRECTORY
inline constexpr std::uint32_t kWriteEa = 0x0010;          // FILE_WRITE_EA
inline constexpr std::uint32_t kDeleteChild = 0x0040;      // FILE_DELETE_CHILD
inline constexpr std::uint32_t kWriteAttributes = 0x0100;  // FILE_WRITE_ATTRIBUTES
inline constexpr std::uint32_t kDelete = 0x00010000;       // DELETE
inline constexpr std::uint32_t kWriteDac = 0x00040000;     // WRITE_DAC
inline constexpr std::uint32_t kWriteOwner = 0x00080000;   // WRITE_OWNER
inline constexpr std::uint32_t kWriteRights = kWriteData | kAppendData | kWriteEa | kDeleteChild |
                                              kWriteAttributes | kDelete | kWriteDac | kWriteOwner;

// Where the folder is.
struct InstallPathInfo {
  // Below FOLDERID_ProgramFiles, both compared as final paths, and not Program
  // Files itself.
  bool underProgramFiles = false;
  // The folder's final path is the path it was reached by.
  bool resolvesToItself = false;
  // The folder, or a folder between it and Program Files, is a reparse point.
  bool reparsePoint = false;
};

// What a token without admin rights may do there: the granted access masks
// AccessCheck returns for MAXIMUM_ALLOWED.
struct InstallRights {
  std::uint32_t folder = 0;
  std::uint32_t executable = 0;
};

inline bool IsAdminOnlyInstallDir(const InstallPathInfo& path, const InstallRights& rights) {
  if (!path.underProgramFiles || !path.resolvesToItself || path.reparsePoint) return false;
  return (rights.folder & kWriteRights) == 0 && (rights.executable & kWriteRights) == 0;
}

}  // namespace urnw::install
