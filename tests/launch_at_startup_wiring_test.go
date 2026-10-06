// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The call sites of "Launch URnetwork on system startup" (owner decision,
// 2026-10-05: like the macOS setting, with its default). What the toggle and a
// launch write is decided by Common/StartupRegistration.h
// (startup_registration_test.go); the Settings page, the registry glue and the
// installer need Windows, so these read their sources with every comment
// blanked.

// The Settings toggle, as macOS has it: labelled with
// launch_urnetwork_on_system_startup (tagged for windows), showing what
// Windows has, writing it when toggled, and going back to the real state when
// the write fails. Its value is read again whenever the page loads, since
// Task Manager can switch it off too.
func TestLaunchAtStartupTheSettingsToggle(t *testing.T) {
	settings := stripComments(readAppSource(t, "SettingsPage.cpp"))
	general := definitionBody(t, "SettingsPage.cpp", settings, "void SettingsPage::BuildGeneralSection(Panel const& host) {")
	signOutRequireInOrder(t, "SettingsPage::BuildGeneralSection", general,
		regexp.QuoteMeta(`launchAtStartup_ = ToggleRow(card, Loc("launch_urnetwork_on_system_startup"), hstring{});`),
		regexp.QuoteMeta("ApplyLaunchAtStartup();"),
		regexp.QuoteMeta("launchAtStartup_.Toggled([this](auto const&, auto const&) { OnLaunchAtStartupToggled(); });"))
	apply := definitionBody(t, "SettingsPage.cpp", settings, "void SettingsPage::ApplyLaunchAtStartup() {")
	signOutRequireInOrder(t, "SettingsPage::ApplyLaunchAtStartup", apply,
		regexp.QuoteMeta("applyingLaunchAtStartup_ = true;"),
		regexp.QuoteMeta("launchAtStartup_.IsOn(urnw::LaunchAtStartupEnabled());"),
		regexp.QuoteMeta("applyingLaunchAtStartup_ = false;"))
	toggled := definitionBody(t, "SettingsPage.cpp", settings, "void SettingsPage::OnLaunchAtStartupToggled() {")
	signOutRequireInOrder(t, "SettingsPage::OnLaunchAtStartupToggled", toggled,
		regexp.QuoteMeta("if (applyingLaunchAtStartup_) return;"),
		regexp.QuoteMeta("if (urnw::SetLaunchAtStartup(wanted)) return;"),
		regexp.QuoteMeta("ApplyLaunchAtStartup();"),
		regexp.QuoteMeta(`snackbar_.Show(Loc("something_went_wrong"), InfoBarSeverity::Error);`))
	local := definitionBody(t, "SettingsPage.cpp", settings, "void SettingsPage::ApplyLocalDeviceState() {")
	provideRequire(t, "SettingsPage::ApplyLocalDeviceState", local, "ApplyLaunchAtStartup();")
	keys := readAppSource(t, filepath.Join("Strings", "windows-keys.txt"))
	if !regexp.MustCompile(`(?m)^launch_urnetwork_on_system_startup$`).MatchString(keys) {
		t.Error("windows-keys.txt does not list launch_urnetwork_on_system_startup: tag it for windows " +
			"in the localizations store and regenerate")
	}
}

// Off until the user turns it on, as on macOS: the toggle is the only place
// that turns it on or off, and a launch only brings an existing registration
// up to date (AppController::Start), so a first run registers nothing.
func TestLaunchAtStartupNothingRegistersByDefault(t *testing.T) {
	setters := 0
	for name, source := range appSourceFiles(t, ".cpp") {
		code := stripComments(source)
		if name != "LaunchAtStartup.cpp" && strings.Contains(code, "SetLaunchAtStartup(") {
			setters++
			if name != "SettingsPage.cpp" {
				t.Errorf("%s turns launch on system startup on or off: only the Settings toggle may", name)
			}
		}
		if name != "LaunchAtStartup.cpp" &&
			(strings.Contains(code, "kStartupRunKey") || strings.Contains(code, "kStartupApprovedKey")) {
			t.Errorf("%s writes the startup registration itself: it belongs to LaunchAtStartup.cpp", name)
		}
	}
	if setters != 1 {
		t.Errorf("SetLaunchAtStartup is called from %d files, want the Settings page alone", setters)
	}
	controller := appControllerSource(t)
	start := definitionBody(t, "AppController.cpp", controller, "void AppController::Start() {")
	provideRequire(t, "AppController::Start", start, "RefreshLaunchAtStartup();")
	if strings.Count(controller, "RefreshLaunchAtStartup();") != 1 {
		t.Error("AppController.cpp must bring the registration up to date in Start alone")
	}
}

