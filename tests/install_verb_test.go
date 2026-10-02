// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// Compile and execute the restart-on-failure access spec (Service/InstallVerb.h).
func TestFailureActionsAccess(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("install verb tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "install-verb-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "Service"),
		filepath.Join(root, "app", "tools", "install-verb-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build install verb tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("failure actions access: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The self-restart re-applies the failure actions before ending the process.
// Its service handle must be opened with the access that write needs
// (install::FailureActionsAccess, which carries SERVICE_START); a handle with
// SERVICE_CHANGE_CONFIG alone makes ChangeServiceConfig2W fail with
// ERROR_ACCESS_DENIED, so the service never restarted itself and the user was
// told to restart urnetworkd by hand.
func TestEnsureRestartOnFailureOpensWithStartAccess(t *testing.T) {
	root := repositoryRoot(t)
	source, err := os.ReadFile(filepath.Join(root, "app", "src", "Service", "main.cpp"))
	if err != nil {
		t.Fatal(err)
	}
	text := string(source)
	start := strings.Index(text, "bool EnsureRestartOnFailure() {")
	if start < 0 {
		t.Fatal("EnsureRestartOnFailure not found in main.cpp")
	}
	end := strings.Index(text[start:], "\n}\n")
	if end < 0 {
		t.Fatal("end of EnsureRestartOnFailure not found")
	}
	body := text[start : start+end]
	open := regexp.MustCompile(`OpenServiceW\(\s*scm,\s*ids::kServiceName,\s*([^)]*\)?)\s*\)`).FindStringSubmatch(body)
	if open == nil {
		t.Fatal("EnsureRestartOnFailure does not open the service with OpenServiceW")
	}
	if access := strings.TrimSpace(open[1]); access != "install::FailureActionsAccess()" {
		t.Fatalf("EnsureRestartOnFailure opens the service with %q; it must use "+
			"install::FailureActionsAccess() so the handle carries SERVICE_START", access)
	}
}
