// SPDX-License-Identifier: MPL-2.0

package tests

// The Earnings page's words for a coldkey signature that POST /sn/wallet
// refuses as not from the entered address (error.code signature_mismatch).
// The decision is BittensorWalletFlow.h's ConnectErrorKey, compiled and run by
// TestBittensorWalletSessionHelper; this file checks the WinRT page's wiring.

import (
	"strings"
	"testing"
)

// WalletPage.cpp is WinRT and does not build here, so this checks its wiring:
// every coldkey connect remembers the wallet that signs, the api fallback keeps
// the refusal's code as the device path does, and a refused connect asks
// ConnectErrorKey for the words, with the wallet's name.
func TestWalletSignatureMismatchWiring(t *testing.T) {
	page := stripLineComments(readAppSource(t, "WalletPage.cpp"))
	connectWallet := definitionBody(t, "WalletPage.cpp", page, "void WalletPage::ConnectWithWallet(")
	if !strings.Contains(connectWallet, "connectWalletId_ = walletId;") {
		t.Error("WalletPage.cpp: ConnectWithWallet does not remember the wallet that signs")
	}
	setWalletError := definitionBody(t, "WalletPage.cpp", page, "urnet::SnError SetWalletError(")
	if !strings.Contains(setWalletError, "error.code = source.code;") {
		t.Error("WalletPage.cpp: the api fallback drops the refusal's code")
	}
	applyResult := definitionBody(t, "WalletPage.cpp", page, "void WalletPage::ApplyWalletConnectResult(")
	for _, want := range []string{
		"urnet::bittensorWalletTransportFor(connectWalletId_, std::string(bittensor::kPlatform))",
		"bittensor::ConnectErrorKey(",
		"urnet::bittensorWalletDisplayName(connectWalletId_)",
	} {
		if !strings.Contains(applyResult, want) {
			t.Errorf("WalletPage.cpp: ApplyWalletConnectResult is missing %s", want)
		}
	}
}
