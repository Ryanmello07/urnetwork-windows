// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// Compile and execute the Seeker verification notice spec
// (App/SeekerVerifyNotice.h): a server answer that found no Seeker or Saga
// token must say seeker_token_not_found, not the generic claim error.
func TestSeekerVerifyNotice(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("seeker verify notice tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "seeker-verify-notice-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "App"),
		filepath.Join(root, "app", "tools", "seeker-verify-notice-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build seeker verify notice tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("seeker verify notice: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
	// the NotHolder notice is shown with the key, and the en resources carry it
	for path, want := range map[string]string{
		filepath.Join(root, "app", "src", "App", "WalletPage.cpp"):                  `"seeker_token_not_found"`,
		filepath.Join(root, "app", "src", "App", "Strings", "en", "Resources.resw"): `name="seeker_token_not_found"`,
	} {
		source, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(string(source), want) {
			t.Errorf("%s: missing %s", path, want)
		}
	}
}