// The registry half: the user's own hive only, the Run value and Task
// Manager's record of it by their shared names, a command line that is an
// autostart, and each write planned by StartupRegistration.h.
func TestLaunchAtStartupTheRegistration(t *testing.T) {
	glue := stripComments(readAppSource(t, "LaunchAtStartup.cpp"))
	if strings.Contains(glue, "HKEY_LOCAL_MACHINE") {
		t.Error("LaunchAtStartup.cpp touches HKEY_LOCAL_MACHINE: the registration is per user")
	}
	command := definitionBody(t, "LaunchAtStartup.cpp", glue, "std::wstring Command() {")
	provideRequire(t, "LaunchAtStartup.cpp's Command", command,
		"startup::RunCommand(exe, instance::kAutostartArgument)")
	set := definitionBody(t, "LaunchAtStartup.cpp", glue, "bool SetLaunchAtStartup(bool enabled) {")
	signOutRequireInOrder(t, "SetLaunchAtStartup", set,
		regexp.QuoteMeta("const std::wstring command = Command();"),
		regexp.QuoteMeta("Apply(startup::PlanSet(enabled, Read(), command));"),
		regexp.QuoteMeta("const bool actual = LaunchAtStartupEnabled();"),
		regexp.QuoteMeta("return applied && actual == enabled;"))
	refresh := definitionBody(t, "LaunchAtStartup.cpp", glue, "void RefreshLaunchAtStartup() {")
	provideRequire(t, "RefreshLaunchAtStartup", refresh, "startup::PlanRefresh(Read(), command);")
	quitForbid(t, "RefreshLaunchAtStartup", refresh, "a launch never turns the setting on or off", "PlanSet(")
	apply := definitionBody(t, "LaunchAtStartup.cpp", glue, "bool Apply(const startup::Plan& plan) {")
	provideRequire(t, "LaunchAtStartup.cpp's Apply", apply,
		"::RegSetKeyValueW(HKEY_CURRENT_USER, ids::kStartupRunKey, ids::kStartupRunValue, REG_SZ,",
		"::RegDeleteKeyValueW(HKEY_CURRENT_USER, key, ids::kStartupRunValue);",
		"if (plan.deleteCommand) ok = deleteValue(ids::kStartupRunKey) && ok;",
		"if (plan.deleteApproval) ok = deleteValue(ids::kStartupApprovedKey) && ok;")
	provideRequire(t, "LaunchAtStartup.cpp's reads", glue,
		"::RegGetValueW(HKEY_CURRENT_USER, ids::kStartupRunKey, ids::kStartupRunValue, flags,",
		"::RegGetValueW(HKEY_CURRENT_USER, ids::kStartupApprovedKey,")
	ids := stripComments(readCommonSource(t, "Ids.h"))
	provideRequire(t, "Ids.h", ids,
		`inline constexpr wchar_t kStartupRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";`,
		`L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";`,
		`inline constexpr wchar_t kStartupRunValue[] = L"URnetwork";`)
	provideRequire(t, "App.vcxproj", readAppSource(t, "App.vcxproj"),
		`<ClCompile Include="LaunchAtStartup.cpp"><PrecompiledHeader>NotUsing</PrecompiledHeader></ClCompile>`,
		`<ClInclude Include="LaunchAtStartup.h" />`)
}

// Uninstalling removes the uninstalling user's registration: the Run value
// and Task Manager's record of it, by the app's names, from deferred actions
// that impersonate that user (HKCU is their hive), never failing the
// uninstall, and not during an upgrade. The package itself never writes the
// Run value, so installing never turns the setting on.
func TestLaunchAtStartupUninstallRemovesTheRegistration(t *testing.T) {
	packagePath := filepath.Join(repositoryRoot(t), "app", "installer", "Package.wxs")
	data, err := os.ReadFile(packagePath)
	if err != nil {
		t.Fatal(err)
	}
	installer := regexp.MustCompile(`(?s)<!--.*?-->`).ReplaceAllString(string(data), "")
	for action, key := range map[string]string{
		"RemoveStartupRunValue":      `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`,
		"RemoveStartupApprovedValue": `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run`,
	} {
		property := regexp.MustCompile(`(?s)<SetProperty Id="` + action + `"[^>]*/>`).FindString(installer)
		if property == "" {
			t.Errorf("Package.wxs has no SetProperty for %s", action)
			continue
		}
		provideRequire(t, "Package.wxs "+action+"'s command", property,
			`Sequence="execute"`, `Before="`+action+`"`,
			`Value="&quot;[System64Folder]reg.exe&quot; delete &quot;`+key+`&quot; /v URnetwork /f"`)
		customAction := regexp.MustCompile(`(?s)<CustomAction Id="` + action + `"[^>]*/>`).FindString(installer)
		provideRequire(t, "Package.wxs "+action, customAction,
			`BinaryRef="Wix4UtilCA_$(sys.BUILDARCHSHORT)"`, `DllEntry="WixQuietExec"`,
			`Execute="deferred"`, `Impersonate="yes"`, `Return="ignore"`)
		scheduled := regexp.MustCompile(`(?s)<Custom Action="` + action + `"[^>]*/>`).FindString(installer)
		provideRequire(t, "Package.wxs "+action+"'s schedule", scheduled,
			`Condition="REMOVE~=&quot;ALL&quot; AND NOT UPGRADINGPRODUCTCODE"`)
	}
	sequence := regexp.MustCompile(`(?s)<InstallExecuteSequence>.*?</InstallExecuteSequence>`).FindString(installer)
	signOutRequireInOrder(t, "Package.wxs InstallExecuteSequence", sequence,
		regexp.QuoteMeta(`<Custom Action="RemoveStartupRunValue" Before="InstallFinalize"`),
		regexp.QuoteMeta(`<Custom Action="RemoveStartupApprovedValue" After="RemoveStartupRunValue"`))
	// the package removes the value; it never writes it
	if regexp.MustCompile(`(?is)<Registry(Key|Value)[^>]*CurrentVersion\\Run`).MatchString(installer) {
		t.Error("Package.wxs writes a Run value: installing must never turn launch on system startup on")
	}
}
