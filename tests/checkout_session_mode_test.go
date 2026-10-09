// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The upgrade sheet's embedded checkout is a redirect_on_completion "never"
// session opened on the SDK's inline bridge url, so the payment hands back
// from Stripe's onComplete on ur.io/checkout instead of a return_url round
// trip, and a completed hand-back still starts the confirmation poll. Reads
// the sheet, then compiles and runs the session-mode spec
// (tools/checkout-session-mode-tests.cpp).
func TestCheckoutSessionMode(t *testing.T) {
	root := repositoryRoot(t)
	appDir := filepath.Join(root, "app", "src", "App")
	source, err := os.ReadFile(filepath.Join(appDir, "BalanceSheets.cpp"))
	if err != nil {
		t.Fatal(err)
	}
	sheets := string(source)
	for _, want := range []string{
		"CheckoutSessionModeFor(embedded)",
		"args.redirect_on_completion = mode.redirectOnCompletion",
		"urnet::buildInlineCheckoutBridgeUrl(clientSecret)",
		// the hand-back still waits for the webhook
		"balance_.StartConfirmationPolling();",
	} {
		if !strings.Contains(sheets, want) {
			t.Errorf("BalanceSheets.cpp: missing %s", want)
		}
	}
	for _, unwanted := range []string{
		// a plain bridge url would never hand back for a "never" session
		"urnet::buildCheckoutBridgeUrl(clientSecret)",
		"redirect_on_completion stays unset",
	} {
		if strings.Contains(sheets, unwanted) {
			t.Errorf("BalanceSheets.cpp: still has %s", unwanted)
		}
	}

	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("checkout session mode tests require a C++20 compiler: ", err)
	}
	program := filepath.Join(t.TempDir(), "checkout-session-mode-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+appDir, filepath.Join(root, "app", "tools", "checkout-session-mode-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build checkout session mode tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("checkout session mode: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}
