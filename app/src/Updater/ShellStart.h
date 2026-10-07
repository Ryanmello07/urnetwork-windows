// Starting a program as the signed-in user, from the elevated helper.
//
// The helper runs elevated, and anything it starts with CreateProcess would be
// elevated too. After an install that failed, it starts its own relaunch
// (main.cpp, no arguments) the way the MSI's WixUnelevatedShellExec does after
// one that took: through the desktop's shell, so the program runs as the user
// the shell runs as, unelevated, in that user's environment. That is
// Explorer's Shell.Application object for the desktop window
// (IShellWindows::FindWindowSW, then IShellDispatch2::ShellExecute).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <filesystem>
#include <string>

namespace urnw::updater {

// Starts `program` with no arguments, in its own folder, through the desktop
// shell. False, with `error`, when there is no shell to ask (no Explorer in
// this session) or it refused.
bool StartThroughShell(const std::filesystem::path& program, std::string& error);

}  // namespace urnw::updater
