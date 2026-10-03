// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// Compile and execute the redeem sheet's answer spec (App/BalanceCodeRedeem.h),
// and check the sheet classifies through the SDK with the network's
// redeemed-code list instead of deciding from the raw answer.
func TestBalanceCodeRedeem(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("balance code redeem tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "balance-code-redeem-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "App"),
		filepath.Join(root, "app", "tools", "balance-code-redeem-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build balance code redeem tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("balance code redeem: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}

	source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "BalanceSheets.cpp"))
	if err != nil {
		t.Fatal(err)
	}
	sheet := string(source)
	// first without the list, then again with it
	if n := strings.Count(sheet, "urnet::classifyBalanceCodeRedeem("); n != 2 {
		t.Errorf("BalanceSheets.cpp: want 2 urnet::classifyBalanceCodeRedeem calls, got %d", n)
	}
	for _, want := range []string{
		"BalanceCodeRedeemNeedsCodeList(",
		"getNetworkRedeemedBalanceCodes(",
		`Loc("balance_code_already_redeemed_message")`,
		"urnet::isBalanceCodeFormatValid(",
		"urnet::BalanceCodeLength",
	} {
		if !strings.Contains(sheet, want) {
			t.Errorf("BalanceSheets.cpp: missing %s", want)
		}
	}
	// the sheet no longer decides from the raw answer or a hardcoded length
	for _, unwanted := range []string{"kBalanceCodeLength", "result->transfer_balance.has_value()"} {
		if strings.Contains(sheet, unwanted) {
			t.Errorf("BalanceSheets.cpp: still has %s", unwanted)
		}
	}
}
