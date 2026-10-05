// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

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
