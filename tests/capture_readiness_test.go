// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// Compile and execute the service's portable capture transaction with injected
// machine effects. No adapter, route, DNS entry, filter, or account is touched.
func captureTestProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("capture regression tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	fixtureDir := t.TempDir()
	includeDir := filepath.Join(root, "app", "src", "Service")
	if mutate != nil {
		source, err := os.ReadFile(filepath.Join(includeDir, "CaptureReadiness.h"))
		if err != nil {
			t.Fatal(err)
		}
		changed := mutate(string(source))
		if changed == string(source) {
			t.Fatal("negative control did not change the production capture code")
		}
		includeDir = fixtureDir
		if err := os.WriteFile(filepath.Join(includeDir, "CaptureReadiness.h"), []byte(changed), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(fixtureDir, "capture-readiness-tests")
	build := exec.Command(compiler, "-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror",
		"-I"+includeDir,
		"-I"+filepath.Join(root, "app", "src", "Common"),
		filepath.Join(root, "app", "tools", "capture-readiness-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build capture tests: %v\n%s", err, output)
	}
	return program
}

// Execute the production coordinator, including forced cancellation at each
// externally visible step and a barrier-controlled in-flight SDK sample.
func TestCaptureReadinessLifecycle(t *testing.T) {
	program := captureTestProgram(t, nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("capture lifecycle: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The original failure was eager capture with no provider evidence. Reintroduce
// that missing guard in an isolated header and require an observable-effects fail.
func TestCaptureReadinessRejectsUnguardedCapture(t *testing.T) {
	program := captureTestProgram(t, func(source string) string {
		return strings.Replace(source, "return CurrentWithLock(ticket);", "(void)ticket; return true;", 1)
	})
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), "unproven start touched machine configuration") {
		t.Fatalf("eager-capture negative control was not detected: %v\n%s", err, output)
	}
}

// The old native order installed routes before starting the consumer. This
// control must compile and fail at the injected route effect, not at compilation.
func TestCaptureReadinessRejectsCaptureBeforePump(t *testing.T) {
	program := captureTestProgram(t, func(source string) string {
		return strings.Replace(source,
			"{CaptureStage::Prepare, CaptureStage::Firewall,\n                             CaptureStage::Network, CaptureStage::Connected}",
			"{CaptureStage::Network, CaptureStage::Firewall,\n                             CaptureStage::Prepare, CaptureStage::Connected}", 1)
	})
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), "before the packet consumer was ready") {
		t.Fatalf("capture-before-pump negative control was not detected: %v\n%s", err, output)
	}
}

// Restore the old event-delivery dependency in a temporary production header:
// a network change invalidates proof only if a watcher is currently registered.
func TestCaptureReadinessRejectsLostNetworkEvent(t *testing.T) {
	program := captureTestProgram(t, func(source string) string {
		return strings.Replace(source,
			"session->NetworkChanged(eventMillis);\n    notify(session, eventMillis);",
			"if (notify(session, eventMillis)) session->NetworkChanged(eventMillis);", 1)
	})
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), "network event without a watcher retained pre-event proof") {
		t.Fatalf("lost-network-event negative control was not detected: %v\n%s", err, output)
	}
}
