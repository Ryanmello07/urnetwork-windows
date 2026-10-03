// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// functionBody returns the body of the function whose definition starts with
// signature (braces balanced from the first '{' after it), or "".
func functionBody(source string, signature string) string {
	at := strings.Index(source, signature)
	if at < 0 {
		return ""
	}
	open := strings.Index(source[at:], "{")
	if open < 0 {
		return ""
	}
	open += at
	depth := 0
	for i := open; i < len(source); i++ {
		switch source[i] {
		case '{':
			depth++
		case '}':
			depth--
			if depth == 0 {
				return source[open : i+1]
			}
		}
	}
	return ""
}

// A legacy guest network (UPGRADE.md D8, S6): the server removed the guest
// upgrade routes and the SDK's UpgradeGuest now always fails, so nothing may
// call it (or create guest networks). Every upgrade and create-account entry
// for a guest offers to sign out instead, warning that the guest balance stays
// on the guest network, and no checkout opens for a guest.
func TestLegacyGuestAccountFlow(t *testing.T) {
	root := repositoryRoot(t)
	appDir := filepath.Join(root, "app", "src", "App")
	read := func(name string) string {
		source, err := os.ReadFile(filepath.Join(appDir, name))
		if err != nil {
			t.Fatal(err)
		}
		return string(source)
	}

	entries, err := os.ReadDir(appDir)
	if err != nil {
		t.Fatal(err)
	}
	for _, entry := range entries {
		name := entry.Name()
		if !strings.HasSuffix(name, ".cpp") && !strings.HasSuffix(name, ".h") &&
			!strings.HasSuffix(name, ".xaml") {
			continue
		}
		source := read(name)
		for _, dead := range []string{"upgradeGuest(", "UpgradeGuest(", "LoginAsGuest", "guest_mode = true", "GuestModeSheet"} {
			if strings.Contains(source, dead) {
				t.Errorf("%s: still has %s", name, dead)
			}
		}
	}

	login := read("LoginPage.cpp")
	offer := functionBody(login, "winrt::fire_and_forget LoginPage::OfferGuestSignOut()")
	if offer == "" {
		t.Fatal("LoginPage.cpp: no OfferGuestSignOut")
	}
	for _, want := range []string{
		`Loc("guest_sign_out_balance_warning")`,
		`Loc("guest_sign_out_and_create_account")`,
		"ContentDialogButton::Close",
		"Sdk().Logout()",
	} {
		if !strings.Contains(offer, want) {
			t.Errorf("OfferGuestSignOut: missing %s", want)
		}
	}
	if !strings.Contains(functionBody(login, "void LoginPage::OnAccountMenu("), "OfferGuestSignOut()") {
		t.Error("the account menu's Create account does not offer the guest sign-out")
	}

	window := read("MainWindow.xaml.cpp")
	for _, signature := range []string{
		"winrt::fire_and_forget MainWindow::ShowUpgradeSheet()",
		"winrt::fire_and_forget MainWindow::ShowUpgradeCheckout(bool yearly)",
		"void MainWindow::OnOpenUpgrade(",
	} {
		body := functionBody(window, signature)
		if !strings.Contains(body, "if (balance_.guest)") || !strings.Contains(body, "OfferGuestSignOut()") {
			t.Errorf("%s: a guest is not diverted to the sign-out offer", signature)
		}
	}

	resources := read(filepath.Join("Strings", "en", "Resources.resw"))
	for _, key := range []string{"guest_sign_out_balance_warning", "guest_sign_out_and_create_account"} {
		if !strings.Contains(resources, `name="`+key+`"`) {
			t.Errorf("en Resources.resw: missing %s", key)
		}
	}
}
