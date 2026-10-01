// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// Ids.h needs a GUID for the tray icon and tun adapter ids; on a non-Windows
// host the tests supply the one typedef <guiddef.h> would.
const guiddefStub = `#pragma once
typedef struct _GUID {
  unsigned long Data1;
  unsigned short Data2;
  unsigned short Data3;
  unsigned char Data4[8];
} GUID;
`

// Compile and execute the bundled network space spec (Common/Ids.h,
// Common/NetworkSpaceStartup.h): the operator host, the legacy key re-keyed to
// it, and the launch order. Both headers are copied into an isolated include
// dir so mutateIds, when set, can rewrite Ids.h for a negative control and the
// copied NetworkSpaceStartup.h picks the rewritten one up.
func networkSpaceTestProgram(t *testing.T, mutateIds func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("network space tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	commonDir := filepath.Join(root, "app", "src", "Common")
	for _, name := range []string{"Ids.h", "NetworkSpaceStartup.h"} {
		source, err := os.ReadFile(filepath.Join(commonDir, name))
		if err != nil {
			t.Fatal(err)
		}
		content := string(source)
		if name == "Ids.h" && mutateIds != nil {
			content = mutateIds(content)
			if content == string(source) {
				t.Fatal("negative control did not change the production ids")
			}
		}
		if err := os.WriteFile(filepath.Join(includeDir, name), []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(filepath.Join(includeDir, "guiddef.h"), []byte(guiddefStub), 0600); err != nil {
		t.Fatal(err)
	}
	program := filepath.Join(includeDir, "network-space-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+includeDir,
		filepath.Join(root, "app", "tools", "network-space-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build network space tests: %v\n%s", err, output)
	}
	return program
}

func TestNetworkSpaceStartup(t *testing.T) {
	program := networkSpaceTestProgram(t, nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("network space startup: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The operator stays bringyour.com: the move to *.ur.network was cancelled, and
// a build that bundles the official space under ur.network again derives
// api.ur.network, a dead host. That has to fail the suite, not just a review.
func TestNetworkSpaceRejectsLegacyHost(t *testing.T) {
	program := networkSpaceTestProgram(t, func(source string) string {
		return strings.Replace(source, `kNetworkSpaceHostName[] = "bringyour.com"`,
			`kNetworkSpaceHostName[] = "ur.network"`, 1)
	})
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), "legacy ur.network key") {
		t.Fatalf("ur.network bundled host was not detected: %v\n%s", err, output)
	}
}
