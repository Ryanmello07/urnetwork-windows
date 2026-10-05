// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os/exec"
	"path/filepath"
	"testing"
)

// Compile and execute the cloud proxy link spec (App/CloudProxyLink.h): the app
// has no protocol switch, so Settings links to the ur.io cloud proxies page for
// WireGuard, SOCKS and HTTPS proxies. The link must be the official page, carry
// no credential, and be opened by the Connections section.
func TestCloudProxyLink(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("cloud proxy link tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "cloud-proxy-link-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "App"),
		filepath.Join(root, "app", "tools", "cloud-proxy-link-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build cloud proxy link tests: %v\n%s", err, output)
	}
	settings := filepath.Join(root, "app", "src", "App", "SettingsPage.cpp")
	if output, err := exec.Command(program, settings).CombinedOutput(); err != nil {
		t.Fatalf("cloud proxy link: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}
