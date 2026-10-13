// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The call sites of the per-user urnetwork:// registration. What is written and what is left
// alone is decided by Common/ProtocolRegistration.h (protocol_registration_test.go); the registry
// glue (App/ProtocolHandler.cpp), the controller that calls it and the portable zip's README need
// Windows or PowerShell, so these read their sources with every comment blanked, as
// launch_at_startup_wiring_test.go does for "Launch URnetwork on system startup".

// The registry half: the user's own hive only, the shell's own answer as the proof, every write
// planned by the header and stopped at its first failure, and a plain Win32 unit with no pch.
func TestProtocolHandlerTheRegistration(t *testing.T) {
	raw := lf(readAppSource(t, "ProtocolHandler.cpp"))
	glue := stripComments(raw)
	for _, forbidden := range []string{"HKEY_LOCAL_MACHINE", "HKEY_CLASSES_ROOT", "HKEY_USERS", "HKEY_CURRENT_CONFIG", "winrt::", "RegCreateKeyExW(HKEY_LOCAL"} {
		if strings.Contains(glue, forbidden) {
			t.Errorf("ProtocolHandler.cpp mentions %q: the registration is the current user's alone, and plain Win32", forbidden)
		}
	}
	if strings.Contains(raw, `#include "pch.h"`) {
		t.Error(`ProtocolHandler.cpp includes pch.h: it is built without the precompiled header (App.vcxproj) so it stays plain Win32`)
	}
	provideRequire(t, "ProtocolHandler.cpp", glue,
		"::RegSetKeyValueW(HKEY_CURRENT_USER, path.c_str(),",
		"::RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name, flags, nullptr, nullptr, &bytes)",
		"::RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &handle)",
		"::AssocQueryStringW(ASSOCF_IS_PROTOCOL, ASSOCSTR_COMMAND,",
		`#pragma comment(lib, "shlwapi.lib")`,
		`#pragma comment(lib, "advapi32.lib")`,
		"constexpr protocol::Policy kPolicy{.adoptOtherUrnetworkExe = true};")
	// nothing there is not the same as not being able to look: only a key or a value that is not there is absent, and a
	// value that could not be read says so (readable) instead of reading as an empty key. (The functions that use these
	// are pinned whole in ur_io_approval_exact_test.go.)
	provideRequire(t, "ProtocolHandler.cpp", glue,
		"bool IsAbsent(LONG status) { return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND; }",
		"bool readable = true;")
	// the decisions are the header's: every write is one the plan names, in the plan's order, and the first
	// that fails stops the rest (the marker is the first, so what is left is still ours)
	apply := definitionBody(t, "ProtocolHandler.cpp", glue, "bool Apply(const protocol::Plan& plan) {")
	signOutRequireInOrder(t, "ProtocolHandler.cpp's Apply", apply,
		regexp.QuoteMeta("for (const protocol::Write& write : plan.writes) {"),
		regexp.QuoteMeta("::RegSetKeyValueW(HKEY_CURRENT_USER,"),
		regexp.QuoteMeta("if (written != ERROR_SUCCESS) return false;"),
		regexp.QuoteMeta("return true;"))
	if strings.Count(glue, "::RegSetKeyValueW(") != 1 {
		t.Error("ProtocolHandler.cpp writes the registry from more than Apply: every write is one the header's plan names")
	}
	ensure := definitionBody(t, "ProtocolHandler.cpp", glue, "bool EnsureProtocolHandler() {")
	signOutRequireInOrder(t, "EnsureProtocolHandler", ensure,
		regexp.QuoteMeta("const std::optional<protocol::UserKey> key = ReadUserKey();"),
		regexp.QuoteMeta("protocol::PlanEnsure(*key, exe, Widen(version::kString), kPolicy)"),
		regexp.QuoteMeta("plan.decision == protocol::Decision::RefuseForeign || plan.decision == protocol::Decision::RefuseUnsafePath"),
		regexp.QuoteMeta("if (!Apply(plan)) {"),
		regexp.QuoteMeta("const bool ours = ShellStartsThisExe(exe);"),
		regexp.QuoteMeta("return ours;"))
	refresh := definitionBody(t, "ProtocolHandler.cpp", glue, "void RefreshProtocolHandler() {")
	provideRequire(t, "RefreshProtocolHandler", refresh, "protocol::PlanRefresh(*key, exe, Widen(version::kString));")
	quitForbid(t, "RefreshProtocolHandler", refresh, "a launch never creates the registration or takes a key it did not write",
		"PlanEnsure(", "EnsureProtocolHandler(", "kPolicy")
	// nothing in the app removes the handler: the README says how, by hand, and a key is never deleted by the code
	quitForbid(t, "ProtocolHandler.cpp", glue, "the registration is written and refreshed here and never removed",
		"RegDeleteTreeW", "RegDeleteKey", "RegDeleteValue", "RegDeleteKeyValue", "RemoveProtocolHandler")
	// the log says closed words only
	for _, leak := range authLinkLogLeaks(glue) {
		t.Errorf("ProtocolHandler.cpp %s", leak)
	}
	if regexp.MustCompile(`Log(?:Info|Warn|Error)\([^;]*\b(?:exe|command|path)\b`).MatchString(authLinkStringLiteral.ReplaceAllString(glue, `""`)) {
		t.Error("ProtocolHandler.cpp logs a path or a command: its lines are closed words")
	}
}

