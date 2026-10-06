// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The marker that refuses launches while the in-app updater's installer runs
// (Common/UpdateMarker.h) as a C++ spec, with negative controls that put each
// defect back into a copy of the header.

// Compile the update marker spec (app/tools/update-marker-tests.cpp) against
// Common/UpdateMarker.h. `mutate`, when set, rewrites a copy of the header for
// a negative control.
func updateMarkerTestProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("update marker tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	fixtureDir := t.TempDir()
	includeDir := filepath.Join(root, "app", "src", "Common")
	if mutate != nil {
		source, err := os.ReadFile(filepath.Join(includeDir, "UpdateMarker.h"))
		if err != nil {
			t.Fatal(err)
		}
		changed := mutate(string(source))
		if changed == string(source) {
			t.Fatal("negative control did not change the production UpdateMarker.h")
		}
		includeDir = fixtureDir
		if err := os.WriteFile(filepath.Join(includeDir, "UpdateMarker.h"), []byte(changed), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(fixtureDir, "update-marker-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+includeDir, filepath.Join(root, "app", "tools", "update-marker-tests.cpp"),
		"-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build update marker tests: %v\n%s", err, output)
	}
	return program
}

// Execute the spec: a launch during an update is refused while the installer
// runs; a marker left by an update that is over, failed, crashed or never
// returned refuses nothing for long; and the marker reads back from its file
// as it was written.
func TestUpdateMarker(t *testing.T) {
	if !strings.Contains(readCommonSource(t, "Common.vcxproj"), `<ClInclude Include="UpdateMarker.h" />`) {
		t.Error("Common.vcxproj does not list UpdateMarker.h")
	}
	program := updateMarkerTestProgram(t, nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("update marker: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Run the spec against a rewritten copy of the header and require that it
// fails, naming want.
func requireUpdateMarkerFailure(t *testing.T, mutate func(string) string, want string) {
	t.Helper()
	program := updateMarkerTestProgram(t, mutate)
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

// The source with the one occurrence of old replaced; a control whose text no
// longer matches fails rather than passing vacuously.
func updateMarkerReplace(t *testing.T, source, old, replacement string) string {
	t.Helper()
	if strings.Count(source, old) != 1 {
		t.Fatalf("negative control expects exactly one %q in UpdateMarker.h", old)
	}
	return strings.Replace(source, old, replacement, 1)
}

// The defect: nothing refused a launch during an update. Put that back and
// the spec must fail.
func TestUpdateMarkerRejectsALaunchThatStartsDuringAnUpdate(t *testing.T) {
	requireUpdateMarkerFailure(t, func(source string) string {
		return updateMarkerReplace(t, source,
			"  if (installer == InstallerState::Ended) return Verdict::Stale;\n  return Verdict::Updating;\n",
			"  if (installer == InstallerState::Ended) return Verdict::Stale;\n  return Verdict::None;\n")
	}, "updating: while the updater's installer runs, a launch is refused")
}

// A marker must never refuse launches for good: not once its installer has
// ended, not past its lifetime, not when it says it was written far in the
// future, and not when it does not parse. Remove any of those, and the spec
// must fail.
func TestUpdateMarkerRejectsAMarkerThatOutlivesItsUpdate(t *testing.T) {
	requireUpdateMarkerFailure(t, func(source string) string {
		return updateMarkerReplace(t, source,
			"  if (installer == InstallerState::Ended) return Verdict::Stale;\n", "  (void)installer;\n")
	}, "stale: once the installer has ended")
	requireUpdateMarkerFailure(t, func(source string) string {
		return updateMarkerReplace(t, source,
			"  if (age > kUpdateMarkerLifetime.count() || age < -kUpdateMarkerClockSkew.count()) {\n",
			"  if (age < -kUpdateMarkerClockSkew.count()) {\n")
	}, "stale: past its lifetime a marker no longer refuses")
	requireUpdateMarkerFailure(t, func(source string) string {
		return updateMarkerReplace(t, source,
			"  if (age > kUpdateMarkerLifetime.count() || age < -kUpdateMarkerClockSkew.count()) {\n",
			"  if (age > kUpdateMarkerLifetime.count()) {\n")
	}, "stale: a marker far in the future cannot refuse launches for good")
	requireUpdateMarkerFailure(t, func(source string) string {
		return updateMarkerReplace(t, source,
			"  if (!marker) return Verdict::Stale;\n", "  if (!marker) return Verdict::None;\n")
	}, "stale: a file that does not parse is stale")
}

// A process id is reused once its process ends, so the marker keeps the
// installer's creation time too. Accept a marker without one, and the spec
// must fail.
func TestUpdateMarkerRejectsAMarkerWithoutTheInstallersCreationTime(t *testing.T) {
	requireUpdateMarkerFailure(t, func(source string) string {
		return updateMarkerReplace(t, source,
			"  if (marker.installerProcessId == 0 || marker.installerCreationTime == 0) return std::nullopt;\n",
			"  if (marker.installerProcessId == 0) return std::nullopt;\n")
	}, "file: '4321 0 1900000000' is not a marker")
}
