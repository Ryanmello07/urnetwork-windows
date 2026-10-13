// The Win32 half of the per-user urnetwork:// registration; the decisions are
// Common/ProtocolRegistration.h. A browser sign-in comes back as a urnetwork://
// link, and a link only reaches this app if Windows has a handler for the scheme
// that starts it: the MSI registers one for the machine, a portable copy and a
// development build register nothing. Under HKEY_CURRENT_USER only, so it needs
// no elevation and touches no other user.
//
// Plain Win32, no WinRT, so the .cpp builds without the precompiled header
// (App.vcxproj).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace urnw {

enum class ProtocolHandlerState {
  Ours,     // the shell starts THIS exe for urnetwork://, or the key is this app's to bring up to date
  Foreign,  // it starts something else, and the key that says so is not one we may change
  Absent,   // nothing handles the scheme for this user: an attempt's start would register it
  Unsafe,   // this exe's path cannot be put in a quoted command
  Failed,   // the registry could not be read
};

// Makes sure the current user's handler starts THIS exe, and reads it back through
// the shell (AssocQueryStringW), because the shell's answer is the one that counts.
// True means a browser launch of urnetwork:// will start this exe. Called when a
// browser sign-in is about to start, BEFORE the browser is opened; false means the
// attempt does not start and the paste flow stays. Logs closed words only.
bool EnsureProtocolHandler();

// At each launch: a registration this app wrote follows the exe (it moved, it
// updated). Never creates one, and never touches a key it did not write.
void RefreshProtocolHandler();

// Where the scheme stands, as EnsureProtocolHandler would find it, without changing
// anything: whether an attempt could start at all. A key that starts something else
// (Foreign) is left alone, and then the action is not offered.
ProtocolHandlerState ProtocolHandlerStatus();

}  // namespace urnw
