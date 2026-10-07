// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os/exec"
	"path/filepath"
	"testing"
)

// Compile and execute the portable Fast DNS on connect spec: the opt-in
// host-network DNS fallback toggle's settings mapping, its labels in the DNS
// editor and the drawer, and the generated English strings.
func TestFastDnsOnConnectSetting(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("dns settings tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	appDir := filepath.Join(root, "app")
	program := filepath.Join(t.TempDir(), "dns-settings-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(appDir, "src", "App"),
		filepath.Join(appDir, "tools", "dns-settings-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build dns settings tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program, appDir).CombinedOutput(); err != nil {
		t.Fatalf("dns settings: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}