// The calls: the registration is written when an attempt starts, before the browser is opened (only
// BeginApproval calls EnsureProtocolHandler, and a false answer ends the attempt), and brought up to
// date at each launch (only Start calls RefreshProtocolHandler, once). Nothing else registers the scheme:
// the installer is not touched (the MSI keeps its machine key) and no other source writes the key.
func TestProtocolHandlerCallSites(t *testing.T) {
	controller := stripComments(lf(readAppSource(t, "AppController.cpp")))
	begin := definitionBody(t, "AppController.cpp", controller, "std::optional<approval::Slot::View> AppController::BeginApproval() {")
	signOutRequireInOrder(t, "AppController::BeginApproval", begin,
		regexp.QuoteMeta("if (!EnsureProtocolHandler()) {"),
		regexp.QuoteMeta("return std::nullopt;"),
		regexp.QuoteMeta("approval::FreshKeyPair(approval::NewKeyPair)"))
	if count := strings.Count(controller, "EnsureProtocolHandler()"); count != 1 {
		t.Errorf("AppController.cpp calls EnsureProtocolHandler %d times; an attempt's start alone does", count)
	}
	if count := strings.Count(controller, "RefreshProtocolHandler();"); count != 1 {
		t.Errorf("AppController.cpp calls RefreshProtocolHandler %d times; Start alone does, once", count)
	}
	start := definitionBody(t, "AppController.cpp", controller, "void AppController::Start() {")
	provideRequire(t, "AppController::Start", start, "RefreshLaunchAtStartup();", "RefreshProtocolHandler();")
	provideRequireOrder(t, "AppController::Start", start, "RefreshLaunchAtStartup();", "RefreshProtocolHandler();")
	for name, code := range authLinkSourceFiles(t) {
		if name == "App/ProtocolHandler.cpp" || name == "App/ProtocolHandler.h" {
			continue
		}
		for _, call := range []string{"EnsureProtocolHandler(", "RefreshProtocolHandler(", "ProtocolHandlerStatus("} {
			if strings.Contains(code, call) && name != "App/AppController.cpp" {
				t.Errorf("%s calls %s: the per-user handler is written by an attempt's start, brought up to date at launch and asked about by the offer, from AppController alone", name, call)
			}
		}
		if strings.Contains(code, "Software\\\\Classes") && name != "Common/ProtocolRegistration.h" {
			t.Errorf("%s names the classes key: only the registrar (ProtocolRegistration.h) knows where the handler lives", name)
		}
	}
	if strings.Contains(controller, "RemoveProtocolHandler(") {
		t.Error("AppController.cpp removes the handler: there is no Settings row for it, the README says how to do it by hand")
	}
	// a key that starts another program is left alone, and then the action is not offered: the offer asks the registration
	// where it stands, once, and the only caller of that question is AppController::ApprovalOfferedIn (the decision for a network
	// that has been read: ApprovalOffered and BeginApproval both ask it)
	if count := strings.Count(controller, "ProtocolHandlerStatus()"); count != 1 {
		t.Errorf("AppController.cpp asks ProtocolHandlerStatus() %d times; ApprovalOfferedIn does, once", count)
	}
	offered := definitionBody(t, "AppController.cpp", controller, "bool AppController::ApprovalOfferedIn(const approval::Network& network) {")
	provideRequire(t, "AppController::ApprovalOfferedIn", offered, "switch (ProtocolHandlerStatus()) {")
	// the installer is not touched: it registers the machine key and nothing more
	data, err := os.ReadFile(filepath.Join(repositoryRoot(t), "app", "installer", "Package.wxs"))
	if err != nil {
		t.Fatal(err)
	}
	installer := lf(string(data))
	if strings.Contains(installer, "UrnwOwner") || strings.Contains(installer, `Root="HKCU" Key="Software\Classes\urnetwork"`) {
		t.Error("Package.wxs writes the per-user handler: the app does, and the MSI keeps to its machine key")
	}
}

