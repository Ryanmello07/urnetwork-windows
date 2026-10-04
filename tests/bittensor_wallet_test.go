// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// UPGRADE.md 4.6: a Bittensor wallet proof is the SDK's session helper
// (urnet::BittensorWalletSession), for three wallets: Talisman and
// WalletConnect through the ur.io bridge in the browser, TAO.com (and any
// other wallet) through the manual form. The app
// keeps only its own decisions (App/BittensorWalletFlow.h, compiled and run
// here) and must not parse the hand-back or build the bridge url itself.
func TestBittensorWalletSessionHelper(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("bittensor wallet tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	appDir := filepath.Join(root, "app", "src", "App")
	program := filepath.Join(t.TempDir(), "bittensor-wallet-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+appDir, filepath.Join(root, "app", "tools", "bittensor-wallet-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build bittensor wallet tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("bittensor wallet: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}

	host := stripLineComments(readAppSource(t, "SdkHost.cpp"))
	for _, want := range []string{
		"urnet::newBittensorWalletSession(",
		"std::string(bittensor::kRedirectLink)",
		"session->challengeArgs(expectedAddress)",
		"session->setChallenge(result,",
		"session->bridgeUrl()",
		"session->handleBridgeReturn(url,",
		"session->handleSignature(address, signature,",
		"bittensor::IsForeignReturn(",
		"bittensor::NeedsWalletConnectProjectId(walletId)",
		"session->setWalletConnectProjectId(urnw::config::kWalletConnectProjectId)",
	} {
		if !strings.Contains(host, want) {
			t.Errorf("SdkHost.cpp: the Bittensor proof does not go through the session helper (missing %s)", want)
		}
	}

	bridge := stripLineComments(readAppSource(t, "WalletConnect.cpp"))
	for _, unwanted := range []string{"wc_project_id", "SignMessageBittensor", `params["signature"]`} {
		if strings.Contains(bridge, unwanted) {
			t.Errorf("WalletConnect.cpp: still builds or parses the Bittensor bridge itself (%s)", unwanted)
		}
	}
	if !strings.Contains(bridge, "on_bittensor_return(url)") {
		t.Error("WalletConnect.cpp: the Bittensor hand-back is not handed to the session")
	}

	for _, name := range []string{"LoginPage.cpp", "WalletPage.cpp"} {
		if !strings.Contains(stripLineComments(readAppSource(t, name)), "ChooseBittensorWallet(") {
			t.Errorf("%s: the Bittensor flow does not start at the wallet chooser", name)
		}
	}
	dialogs := stripLineComments(readAppSource(t, "BittensorWalletDialogs.cpp"))
	for _, want := range []string{
		"urnet::bittensorWalletDisplayName(",
		"bittensor::kChooserWallets[i]",
		"bittensor::ChooserHintKey(walletId)",
		"bittensor::WalletForChoice(*choice)",
		"Sdk().SubmitBittensorManual(",
		"Sdk().CancelBittensorProof()",
		`Loc("bittensor_choose_wallet")`,
		`Loc("bittensor_message_to_sign")`,
		`Loc("bittensor_signature_label")`,
	} {
		if !strings.Contains(dialogs, want) {
			t.Errorf("BittensorWalletDialogs.cpp: missing %s", want)
		}
	}
	if !strings.Contains(stripLineComments(readAppSource(t, "WalletPage.cpp")), "bittensor::BrowserHintKey(walletId)") {
		t.Error("WalletPage.cpp: the browser hint does not follow the chosen wallet")
	}
	if strings.Contains(dialogs, `"Talisman"`) || strings.Contains(dialogs, `"TAO.com"`) || strings.Contains(dialogs, `"WalletConnect"`) {
		t.Error("BittensorWalletDialogs.cpp: wallet names come from the sdk, not literals")
	}
	if !strings.Contains(readAppSource(t, "App.vcxproj"), `<ClCompile Include="BittensorWalletDialogs.cpp" />`) {
		t.Error("App.vcxproj: BittensorWalletDialogs.cpp is not compiled")
	}
}
