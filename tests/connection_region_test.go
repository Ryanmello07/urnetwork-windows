// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os/exec"
	"path/filepath"
	"testing"
)

// Compile and execute the portable connection region spec: the picker rows,
// the shared stored value, the can't-reach classification and the wiring of
// the login control, the DNS editor and the prompt.
func TestConnectionRegion(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("connection region tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	appDir := filepath.Join(root, "app")
	program := filepath.Join(t.TempDir(), "connection-region-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(appDir, "src", "App"),
		filepath.Join(appDir, "tools", "connection-region-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build connection region tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program, appDir).CombinedOutput(); err != nil {
		t.Fatalf("connection region: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}
