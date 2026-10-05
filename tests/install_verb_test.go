// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
)

// Compile and execute the restart-on-failure access spec (Service/InstallVerb.h).
func TestFailureActionsAccess(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("install verb tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "install-verb-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "Service"),
		filepath.Join(root, "app", "tools", "install-verb-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build install verb tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("failure actions access: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The self-restart re-applies the failure actions before ending the process.
// Its service handle must be opened with the access that write needs
// (install::FailureActionsAccess, which carries SERVICE_START); a handle with
// SERVICE_CHANGE_CONFIG alone makes ChangeServiceConfig2W fail with
// ERROR_ACCESS_DENIED, so the service never restarted itself and the user was
// told to restart urnetworkd by hand.
func TestEnsureRestartOnFailureOpensWithStartAccess(t *testing.T) {
	root := repositoryRoot(t)
	source, err := os.ReadFile(filepath.Join(root, "app", "src", "Service", "main.cpp"))
	if err != nil {
		t.Fatal(err)
	}
	text := string(source)
	start := strings.Index(text, "bool EnsureRestartOnFailure() {")
	if start < 0 {
		t.Fatal("EnsureRestartOnFailure not found in main.cpp")
	}
	end := strings.Index(text[start:], "\n}\n")
	if end < 0 {
		t.Fatal("end of EnsureRestartOnFailure not found")
	}
	body := text[start : start+end]
	open := regexp.MustCompile(`OpenServiceW\(\s*scm,\s*ids::kServiceName,\s*([^)]*\)?)\s*\)`).FindStringSubmatch(body)
	if open == nil {
		t.Fatal("EnsureRestartOnFailure does not open the service with OpenServiceW")
	}
	if access := strings.TrimSpace(open[1]); access != "install::FailureActionsAccess()" {
		t.Fatalf("EnsureRestartOnFailure opens the service with %q; it must use "+
			"install::FailureActionsAccess() so the handle carries SERVICE_START", access)
	}
}

// The MSI (the Microsoft Store channel) registers urnetworkd with
// util:ServiceConfig. The SCM repeats the last failure action for every crash
// past the third in a reset period, so a "none" there leaves the machine with
// no service after its third crash in a day, until a reboot. Every action must
// restart, with the reset period and first delay of install::kFailureActions.
func TestInstallerServiceRestartsAfterEveryFailure(t *testing.T) {
	root := repositoryRoot(t)
	packageXML := parseXML(t, filepath.Join(root, "app", "installer", "Package.wxs"))
	var service *xmlNode
	for _, node := range packageXML.descendants(wixNamespace, "ServiceInstall") {
		if name, _ := node.attribute("Name"); name == "urnetworkd" {
			service = node
		}
	}
	if service == nil {
		t.Fatal("urnetworkd ServiceInstall is missing from Package.wxs")
	}
	configs := service.children(utilNamespace, "ServiceConfig")
	if len(configs) != 1 {
		t.Fatalf("urnetworkd has %d util:ServiceConfig elements; want 1", len(configs))
	}
	config := configs[0]
	for _, slot := range []string{"FirstFailureActionType", "SecondFailureActionType", "ThirdFailureActionType"} {
		if action, _ := config.attribute(slot); action != "restart" {
			t.Errorf("urnetworkd %s = %q; every failure action must be restart", slot, action)
		}
	}

	header, err := os.ReadFile(filepath.Join(root, "app", "src", "Service", "InstallVerb.h"))
	if err != nil {
		t.Fatal(err)
	}
	resetMatch := regexp.MustCompile(`kFailureResetPeriodSeconds\s*=\s*(\d+)\s*;`).FindSubmatch(header)
	delayMatch := regexp.MustCompile(`kFailureActions\[\]\s*=\s*\{\s*\{\s*true\s*,\s*(\d+)\s*\}`).FindSubmatch(header)
	if resetMatch == nil || delayMatch == nil {
		t.Fatal("kFailureResetPeriodSeconds or the first kFailureActions entry not found in InstallVerb.h")
	}
	resetSeconds, _ := strconv.Atoi(string(resetMatch[1]))
	firstDelayMs, _ := strconv.Atoi(string(delayMatch[1]))
	resetDays, _ := config.attribute("ResetPeriodInDays")
	if days, err := strconv.Atoi(resetDays); err != nil || days*86400 != resetSeconds {
		t.Errorf("urnetworkd ResetPeriodInDays = %q; want %d (kFailureResetPeriodSeconds %d)", resetDays, resetSeconds/86400, resetSeconds)
	}
	restartDelay, _ := config.attribute("RestartServiceDelayInSeconds")
	if seconds, err := strconv.Atoi(restartDelay); err != nil || seconds*1000 != firstDelayMs {
		t.Errorf("urnetworkd RestartServiceDelayInSeconds = %q; want %d (first kFailureActions delay %dms)", restartDelay, firstDelayMs/1000, firstDelayMs)
	}
}

// An MSI registration, an in-place update or an administrator's edit can leave
// a failure policy that is not install::kFailureActions. ServiceMain re-applies
// it once the control handler is registered and before the session runs, so
// every service start converges without waiting for an install verb or an MSI
// repair.
func TestServiceMainAppliesRestartOnFailure(t *testing.T) {
	root := repositoryRoot(t)
	source, err := os.ReadFile(filepath.Join(root, "app", "src", "Service", "main.cpp"))
	if err != nil {
		t.Fatal(err)
	}
	text := string(source)
	start := strings.Index(text, "void WINAPI ServiceMain(DWORD, LPWSTR*) {")
	if start < 0 {
		t.Fatal("ServiceMain not found in main.cpp")
	}
	end := strings.Index(text[start:], "\n}\n")
	if end < 0 {
		t.Fatal("end of ServiceMain not found")
	}
	var code strings.Builder
	for _, line := range strings.Split(text[start:start+end], "\n") {
		if comment := strings.Index(line, "//"); comment >= 0 {
			line = line[:comment]
		}
		code.WriteString(line + "\n")
	}
	body := code.String()
	register := strings.Index(body, "RegisterServiceCtrlHandlerExW(")
	ensure := regexp.MustCompile(`EnsureRestartOnFailure\(\)`).FindStringIndex(body)
	run := strings.Index(body, "Run();")
	if register < 0 || run < 0 {
		t.Fatalf("ServiceMain control handler registration or Run() not found: register=%d run=%d", register, run)
	}
	if ensure == nil {
		t.Fatal("ServiceMain never calls EnsureRestartOnFailure(); an MSI-registered service keeps the installer's failure actions")
	}
	if ensure[0] < register || ensure[0] > run {
		t.Fatalf("ServiceMain calls EnsureRestartOnFailure() at %d; want it after RegisterServiceCtrlHandlerExW (%d) and before Run() (%d)", ensure[0], register, run)
	}
}
