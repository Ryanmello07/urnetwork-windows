// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// Compile the network-change spec (app/tools/network-change-notify-tests.cpp)
// against Service/NetworkChangeNotify.h, which is portable on purpose: the
// provider-only device is told about Wi-Fi and Ethernet changes through it,
// and the service that calls it cannot be built here. `mutate`, when set,
// rewrites a copy of the header for a negative control.
func networkChangeTestProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("network change tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	fixtureDir := t.TempDir()
	includeDir := filepath.Join(root, "app", "src", "Service")
	if mutate != nil {
		source, err := os.ReadFile(filepath.Join(includeDir, "NetworkChangeNotify.h"))
		if err != nil {
			t.Fatal(err)
		}
		changed := mutate(string(source))
		if changed == string(source) {
			t.Fatal("negative control did not change the production NetworkChangeNotify.h")
		}
		includeDir = fixtureDir
		if err := os.WriteFile(filepath.Join(includeDir, "NetworkChangeNotify.h"), []byte(changed), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(fixtureDir, "network-change-notify-tests")
	build := exec.Command(compiler, "-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror",
		"-I"+includeDir, filepath.Join(root, "app", "tools", "network-change-notify-tests.cpp"),
		"-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build network change tests: %v\n%s", err, output)
	}
	return program
}

// Execute the spec: one call per burst, quality dropped inside a network pass,
// a window a flapping link cannot starve, nothing after destruction, a
// destructor that waits out a call already running, a call that throws.
func TestNetworkChangeNotifier(t *testing.T) {
	program := networkChangeTestProgram(t, nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("network change notifier: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

func requireNetworkChangeFailure(t *testing.T, mutate func(string) string, want string) {
	t.Helper()
	program := networkChangeTestProgram(t, mutate)
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

// Every OS observation of a roam becoming its own kick of every transport.
func TestNetworkChangeNotifierRejectsNoCoalescing(t *testing.T) {
	requireNetworkChangeFailure(t, func(source string) string {
		return strings.Replace(source,
			"    deadlineMillis_ = nowMillis + kNetworkNotifyDebounceMillis;",
			"    deadlineMillis_ = nowMillis;", 1)
	}, "burst: a burst is one notification")
}

// A re-extending debounce, which a flapping link starves.
func TestNetworkChangeNotifierRejectsReextendingWindow(t *testing.T) {
	requireNetworkChangeFailure(t, func(source string) string {
		return strings.Replace(source, "    ++coalesced_;\n    if (pending_) return;\n",
			"    ++coalesced_;\n", 1)
	}, "flapping: told while it still flaps")
}

// A destructor that returns while a call into the device still runs: the
// retire would close the device under it.
func TestNetworkChangeNotifierRejectsDetachedDestruction(t *testing.T) {
	requireNetworkChangeFailure(t, func(source string) string {
		return strings.Replace(source, "if (thread_.joinable()) thread_.join();",
			"if (thread_.joinable()) thread_.detach();", 1)
	}, "join: destruction returns only after the call into the device has returned")
}

// A quality change told beside the network change it already rides on.
func TestNetworkChangeNotifierRejectsQualityInsideNetworkPass(t *testing.T) {
	requireNetworkChangeFailure(t, func(source string) string {
		return strings.Replace(source, "if (qualityDue && !networkDue && channel->networkQualityChanged)",
			"if (qualityDue && channel->networkQualityChanged)", 1)
	}, "quality in a network pass: dropped")
}
