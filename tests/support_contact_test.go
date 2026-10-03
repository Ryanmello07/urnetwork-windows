// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os/exec"
	"path/filepath"
	"testing"
)

// Compile and execute the support contact spec (App/SupportContact.h): the
// Settings Stay in touch card must offer support@ur.io wherever it offers the
// Discord invite, which is unreachable in some regions.
func TestSupportContact(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("support contact tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "support-contact-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "App"),
		filepath.Join(root, "app", "tools", "support-contact-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build support contact tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("support contact: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}
