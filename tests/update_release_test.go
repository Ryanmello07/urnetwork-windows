// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// Compile and execute the update checker's pure release decision and feed
// (Common/ReleaseSelection.h, App/Config.h) against the tag and MSI names the
// stable urnetwork/windows releases carry. mutateConfig, when set, rewrites
// Config.h in an isolated include dir for a negative control.
func updateReleaseTestProgram(t *testing.T, mutateConfig func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("update release tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	fixtureDir := t.TempDir()
	appDir := filepath.Join(root, "app", "src", "App")
	if mutateConfig != nil {
		source, err := os.ReadFile(filepath.Join(appDir, "Config.h"))
		if err != nil {
			t.Fatal(err)
		}
		changed := mutateConfig(string(source))
		if changed == string(source) {
			t.Fatal("negative control did not change the production config")
		}
		appDir = fixtureDir
		if err := os.WriteFile(filepath.Join(appDir, "Config.h"), []byte(changed), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(fixtureDir, "update-release-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "Common"),
		"-I"+appDir,
		filepath.Join(root, "app", "tools", "update-release-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build update release tests: %v\n%s", err, output)
	}
	return program
}

func TestUpdateReleaseSelection(t *testing.T) {
	program := updateReleaseTestProgram(t, nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("update release selection: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The update feed must be the stable urnetwork/windows releases: pointing it
// back at the nightly build repo or a personal fork has to fail the suite, not
// just a review.
func TestUpdateReleaseRejectsOtherFeeds(t *testing.T) {
	for _, tc := range []struct {
		name, repo, detected string
	}{
		{"nightly build repo", "urnetwork/build", "nightly builds, not the stable feed"},
		{"personal fork", "Ryanmello07/urnetwork-windows", "official urnetwork repo"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			program := updateReleaseTestProgram(t, func(source string) string {
				return strings.Replace(source, `L"urnetwork/windows"`, `L"`+tc.repo+`"`, 1)
			})
			output, err := exec.Command(program).CombinedOutput()
			if err == nil || !strings.Contains(string(output), "stable urnetwork/windows") ||
				!strings.Contains(string(output), tc.detected) {
				t.Fatalf("%s negative control was not detected: %v\n%s", tc.name, err, output)
			}
		})
	}
}
