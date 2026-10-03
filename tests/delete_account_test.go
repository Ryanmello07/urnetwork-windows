// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os/exec"
	"path/filepath"
	"testing"
)

// Compile and execute the delete-account outcome (Common/DeleteAccountOutcome.h):
// a deletion the server refuses keeps the user signed in and shows why.
func TestDeleteAccountOutcome(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("delete-account tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "delete-account-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "Common"),
		filepath.Join(root, "app", "tools", "delete-account-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build delete-account tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("delete account: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}
