// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The Solana payout wallet (App/SolanaWalletPresentation.h): the C++ spec, and
// what the Earnings pane says when removing the payout wallet.

// Compile and execute the Solana payout wallet spec
// (App/SolanaWalletPresentation.h): the address check, the short form, which
// account wallet is the payout wallet, the USDC waiting, how the three reads
// commit, the connect sheet's state machine, the wallet bridge routing, and
// which wallet a removal promoted in the payout wallet's place.
func TestSolanaWalletPresentation(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("solana wallet tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	appDir := filepath.Join(root, "app", "src", "App")
	program := filepath.Join(t.TempDir(), "solana-wallet-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+appDir,
		filepath.Join(root, "app", "tools", "solana-wallet-tests.cpp"),
		filepath.Join(appDir, "SolanaWalletPresentation.cpp"),
		"-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build solana wallet tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("solana wallet: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Removing the payout wallet makes another active Solana or Polygon wallet of
// the network the payout wallet when there is one (server
// fix/remove-wallet-promote). The Earnings pane then says "Payouts now go to
// <short address>." (payouts_now_go_to) once the reload after the removal
// commits.
func TestRemovalNamesThePromotedPayoutWallet(t *testing.T) {
	root := repositoryRoot(t)
	appDir := filepath.Join(root, "app", "src", "App")
	source, err := os.ReadFile(filepath.Join(appDir, "WalletPage.cpp"))
	if err != nil {
		t.Fatal(err)
	}
	page := string(source)

	remove := functionBody(page, "void WalletPage::ApplyRemoveResult(")
	if !strings.Contains(remove, "payoutRemoval_ = removal;") ||
		strings.Index(remove, "payoutRemoval_ = removal;") > strings.Index(remove, "LoadLegacyWallets(/*reset=*/true);") {
		t.Error("ApplyRemoveResult does not keep the removal for the reload it starts")
	}
	commit := functionBody(page, "void WalletPage::ApplyLegacyAnswer(")
	if !strings.Contains(commit, "NotifyPromotedPayoutWallet();") {
		t.Error("the reload's commit does not look for a promoted payout wallet")
	}
	notify := functionBody(page, "void WalletPage::NotifyPromotedPayoutWallet(")
	for _, want := range []string{
		"solana::PromotedPayoutWallet(removal, legacy_)",
		`urnw::Format("payouts_now_go_to"`,
		"solana::ShortAddress(promoted->address)",
	} {
		if !strings.Contains(notify, want) {
			t.Errorf("NotifyPromotedPayoutWallet: missing %s", want)
		}
	}

	english := reswValue(t, root, "en", "payouts_now_go_to")
	if english != "Payouts now go to {}." {
		t.Fatalf("en/Resources.resw payouts_now_go_to = %q", english)
	}
	entries, err := os.ReadDir(filepath.Join(appDir, "Strings"))
	if err != nil {
		t.Fatal(err)
	}
	for _, entry := range entries {
		if !entry.IsDir() || entry.Name() == "en" {
			continue
		}
		value := reswValue(t, root, entry.Name(), "payouts_now_go_to")
		if value == "" || value == english || !strings.Contains(value, "{}") {
			t.Errorf("%s payouts_now_go_to = %q", entry.Name(), value)
		}
	}
}

// Before the removal, the confirmation covers both outcomes of removing the
// payout wallet: another of the network's Solana or Polygon wallets takes over
// (the server picks it, and the line above names it afterwards), or USDC
// payouts are held while there is none (remove_wallet_moves_or_holds_payouts).
// The retired remove_wallet_holds_payouts said they were always held, so no app
// source may show it again.
func TestRemoveConfirmationSaysPayoutsMoveOrAreHeld(t *testing.T) {
	root := repositoryRoot(t)
	appDir := filepath.Join(root, "app", "src", "App")
	confirm := functionBody(readAppSource(t, "WalletPage.cpp"), "winrt::fire_and_forget WalletPage::ConfirmRemoveSolanaWallet(")
	if !strings.Contains(confirm, `dialog.Content(winrt::box_value(Loc("remove_wallet_moves_or_holds_payouts")));`) {
		t.Error("the remove confirmation does not show remove_wallet_moves_or_holds_payouts")
	}
	sources, err := os.ReadDir(appDir)
	if err != nil {
		t.Fatal(err)
	}
	for _, entry := range sources {
		ext := filepath.Ext(entry.Name())
		if entry.IsDir() || (ext != ".cpp" && ext != ".h" && ext != ".xaml") {
			continue
		}
		if strings.Contains(readAppSource(t, entry.Name()), `"remove_wallet_holds_payouts"`) {
			t.Errorf("%s shows remove_wallet_holds_payouts; payouts move to another wallet when there is one", entry.Name())
		}
	}

	const confirmation = "USDC payouts move to another of your Solana or Polygon wallets, or are held until you connect one."
	if english := reswValue(t, root, "en", "remove_wallet_moves_or_holds_payouts"); english != confirmation {
		t.Fatalf("en/Resources.resw remove_wallet_moves_or_holds_payouts = %q", english)
	}
	locales, err := os.ReadDir(filepath.Join(appDir, "Strings"))
	if err != nil {
		t.Fatal(err)
	}
	for _, entry := range locales {
		if !entry.IsDir() {
			continue
		}
		if reswValue(t, root, entry.Name(), "remove_wallet_holds_payouts") != "" {
			t.Errorf("%s keeps the retired remove_wallet_holds_payouts", entry.Name())
		}
		if entry.Name() == "en" {
			continue
		}
		value := reswValue(t, root, entry.Name(), "remove_wallet_moves_or_holds_payouts")
		if value == "" || value == confirmation {
			t.Errorf("%s remove_wallet_moves_or_holds_payouts = %q", entry.Name(), value)
			continue
		}
		for _, name := range []string{"USDC", "Solana", "Polygon"} {
			if !strings.Contains(value, name) {
				t.Errorf("%s remove_wallet_moves_or_holds_payouts drops %s: %q", entry.Name(), name, value)
			}
		}
	}
}

// The ur.io wallet bridge hands a failure back with its own code (the sdk's
// SolanaWalletBridgeError*) next to its English text, on the connect and the
// sign step. WalletConnect.cpp says a code the app knows in the app's words
// (App/SolanaWalletPresentation.h BridgeErrorTextFor, run by
// TestSolanaWalletPresentation), with Phantom or Solflare where the string
// takes the wallet's name; any other code still shows the page's text.
func TestSolanaBridgeErrorCodes(t *testing.T) {
	bridge := stripLineComments(readAppSource(t, "WalletConnect.cpp"))
	localized := functionBody(bridge, "std::string LocalizedBridgeError(")
	for _, want := range []string{
		"solana::BridgeErrorTextFor(code)",
		"if (text.key.empty()) return pageText;",
		`Localized(p == WalletConnect::Provider::Solflare ? "solflare" : "phantom")`,
		"Format(text.key, walletName)",
	} {
		if !strings.Contains(localized, want) {
			t.Errorf("WalletConnect.cpp LocalizedBridgeError: missing %s", want)
		}
	}
	for _, handler := range []string{"void WalletConnect::HandleConnect(", "void WalletConnect::HandleSignMessage("} {
		if !strings.Contains(functionBody(bridge, handler), `on_error(LocalizedBridgeError(p, params["errorCode"], pageText));`) {
			t.Errorf("WalletConnect.cpp %s: the bridge page's code does not reach the error text", handler)
		}
	}

	// every key BridgeErrorTextFor returns is translated in every language, with
	// the wallet name's {} exactly where the key takes it
	root := repositoryRoot(t)
	entries, err := os.ReadDir(filepath.Join(root, "app", "src", "App", "Strings"))
	if err != nil {
		t.Fatal(err)
	}
	for name, takesWalletName := range map[string]bool{
		"bittensor_error_extension_not_found":   true,
		"bittensor_error_no_account":            true,
		"bittensor_error_user_rejected":         false,
		"solana_wallet_error_session_not_found": false,
	} {
		englishValue := reswValue(t, root, "en", name)
		if englishValue == "" {
			t.Errorf("en/Resources.resw has no %s", name)
			continue
		}
		for _, entry := range entries {
			if !entry.IsDir() {
				continue
			}
			value := reswValue(t, root, entry.Name(), name)
			if value == "" {
				t.Errorf("%s/Resources.resw has no %s", entry.Name(), name)
				continue
			}
			if entry.Name() != "en" && value == englishValue {
				t.Errorf("%s/Resources.resw %s is English", entry.Name(), name)
			}
			if strings.Contains(value, "{}") != takesWalletName {
				t.Errorf("%s/Resources.resw %s = %q, want the wallet name placeholder: %v", entry.Name(), name, value, takesWalletName)
			}
		}
	}
	for _, name := range []string{"phantom", "solflare"} {
		if reswValue(t, root, "en", name) == "" {
			t.Errorf("en/Resources.resw has no %s, the wallet name the strings take", name)
		}
	}
}

// The codes BridgeErrorTextFor knows are the sdk's own
// (urnet::SolanaWalletBridgeError*). Skipped without an sdk header that has
// them (see sdkHeaderWith).
func TestSolanaBridgeCodesMatchTheSdkHeader(t *testing.T) {
	headerSource := sdkHeaderWith(t, "SolanaWalletBridgeError")
	presentationSource := readAppSource(t, "SolanaWalletPresentation.cpp")
	for name, code := range map[string]string{
		"SolanaWalletBridgeErrorExtensionNotFound": "extension_not_found",
		"SolanaWalletBridgeErrorNoAccount":         "no_account",
		"SolanaWalletBridgeErrorSessionNotFound":   "session_not_found",
		"SolanaWalletBridgeErrorUserRejected":      "user_rejected",
	} {
		if !strings.Contains(headerSource, `inline constexpr const char* `+name+` = "`+code+`";`) {
			t.Errorf("urnetwork_sdk.hpp: urnet::%s is not %q", name, code)
		}
		if !strings.Contains(presentationSource, `if (code == "`+code+`")`) {
			t.Errorf("SolanaWalletPresentation.cpp: BridgeErrorTextFor does not know the sdk's %q", code)
		}
	}
}
