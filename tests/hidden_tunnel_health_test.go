// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The connection health while the window is hidden (Common/ConnectionHealth.h,
// owner report 2026-10-16) as a C++ spec, with negative controls that rewrite
// a copy of the header, and ReadStats' lines that mark and fold a status from
// the location alone, read with every comment blanked.

func hiddenTunnelHealthProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("hidden tunnel health tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	source, err := os.ReadFile(filepath.Join(root, "app", "src", "Common", "ConnectionHealth.h"))
	if err != nil {
		t.Fatal(err)
	}
	content := string(source)
	if mutate != nil {
		content = mutate(content)
		if content == string(source) {
			t.Fatal("negative control did not change the production ConnectionHealth.h")
		}
	}
	if err := os.WriteFile(filepath.Join(includeDir, "ConnectionHealth.h"), []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	program := filepath.Join(includeDir, "hidden-tunnel-health-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I"+includeDir,
		filepath.Join(root, "app", "tools", "hidden-tunnel-health-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build hidden tunnel health tests: %v\n%s", err, output)
	}
	return program
}

// The spec: a location set while hidden reads connected once the tunnel is up
// and connecting while it prepares; the window's evidence holds across a hide;
// a failed verdict stands while it lasts; the feed takes over on show.
func TestHiddenTunnelHealth(t *testing.T) {
	if output, err := exec.Command(hiddenTunnelHealthProgram(t, nil)).CombinedOutput(); err != nil {
		t.Fatalf("hidden tunnel health: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

func requireHiddenTunnelHealthFailure(t *testing.T, old, replacement, want string) {
	t.Helper()
	program := hiddenTunnelHealthProgram(t, func(source string) string {
		if strings.Count(source, old) != 1 {
			t.Fatalf("ConnectionHealth.h no longer has exactly one %q; update this control", old)
		}
		return strings.Replace(source, old, replacement, 1)
	})
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

// The defect: a destination set from the location alone read as a connect in
// progress, so the tray said Connecting over a working tunnel while hidden.
func TestHiddenTunnelHealthRejectsTheLocationReadAsConnecting(t *testing.T) {
	requireHiddenTunnelHealthFailure(t,
		"(in.statusFromLocation && in.activity == Activity::Connecting)",
		"(in.statusFromLocation && false)",
		"a location set while hidden reads connected once the tunnel is up")
}

// Without the failed verdict while hidden, a failed attempt would read as the
// connection under the location rule.
func TestHiddenTunnelHealthRejectsIgnoringTheFailedVerdict(t *testing.T) {
	requireHiddenTunnelHealthFailure(t,
		"      if (in.windowFailed && !proven_) return state_ = State::Failed;\n", "",
		"the SDK's failed verdict stands while hidden, not connected")
}

// ReadStats marks a status from the location alone, clears the mark in the
// rpc-only and service-down clamps, and folds it into the tracker.
func TestHiddenTunnelHealthWiring(t *testing.T) {
	if !strings.Contains(readCommonSource(t, "Common.vcxproj"), `<ClInclude Include="ConnectionHealth.h" />`) {
		t.Error("Common.vcxproj does not list ConnectionHealth.h")
	}
	host := stripComments(readAppSource(t, "SdkHost.cpp"))
	read := functionBody(host, "LiveStats SdkHost::ReadStats(")
	quitRequireSequence(t, "SdkHost::ReadStats", read,
		`\} else if \(device_ && serviceUp\) \{`,
		`s\.connectionStatus = s\.connected \? "DESTINATION_SET" : "DISCONNECTED";`,
		`s\.statusFromLocation = s\.connected;`,
		`s\.connectionStatus = "RPC_ONLY";`, `s\.statusFromLocation = false;`,
		`s\.connectionStatus = "SERVICE_DOWN";`,
		`hs\.statusFromLocation = s\.statusFromLocation;`, `healthTracker_\.Update\(hs, nowMillis\)`)
	down := read[strings.Index(read, `s.connectionStatus = "SERVICE_DOWN";`):]
	if !strings.Contains(down[:strings.Index(down, "health::Signals hs;")], "s.statusFromLocation = false;") {
		t.Error("ReadStats' service-down clamp must clear the location mark too")
	}
}
