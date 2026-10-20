// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The tunnel watchdog's settings (Service/TunnelWatchdogSettings.h) and the
// freeze rule that reads two of them, as a C++ spec with negative controls that
// rewrite a copy of a header. The watchdog itself needs Windows and the sdk;
// network_switch_wiring_test.go reads that it is built from these settings and
// reads them where they are used.

// The headers the spec compiles against: the settings and the two components'
// own settings they are built from.
var watchdogSettingsHeaders = []string{"TunnelWatchdogSettings.h", "NetworkChangeNotify.h", "CaptureReadiness.h"}

// Compile the spec (app/tools/watchdog-settings-tests.cpp) against copies of
// its headers. `mutate`, when set, rewrites the copy of the header `name`.
func watchdogSettingsTestProgram(t *testing.T, name string, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("watchdog settings tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	for _, header := range watchdogSettingsHeaders {
		source, err := os.ReadFile(filepath.Join(root, "app", "src", "Service", header))
		if err != nil {
			t.Fatal(err)
		}
		content := string(source)
		if mutate != nil && header == name {
			content = mutate(content)
			if content == string(source) {
				t.Fatalf("negative control did not change the production %s", header)
			}
		}
		if err := os.WriteFile(filepath.Join(includeDir, header), []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(includeDir, "watchdog-settings-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
		"-I"+includeDir, filepath.Join(root, "app", "tools", "watchdog-settings-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build watchdog settings tests: %v\n%s", err, output)
	}
	return program
}

// Execute the spec: the freeze gaps at the defaults, settings other than the
// defaults honoured, and a readiness that keeps the watchdog's capture timing.
func TestWatchdogSettings(t *testing.T) {
	project := readServiceSource(t, "Service.vcxproj")
	for _, header := range watchdogSettingsHeaders {
		if !strings.Contains(project, `<ClInclude Include="`+header+`" />`) {
			t.Errorf("Service.vcxproj does not list %s", header)
		}
	}
	program := watchdogSettingsTestProgram(t, "", nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("watchdog settings: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Run the spec against a rewritten copy of `name` and require that it fails,
// naming want.
func requireWatchdogSettingsFailure(t *testing.T, name string, mutate func(string) string, want string) {
	t.Helper()
	program := watchdogSettingsTestProgram(t, name, mutate)
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

// A stall told to the device as a network change: every load stall of 5 s or
// more kicked every transport and cancelled its dial.
func TestFreezeResponseRejectsKickingOnAStall(t *testing.T) {
	requireWatchdogSettingsFailure(t, "NetworkChangeNotify.h", func(source string) string {
		return strings.Replace(source,
			"  response.networkChanged = response.rebase && tickGapMillis >= networkChangeMillis;",
			"  response.networkChanged = response.rebase || tickGapMillis >= networkChangeMillis;", 1)
	}, "freeze: a 5 s stall rebases and is not a network change")
}

// A readiness that keeps its freshness window in a constant: the watchdog's
// capture timing, handed down by the controller, was never the one in force.
func TestWatchdogSettingsRejectsAReadinessThatIgnoresItsTiming(t *testing.T) {
	requireWatchdogSettingsFailure(t, "CaptureReadiness.h", func(source string) string {
		return strings.Replace(source, "    return 0 <= age && age <= settings_.freshnessMillis;",
			"    return 0 <= age && age <= 8000 + 0 * settings_.freshnessMillis;", 1)
	}, "capture: a readiness built from the watchdog's capture timing keeps it")
}
