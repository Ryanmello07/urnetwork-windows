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

// The SN payout line and settlement v2 (App/SnPayoutPresentation.h): the C++
// spec, the page's wiring and the strings.

// Builds the C++ spec (tools/sn-payout-tests.cpp) with `extra` arguments.
func buildSnPayoutTests(t *testing.T, extra ...string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("sn payout tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	appDir := filepath.Join(root, "app", "src", "App")
	program := filepath.Join(t.TempDir(), "sn-payout-tests")
	arguments := []string{"-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + appDir}
	arguments = append(arguments, extra...)
	arguments = append(arguments,
		filepath.Join(root, "app", "tools", "sn-payout-tests.cpp"),
		filepath.Join(appDir, "SnPayoutPresentation.cpp"),
		filepath.Join(appDir, "SolanaWalletPresentation.cpp"),
		"-o", program)
	if output, err := exec.Command(compiler, arguments...).CombinedOutput(); err != nil {
		t.Fatalf("build sn payout tests: %v\n%s", err, output)
	}
	return program
}

func runSnPayoutTests(t *testing.T, program string) {
	t.Helper()
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("sn payout: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Compile and execute the SN payout line spec (App/SnPayoutPresentation.h):
// provider payouts moved to the UR subnet, and Earnings says how and when a
// provider is paid under the points figure, with the current epoch's times
// from the SDK's epoch schedule, Claim while something is claimable and the
// coldkey prompt while none is set. The final USDC payout line shows only while
// USDC is pending. The same program is settlement v2's spec (APPS-SPEC.md, the
// UX rules): the section for every state of the read, configured or not,
// pending as pending and unavailable never as a zero, and the v2 claim
// dialog's progress.
func TestSnPayoutLine(t *testing.T) {
	root := repositoryRoot(t)
	appDir := filepath.Join(root, "app", "src", "App")
	runSnPayoutTests(t, buildSnPayoutTests(t))

	// the page draws the decision, with the schedule the claims read carries,
	// and its actions are the existing claim dialog and coldkey flow
	read := func(path string) string {
		source, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		return string(source)
	}
	walletPage := read(filepath.Join(appDir, "WalletPage.cpp"))
	for _, want := range []string{
		"snpayout::PayoutLineFor(",
		"result->schedule->claim_open_millis",
		`"sn_payout_schedule"`,
		`"sn_payout_schedule_times"`,
		`"set_coldkey_to_get_paid"`,
		`"set_coldkey"`,
	} {
		if !strings.Contains(walletPage, want) {
			t.Errorf("WalletPage.cpp: missing %s", want)
		}
	}
	mainWindow := read(filepath.Join(appDir, "MainWindow.xaml"))
	for _, want := range []*regexp.Regexp{
		regexp.MustCompile(`x:Name="SetColdkeyButton"[^>]*Click="OnConnectWallet"`),
		regexp.MustCompile(`x:Name="SnPayoutClaimButton"[^>]*Click="OnClaimAlpha"`),
	} {
		if !want.MatchString(mainWindow) {
			t.Errorf("MainWindow.xaml: missing %s", want)
		}
	}
	if project := read(filepath.Join(appDir, "App.vcxproj")); !strings.Contains(project, `Include="SnPayoutPresentation.cpp"`) {
		t.Error("App.vcxproj does not compile SnPayoutPresentation.cpp")
	}

	// the en resources: the line's strings, the USDC line labeled as the final
	// USDC payout, and no payout string that fixes the claim window
	resw := read(filepath.Join(appDir, "Strings", "en", "Resources.resw"))
	value := func(key string) string {
		match := regexp.MustCompile(`<data name="` + key + `" xml:space="preserve">\s*<value>([^<]*)</value>`).FindStringSubmatch(resw)
		if match == nil {
			t.Errorf("Resources.resw: no %s", key)
			return ""
		}
		return match[1]
	}
	if got, want := value("sn_payout_schedule_times"), "This epoch ends {0}. Claim your share from {1} until {2}."; got != want {
		t.Errorf("sn_payout_schedule_times = %q, want %q", got, want)
	}
	if got, want := value("usdc_waiting"), "Final USDC payout: {} USDC waiting"; got != want {
		t.Errorf("usdc_waiting = %q, want %q", got, want)
	}
	duration := regexp.MustCompile(`\d+ (hours?|days?|weeks?|epochs?)`)
	for _, key := range []string{"sn_payout_schedule", "set_coldkey_to_get_paid", "set_coldkey", "claims_open_after_finalization"} {
		if v := value(key); duration.MatchString(v) {
			t.Errorf("%s fixes a duration: %q", key, v)
		}
	}
	if strings.Contains(resw, `name="payouts_amount_threshold"`) {
		t.Error("Resources.resw still carries the retired payouts_amount_threshold")
	}
}

// The v2 boundary templates against the generated SDK header: WalletPage
// instantiates snpayout::LedgerAccountFrom with urnet::SnLedgerAccount and
// snpayout::LedgerClaimCallbackFor with urnet::SnLedgerClaimCallback, so the
// spec builds both with the header's own types and reads an account from the
// json the device answers with. The header is git-ignored (fetch-deps unpacks
// it into app/third_party/urnetwork-sdk/<arch>; URNETWORK_SDK_INCLUDE names
// another directory) and needs nlohmann/json, so a host without them skips
// this, and so does a copy from before the v2 claim.
func TestSnPayoutAgainstSdkHeader(t *testing.T) {
	root := repositoryRoot(t)
	sdkDir := sdkHeaderDirWith(t, root, "SnLedgerClaimCallback")
	if sdkDir == "" {
		t.Skip("no urnetwork_sdk.hpp with the v2 claim (set URNETWORK_SDK_INCLUDE)")
	}
	jsonDir, found := jsonIncludeDir(root)
	if !found {
		t.Skip("no nlohmann/json.hpp (set URNETWORK_JSON_INCLUDE)")
	}
	// System includes: the generated wrapper does not build with -Wextra -Werror.
	extra := []string{"-DURNW_SN_PAYOUT_TESTS_SDK", "-isystem", sdkDir}
	if jsonDir != "" {
		extra = append(extra, "-isystem", jsonDir)
	}
	t.Logf("against %s", filepath.Join(sdkDir, "urnetwork_sdk.hpp"))
	runSnPayoutTests(t, buildSnPayoutTests(t, extra...))
}
