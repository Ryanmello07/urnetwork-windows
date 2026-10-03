// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// Compile and execute the purchase-confirmation poll gate
// (App/ConfirmationPollGate.h): the give-up budget must pause on focus loss as
// well as on hide, and the window's activation must reach the balance store.
func TestConfirmationPollGate(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("confirmation poll gate tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "confirmation-poll-gate-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "App"),
		filepath.Join(root, "app", "tools", "confirmation-poll-gate-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build confirmation poll gate tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("confirmation poll gate: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}

	// the gate is what the store consults, and the window's activation feeds it
	for path, wants := range map[string][]string{
		filepath.Join(root, "app", "src", "App", "SubscriptionBalance.cpp"): {
			"gate_.SetFocused(", "gate_.SetVisible(", "gate_.ExpiredAt(",
		},
		filepath.Join(root, "app", "src", "App", "AppController.cpp"): {
			"window_.Activated(", "balance_.SetFocused(",
		},
	} {
		source, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		for _, want := range wants {
			if !strings.Contains(string(source), want) {
				t.Errorf("%s: missing %s", filepath.Base(path), want)
			}
		}
	}
}
