// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os/exec"
	"path/filepath"
	"testing"
)

// The referral invitation text (App/ReferralShare.h) as a C++ spec.

// Compile and execute the referral invitation spec (App/ReferralShare.h): the
// copied invitation must carry the code's ur.io/c link after the message, so a
// friend on Android opens the app (or Play with the install referrer) with the
// code applied.
func TestReferralShare(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("referral share tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "referral-share-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "App"),
		filepath.Join(root, "app", "tools", "referral-share-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build referral share tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("referral share: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}
