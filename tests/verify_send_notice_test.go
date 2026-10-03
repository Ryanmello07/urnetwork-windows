// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// Compile and execute the verify send notice spec (App/VerifySendNotice.h): a
// verification code the server reports it did not send (send failed, rate
// limited) must not read "code sent".
func TestVerifySendNotice(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("verify send notice tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "verify-send-notice-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "App"),
		filepath.Join(root, "app", "tools", "verify-send-notice-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build verify send notice tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("verify send notice: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The app carries the server's send error into the verify step: sign in and
// create read verification_required.send_error, the resend asks for
// result_errors and reads result.error, and the en resources carry the keys.
func TestVerifySendNoticeWiring(t *testing.T) {
	root := repositoryRoot(t)
	app := filepath.Join(root, "app", "src", "App")
	read := func(name string) string {
		source, err := os.ReadFile(filepath.Join(app, name))
		if err != nil {
			t.Fatal(err)
		}
		return string(source)
	}
	sdkHost := read("SdkHost.cpp")
	if n := strings.Count(sdkHost, "VerifySendNoticeOf(result->verification_required->send_error)"); n != 2 {
		t.Errorf("SdkHost.cpp: send_error is read for %d of the 2 verification results (login with password, network create)", n)
	}
	for _, want := range []string{"args.result_errors = true;", "VerifySendNoticeOf(result->error)"} {
		if !strings.Contains(sdkHost, want) {
			t.Errorf("SdkHost.cpp: missing %s", want)
		}
	}
	loginPage := read("LoginPage.cpp")
	if n := strings.Count(loginPage, "ShowVerifySendError(r.verify_send)"); n != 2 {
		t.Errorf("LoginPage.cpp: the send error is shown after %d of the 2 verify routes (sign in, create)", n)
	}
	if !strings.Contains(loginPage, "ShowVerifySendError(notice)") {
		t.Error("LoginPage.cpp: the resend does not show the send error")
	}
	resources := read(filepath.Join("Strings", "en", "Resources.resw"))
	for _, want := range []string{
		`name="error_sending_verification_code"`,
		`name="verify_code_rate_limited.one"`,
		`name="verify_code_rate_limited.other"`,
	} {
		if !strings.Contains(resources, want) {
			t.Errorf("en Resources.resw: missing %s", want)
		}
	}
}
