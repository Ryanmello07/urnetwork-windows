// SPDX-License-Identifier: MPL-2.0

package tests

// Sign-in, network create and an added sign-in method say when a pasted
// TAO.com signature is from another account than the entered address: the
// server's signature_mismatch (sign-in and create with result_errors). The
// words are BittensorWalletFlow.h's ConnectErrorKey and the add flow's code is
// App/AddSignIn.h's, both compiled and run by TestBittensorWalletSessionHelper
// and TestAddSignInOptionsAndFlow; this file checks the WinRT files' wiring.

import (
	"strings"
	"testing"
)

// SdkHost.cpp and SettingsSheets.cpp are WinRT and do not build here, so this
// checks their wiring: the wallet sign-in and the wallet create ask for coded
// refusals and word them with the Bittensor wallet that signed, and the add
// sheet passes addAuth's code to the flow and words its refusal the same way.
func TestWalletAuthSignatureMismatchWiring(t *testing.T) {
	host := stripLineComments(readAppSource(t, "SdkHost.cpp"))
	refusalText := definitionBody(t, "SdkHost.cpp", host, "std::string WalletProofRefusalText(")
	for _, want := range []string{
		"bittensor::ConnectErrorKey(",
		"urnet::bittensorWalletTransportFor(bittensorWalletId, std::string(bittensor::kPlatform))",
		"urnet::bittensorWalletDisplayName(bittensorWalletId)",
	} {
		if !strings.Contains(refusalText, want) {
			t.Errorf("SdkHost.cpp: WalletProofRefusalText is missing %s", want)
		}
	}
	signIn := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::SignInWithBittensor(")
	if !strings.Contains(signIn, "WalletConnect::Provider::Bittensor, walletId)") {
		t.Error("SdkHost.cpp: SignInWithBittensor does not tell the sign-in which wallet signed")
	}
	create := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::CreateNetwork(")
	if !strings.Contains(create, "SubmitCreateNetwork(params, std::move(createAuth), std::move(done), walletId)") {
		t.Error("SdkHost.cpp: the Bittensor create does not tell the create which wallet signed")
	}
	for _, signature := range []string{"void SdkHost::AuthLoginWithWallet(", "void SdkHost::SubmitCreateNetwork("} {
		body := definitionBody(t, "SdkHost.cpp", host, signature)
		for _, want := range []string{
			"args.result_errors = true;",
			"WalletProofRefusalText(result->error->code.value_or(std::string()),",
		} {
			if !strings.Contains(body, want) {
				t.Errorf("SdkHost.cpp: %s is missing %s", signature, want)
			}
		}
	}

	sheets := stripLineComments(readAppSource(t, "SettingsSheets.cpp"))
	for _, want := range []string{
		"result->error->code.value_or(std::string())",
		"done(error, code);",
		"WalletProofRefusalText(add_->ErrorCode(), add_->Error(), add_->ErrorWalletId())",
	} {
		if !strings.Contains(sheets, want) {
			t.Errorf("SettingsSheets.cpp: the add sheet is missing %s", want)
		}
	}
}
