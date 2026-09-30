// The user-facing wording for a wintun failure, with the Windows error code in
// it.
//
// Tunnel start errors reach the user verbatim (TunnelController's error_ is
// the status the app shows), and "failed to create the wintun adapter (needs
// LocalSystem/admin and a loadable wintun driver)" named two possible causes
// without saying which one happened: the GetLastError code that decides it
// went to the service log only. This keeps the code in the message and adds a
// cause for the codes whose meaning is unambiguous, so a report carries what
// is needed to act on it.
//
// Pure (no Windows headers): the codes are spelled as numbers so
// tools/wintun-error-tests.cpp runs this on any host.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace urnw::wintun_error {

// winerror.h values, as numbers.
inline constexpr std::uint32_t kAccessDenied = 5;          // ERROR_ACCESS_DENIED
inline constexpr std::uint32_t kModNotFound = 126;         // ERROR_MOD_NOT_FOUND
inline constexpr std::uint32_t kProcNotFound = 127;        // ERROR_PROC_NOT_FOUND
inline constexpr std::uint32_t kBadExeFormat = 193;        // ERROR_BAD_EXE_FORMAT
inline constexpr std::uint32_t kInvalidImageHash = 577;    // ERROR_INVALID_IMAGE_HASH
inline constexpr std::uint32_t kDriverBlocked = 1275;      // ERROR_DRIVER_BLOCKED

inline std::string CodeText(std::uint32_t code) {
  return std::format("Windows error {} (0x{:X})", code, code);
}

// Step 1a: LoadLibrary(wintun.dll) or resolving its exports failed.
inline std::string LoadFailure(std::uint32_t code) {
  std::string cause;
  switch (code) {
    case kModNotFound:
      cause = "wintun.dll is missing next to urnetworkd.exe; reinstall "
              "URnetwork, and check whether antivirus quarantined it";
      break;
    case kProcNotFound:
      cause = "wintun.dll is not the expected Wintun build; reinstall URnetwork";
      break;
    case kBadExeFormat:
      cause = "wintun.dll is for a different CPU architecture; install the "
              "URnetwork build for this PC";
      break;
    default:
      cause = "is it next to urnetworkd.exe?";
      break;
  }
  return "failed to load wintun.dll: " + CodeText(code) + " - " + cause;
}

// Step 1b: WintunCreateAdapter (which installs the driver on first use) or
// WintunStartSession failed.
inline std::string AdapterFailure(std::uint32_t code) {
  std::string cause;
  switch (code) {
    case kAccessDenied:
      cause = "access denied; the tunnel must run in the URnetwork service "
              "(LocalSystem) or an elevated console";
      break;
    case kInvalidImageHash:
      cause = "Windows rejected the wintun driver's signature; check the "
              "driver blocklist, Memory integrity, or antivirus";
      break;
    case kDriverBlocked:
      cause = "Windows or security software blocked the wintun driver";
      break;
    default:
      cause = "it needs the URnetwork service (LocalSystem) or administrator "
              "rights and a wintun driver Windows will load; the service log "
              "has wintun's own messages";
      break;
  }
  return "failed to create the wintun adapter: " + CodeText(code) + " - " + cause;
}

}  // namespace urnw::wintun_error
