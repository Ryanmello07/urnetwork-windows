// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// What each ending of the tray app stops in the service (Common/AppLifetime.h)
// as a C++ spec, with negative controls that rewrite a copy of the header.

// Compile the app-lifetime spec (app/tools/app-lifetime-tests.cpp) against
// Common/AppLifetime.h, the owner's 2026-10-05 decision on what each ending of
// the tray app stops in the service. `mutate`, when set, rewrites a copy of the
// header for a negative control.
func appLifetimeTestProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("app lifetime tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	fixtureDir := t.TempDir()
	includeDir := filepath.Join(root, "app", "src", "Common")
	if mutate != nil {
		source, err := os.ReadFile(filepath.Join(includeDir, "AppLifetime.h"))
		if err != nil {
			t.Fatal(err)
		}
		changed := mutate(string(source))
		if changed == string(source) {
			t.Fatal("negative control did not change the production AppLifetime.h")
		}
		includeDir = fixtureDir
		if err := os.WriteFile(filepath.Join(includeDir, "AppLifetime.h"), []byte(changed), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(fixtureDir, "app-lifetime-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+includeDir, filepath.Join(root, "app", "tools", "app-lifetime-tests.cpp"),
		"-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build app lifetime tests: %v\n%s", err, output)
	}
	return program
}

// Execute the spec: the tray's Quit and the end of the Windows session stop
// the tunnel and the provider in the service, a close request (the in-app
// update's installer closes the app with one) leaves the service as it is,
// and nothing else stops anything.
func TestAppLifetime(t *testing.T) {
	if !strings.Contains(readCommonSource(t, "Common.vcxproj"), `<ClInclude Include="AppLifetime.h" />`) {
		t.Error("Common.vcxproj does not list AppLifetime.h")
	}
	program := appLifetimeTestProgram(t, nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("app lifetime: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Run the spec against a rewritten copy of the header and require that it
// fails, naming want.
func requireAppLifetimeFailure(t *testing.T, mutate func(string) string, want string) {
	t.Helper()
	program := appLifetimeTestProgram(t, mutate)
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

// The defect: the tray's Quit left the session and the provider-only device
// running in the service. Put that back and the spec must fail.
func TestAppLifetimeRejectsQuitLeavingTheService(t *testing.T) {
	requireAppLifetimeFailure(t, func(source string) string {
		return strings.Replace(source,
			"    case Ending::Quit:\n      p.stopService = true;\n",
			"    case Ending::Quit:\n", 1)
	}, "quit: the service ends with no session and no provider-only device")
}

// Windows ending the session (a sign-out of Windows, a shutdown) stops the
// tunnel and the provider as Quit does (owner decision, 2026-10-05). Put back
// the old ending, which left them running for a user no longer signed in, and
// the spec must fail.
func TestAppLifetimeRejectsASessionEndLeavingTheService(t *testing.T) {
	requireAppLifetimeFailure(t, func(source string) string {
		return strings.Replace(source,
			"    case Ending::SessionEnd:\n      p.stopService = true;\n",
			"    case Ending::SessionEnd:\n", 1)
	}, "session end: a sign-out of Windows or a shutdown stops the tunnel and the provider")
}

// A WM_CLOSE from outside the app (taskkill, an installer) is nobody choosing
// Quit, and the update's MSI stops the service itself: it may not stop it.
func TestAppLifetimeRejectsStoppingWithoutAQuit(t *testing.T) {
	requireAppLifetimeFailure(t, func(source string) string {
		return strings.Replace(source,
			"    case Ending::CloseRequest:\n",
			"    case Ending::CloseRequest:\n      p.stopService = true;\n", 1)
	}, "close request: the service keeps what it runs")
}
