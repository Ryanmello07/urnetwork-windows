// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The upgrade sheet's checkout page hands a failure back with its own code
// (the sdk's CheckoutBridgeError*, passed on by urnet::parseCheckoutRedirect)
// next to its English text. A code the app knows reads in the app's words
// (App/CheckoutBridgeError.h, compiled and run here); any other failure reads
// in the page's text, and one without a text says something went wrong.
func TestCheckoutBridgeErrorCodes(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("checkout bridge error tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	appDir := filepath.Join(root, "app", "src", "App")
	program := filepath.Join(t.TempDir(), "checkout-bridge-error-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+appDir, filepath.Join(root, "app", "tools", "checkout-bridge-error-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build checkout bridge error tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("checkout bridge error: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}

	sheets := stripLineComments(readAppSource(t, "BalanceSheets.cpp"))
	callback := functionBody(sheets, "void UpgradeSheet::HandleCheckoutCallback(")
	for _, want := range []string{
		"errorCode = redirect->ErrorCode;",
		"CheckoutFailureTextFor(errorCode, errorMessage)",
		"failure.key.empty() ? H(failure.pageText) : Loc(failure.key)",
	} {
		if !strings.Contains(callback, want) {
			t.Errorf("BalanceSheets.cpp HandleCheckoutCallback: missing %s", want)
		}
	}
	if !strings.Contains(readAppSource(t, "App.vcxproj"), `<ClInclude Include="CheckoutBridgeError.h" />`) {
		t.Error("App.vcxproj does not list CheckoutBridgeError.h")
	}

	// the two strings are translated in every language
	entries, err := os.ReadDir(filepath.Join(appDir, "Strings"))
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"checkout_error_unavailable", "checkout_error_payment_form_unavailable"} {
		englishValue := reswValue(t, root, "en", name)
		if englishValue == "" {
			t.Errorf("en/Resources.resw has no %s", name)
			continue
		}
		for _, entry := range entries {
			if !entry.IsDir() || entry.Name() == "en" {
				continue
			}
			if value := reswValue(t, root, entry.Name(), name); value == "" {
				t.Errorf("%s/Resources.resw has no %s", entry.Name(), name)
			} else if value == englishValue {
				t.Errorf("%s/Resources.resw %s is English", entry.Name(), name)
			}
		}
	}
}

// The codes CheckoutBridgeError.h knows are the sdk's own
// (urnet::CheckoutBridgeError*). Skipped without an sdk header that has them
// (see sdkHeaderWith).
func TestCheckoutBridgeCodesMatchTheSdkHeader(t *testing.T) {
	headerSource := sdkHeaderWith(t, "CheckoutBridgeError")
	errorSource := readAppSource(t, "CheckoutBridgeError.h")
	for name, code := range map[string]string{
		"CheckoutBridgeErrorUnavailable":       "checkout_unavailable",
		"CheckoutBridgeErrorStripeUnavailable": "stripe_unavailable",
	} {
		if !strings.Contains(headerSource, `inline constexpr const char* `+name+` = "`+code+`";`) {
			t.Errorf("urnetwork_sdk.hpp: urnet::%s is not %q", name, code)
		}
		if !strings.Contains(errorSource, `if (code == "`+code+`")`) {
			t.Errorf("CheckoutBridgeError.h: CheckoutFailureTextFor does not know the sdk's %q", code)
		}
	}
}

// The sdk's C++ header (urnetwork_sdk.hpp) when it has `marker`, else the test
// is skipped. The header is git-ignored: fetch-deps unpacks it into
// app/third_party/urnetwork-sdk/<arch>, and URNETWORK_SDK_INCLUDE names another
// directory, so a host without one, or with one from before the marker, skips.
func sdkHeaderWith(t *testing.T, marker string) string {
	t.Helper()
	root := repositoryRoot(t)
	headerDirs := []string{}
	if explicit := os.Getenv("URNETWORK_SDK_INCLUDE"); explicit != "" {
		headerDirs = append(headerDirs, explicit)
	}
	for _, arch := range []string{"amd64", "arm64"} {
		headerDirs = append(headerDirs, filepath.Join(root, "app", "third_party", "urnetwork-sdk", arch))
	}
	for _, dir := range headerDirs {
		data, err := os.ReadFile(filepath.Join(dir, "urnetwork_sdk.hpp"))
		if err == nil && strings.Contains(string(data), marker) {
			t.Logf("against %s", filepath.Join(dir, "urnetwork_sdk.hpp"))
			return string(data)
		}
	}
	t.Skipf("no urnetwork_sdk.hpp with %s (set URNETWORK_SDK_INCLUDE)", marker)
	return ""
}
