// The two Bittensor wallet dialogs: the chooser (Talisman / TAO.com) in front
// of every Bittensor flow, and the manual form a TAO.com proof needs (the
// message to sign, the coldkey address, the pasted signature). The protocol
// behind both is the SDK session helper, run by SdkHost; these only collect
// the user's answers. Both run on the UI thread.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <string>

#include <winrt/Windows.Foundation.h>

#include "SdkHost.h"

namespace winrt::URnetwork::implementation {
struct MainWindow;
}

namespace urnw::pages {

// `next` gets "talisman", "taocom", or "" when the user cancelled. Does
// nothing (and calls nothing) while another ContentDialog is open.
winrt::fire_and_forget ChooseBittensorWallet(
    winrt::com_ptr<winrt::URnetwork::implementation::MainWindow> window,
    std::function<void(std::string walletId)> next);

// The manual form for a proof SdkHost asked for (SetBittensorManualHandler).
// Continue submits to SdkHost::SubmitBittensorManual and stays open on a
// correctable error; closing it cancels the proof.
winrt::fire_and_forget ShowBittensorManualForm(
    winrt::com_ptr<winrt::URnetwork::implementation::MainWindow> window,
    SdkHost::BittensorManualRequest request);

}  // namespace urnw::pages