// The portable zip's README says what the browser will ask, what the app registered, and how to take it
// away. It is an expandable here-string in a script that must read the same under Windows PowerShell 5.1
// (which reads an unmarked file as cp1252) and pwsh: ASCII only, and no '$' or backtick in the new lines.
func TestProtocolHandlerReadmeSaysHowToRemoveIt(t *testing.T) {
	path := filepath.Join(repositoryRoot(t), "app", "tools", "package-portable.ps1")
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	for at, b := range data {
		if b >= 0x80 {
			t.Fatalf("package-portable.ps1 has a non-ASCII byte at offset %d: it must parse the same under PowerShell 5.1 and pwsh", at)
		}
	}
	script := lf(string(data))
	start := strings.Index(script, "$readme = @\"")
	end := strings.Index(script, "\n\"@")
	if start < 0 || end < start {
		t.Fatal("package-portable.ps1 no longer builds its README from a here-string; update this contract")
	}
	readme := script[start:end]
	provideRequire(t, "the portable zip's README", readme, `"Open URnetwork?"`, "reg delete HKCU\\Software\\Classes\\urnetwork /f", "current Windows user only")
	// the lines added for the browser sign-in carry nothing PowerShell would expand, and say that the button they describe
	// is off until the approval page is live: this build's testers are not promised a button the build does not show
	first := strings.Index(readme, `If this build offers "Continue in browser"`)
	last := strings.Index(readme, "Updating\n--------")
	if first < 0 || last < first {
		t.Fatal("the README's browser sign-in paragraph is missing")
	}
	paragraph := readme[first:last]
	if strings.ContainsAny(paragraph, "$`") {
		t.Errorf("the README's browser sign-in paragraph has a '$' or a backtick, which the here-string would expand:\n%s", paragraph)
	}
	provideRequire(t, "the README's browser sign-in paragraph", paragraph, "switched off until the approval page is live", "Copy button")
	// and the repository's README says where the per-user key is and that an uninstall leaves it
	repoReadme := lf(readFileOrFail(t, filepath.Join(repositoryRoot(t), "README.md")))
	provideRequire(t, "README.md", repoReadme, "reg delete HKCU\\Software\\Classes\\urnetwork /f", "an MSI uninstall does not")
}

func readFileOrFail(t *testing.T, path string) string {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return string(data)
}
