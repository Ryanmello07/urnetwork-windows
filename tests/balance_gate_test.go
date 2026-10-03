// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os/exec"
	"path/filepath"
	"testing"
)

// Compile and execute the insufficient-balance gate (Common/BalanceGate.h): the
// connect button keeps Disconnect out of balance, and auto-disconnect waits out the grace.
func TestBalanceGate(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("balance gate tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "balance-gate-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "Common"),
		filepath.Join(root, "app", "tools", "balance-gate-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build balance gate tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("balance gate: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}
