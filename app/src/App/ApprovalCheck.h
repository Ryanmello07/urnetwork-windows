// The checks that stand between a click and "Continue in browser": whether the
// approval page is live in this run and lists the contract version the network
// needs, and whether this process may use it at all. The rest of the conditions
// (the build flag, a network whose API a request can name) are
// AppController::ApprovalOffered's, which asks these.
//
// Plain Win32, no WinRT, so the .cpp builds without the precompiled header
// (App.vcxproj). The judgment of the page's answer is ApprovalDescriptor.h.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

namespace urnw {

// This process runs with an elevated token (an administrator's, after a UAC prompt).
// The browser starts the app for a link at medium integrity, and Windows does not hand
// that launch to an elevated instance, so the link would never arrive: the action is
// hidden there, and the paste flow stays. True also when the token cannot be read, so
// that a process whose rights are not known is not offered an approval it cannot finish.
bool ProcessIsElevated();

enum class ApprovalPage {
  NotChecked,  // nobody has asked yet
  Checking,    // the one request of this run is on its way
  Live,        // the page answered its descriptor in time, and lists this platform and a contract version this app speaks
  Down,        // it did not, for any reason: the action stays hidden for the rest of the run
};

// Starts the one check of this run, on a worker thread, if it has not started: a single
// GET of the descriptor (config::kApprovalDescriptorUrl) with a three-second budget and no
// redirect, whose body and content type ApprovalDescriptor.h judges. The answer, success
// and failure alike, is kept for the run; nothing asks again. Safe to call from any thread
// and as often as wanted. Never blocks. Logs closed words only.
void StartApprovalPageCheck();

// Where the check stands. NotChecked until StartApprovalPageCheck.
ApprovalPage ApprovalPageState();

// Whether the descriptor the page answered in this run lists `contract` (ApprovalNetwork.h: 1 for the official network,
// 2 for another one). False until the page has answered Live, and false for a page that lists other versions only, so a
// page that serves the official network alone is never sent a request that names another.
bool ApprovalPageLists(std::int64_t contract);

}  // namespace urnw
