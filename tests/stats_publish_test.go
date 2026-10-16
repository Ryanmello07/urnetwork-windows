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

// How the app publishes and applies live stats snapshots: in the order their
// reads began (App/StatsReadOrder.h) as a C++ spec with negative controls
// that rewrite a copy of a header, and the wiring in SdkHost, AppController
// and ConnectPage, which need Windows and WinRT, read with every comment
// blanked so prose cannot satisfy a contract.

// The headers the spec compiles against.
var statsPublishHeaders = []string{"StatsReadOrder.h"}

// Compile the spec (app/tools/stats-publish-tests.cpp) against copies of its
// headers. `mutate`, when set, rewrites the copy of the header `name`.
func statsPublishTestProgram(t *testing.T, name string, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("stats publish tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	for _, header := range statsPublishHeaders {
		source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", header))
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
	program := filepath.Join(includeDir, "stats-publish-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
		"-I"+includeDir, filepath.Join(root, "app", "tools", "stats-publish-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build stats publish tests: %v\n%s", err, output)
	}
	return program
}

// Execute the spec: snapshots apply in the order their reads began.
func TestStatsPublish(t *testing.T) {
	vcxproj := readAppSource(t, "App.vcxproj")
	for _, header := range statsPublishHeaders {
		if !strings.Contains(vcxproj, `<ClInclude Include="`+header+`" />`) {
			t.Errorf("App.vcxproj does not list %s", header)
		}
	}
	program := statsPublishTestProgram(t, "", nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("stats publish: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Run the spec against a rewritten copy of `name` and require that it fails,
// naming want.
func requireStatsPublishFailure(t *testing.T, name string, mutate func(string) string, want string) {
	t.Helper()
	program := statsPublishTestProgram(t, name, mutate)
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

// The source with the one occurrence of old rewritten; the control fails when
// the header no longer has exactly one.
func replaceStatsPublishOnce(t *testing.T, source, old, replacement string) string {
	t.Helper()
	if strings.Count(source, old) != 1 {
		t.Fatalf("the header no longer has exactly one %q; update this control", old)
	}
	return strings.Replace(source, old, replacement, 1)
}

// The defect: every snapshot was applied in the order it arrived, so a read
// that waited on a getter rolled the window back when it finished.
func TestStatsReadOrderRejectsApplyingWhateverArrives(t *testing.T) {
	requireStatsPublishFailure(t, "StatsReadOrder.h", func(source string) string {
		return replaceStatsPublishOnce(t, source, "if (readSequence <= applied_) return false;", "")
	}, "the older read rolled the snapshot back")
}

// A repeated snapshot is not newer than itself.
func TestStatsReadOrderRejectsApplyingASnapshotTwice(t *testing.T) {
	requireStatsPublishFailure(t, "StatsReadOrder.h", func(source string) string {
		return replaceStatsPublishOnce(t, source, "readSequence <= applied_", "readSequence < applied_")
	}, "the same snapshot was applied twice")
}

// An order that forgets what it applied cannot tell an older read.
func TestStatsReadOrderRejectsForgettingTheAppliedRead(t *testing.T) {
	requireStatsPublishFailure(t, "StatsReadOrder.h", func(source string) string {
		return replaceStatsPublishOnce(t, source, "    applied_ = readSequence;\n", "")
	}, "the older read rolled the snapshot back")
}

// The text of `pattern`'s first match in text, or -1.
func statsIndex(text, pattern string) int {
	if location := regexp.MustCompile(pattern).FindStringIndex(text); location != nil {
		return location[0]
	}
	return -1
}

// Each read takes its sequence before it reads anything, and the UI applies a
// snapshot only when it is newer than every one it applied, before anything
// else in OnStats acts on it.
func TestStatsSnapshotsApplyInReadOrder(t *testing.T) {
	sdkHost := stripComments(readAppSource(t, "SdkHost.cpp"))
	read := definitionBody(t, "SdkHost.cpp", sdkHost, "LiveStats SdkHost::ReadStats()")
	sequence := statsIndex(read, `s\.readSequence = statsReadSequence_\.fetch_add\(`)
	if sequence < 0 {
		t.Fatal("ReadStats does not stamp its snapshot with a read sequence")
	}
	for _, firstRead := range []string{`service_\.IsConnected\(\)`, `connectVc_->`, `device_->`} {
		if at := statsIndex(read, firstRead); at >= 0 && at < sequence {
			t.Errorf("ReadStats reads %s before it takes its read sequence", firstRead)
		}
	}

	controller := stripComments(readAppSource(t, "AppController.cpp"))
	onStats := definitionBody(t, "AppController.cpp", controller, "void AppController::OnStats(const LiveStats& stats)")
	admit := statsIndex(onStats, `if \(!statsReadOrder_\.Admit\(stats\.readSequence\)\) return;`)
	if admit < 0 {
		t.Fatal("AppController::OnStats applies a snapshot whatever its read order")
	}
	for _, state := range []string{`trayHealth_`, `rawInsufficientBalance_`, `connectRequested_`, `OnStatsChanged`} {
		if at := statsIndex(onStats, state); at < 0 || at < admit {
			t.Errorf("AppController::OnStats acts on %s before it drops an older snapshot", state)
		}
	}
}
