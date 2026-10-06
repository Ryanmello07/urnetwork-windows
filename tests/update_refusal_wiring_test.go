// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The call sites of the owner's 2026-10-05 decision that launches during an
// update are refused ("protect the update to not be corrupted where
// possible"). What a marker means is decided by Common/UpdateMarker.h
// (update_marker_test.go) and when a launch asks by instance::Launch
// (instance_handover_test.go); the updater, wWinMain and the Win32 glue need
// Windows, so these read their sources with every comment blanked.

// The handoff records the installer before anything waits on it: the update
// helper, which runs msiexec, is started with its process handle kept, the
// handle goes into the marker (its process id and creation time), and only
// then does the updater wait on it. The app does not quit for it: the
// installer closes the app before it replaces its files.
func TestUpdateRefusalTheHandoffRecordsTheInstaller(t *testing.T) {
	checker := stripComments(readAppSource(t, "UpdateChecker.cpp"))
	launch := definitionBody(t, "UpdateChecker.cpp", checker,
		"bool LaunchUpdateHelper(fs::path const& helperPath, std::wstring const& tag, HANDLE* helper,")
	signOutRequireInOrder(t, "LaunchUpdateHelper", launch,
		regexp.QuoteMeta("sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS;"),
		regexp.QuoteMeta(`sei.lpVerb = L"runas";`),
		regexp.QuoteMeta("if (!::ShellExecuteExW(&sei)) {"),
		regexp.QuoteMeta("return false;"),
		regexp.QuoteMeta("if (sei.hProcess) {"),
		regexp.QuoteMeta("RecordUpdateInProgress(sei.hProcess);"),
		regexp.QuoteMeta("*helper = sei.hProcess;"),
		regexp.QuoteMeta("return true;"))
	apply := definitionBody(t, "UpdateChecker.cpp", checker, "void UpdateChecker::RunApply() {")
	signOutRequireInOrder(t, "UpdateChecker::RunApply", apply,
		regexp.QuoteMeta("LaunchUpdateHelper(installFolder_ / kHelperName, offer.tag, &helper, launchError)"),
		regexp.QuoteMeta("::WaitForSingleObject(helper, 250)"),
		regexp.QuoteMeta("::CloseHandle(helper);"))

	glue := stripComments(readAppSource(t, "SingleInstance.cpp"))
	record := definitionBody(t, "SingleInstance.cpp", glue, "void RecordUpdateInProgress(void* installerProcess) {")
	signOutRequireInOrder(t, "RecordUpdateInProgress", record,
		regexp.QuoteMeta(".installerProcessId = ::GetProcessId(process),"),
		regexp.QuoteMeta(".installerCreationTime = CreationTime(process),"),
		regexp.QuoteMeta(".writtenAt = NowSeconds()};"),
		regexp.QuoteMeta("const std::filesystem::path path = UpdateInProgressFile();"),
		regexp.QuoteMeta("std::ofstream file(written, std::ios::binary | std::ios::trunc);"),
		regexp.QuoteMeta("file << update::FormatUpdateMarker(marker);"),
		regexp.QuoteMeta("std::filesystem::rename(written, path, error);"))
	if !regexp.MustCompile(`return StorageRoot\(\s*false\) / L"update_in_progress";`).
		MatchString(stripComments(readCommonSource(t, "Paths.cpp"))) {
		t.Error("Paths.cpp no longer keeps the update marker in the app's own storage root")
	}
}

// Every launch asks before it does anything else, the first launch included:
// instance::Launch runs for every launch and asks UpdateInProgress at the top
// of each round, and a launch that gets Updating tells the user (an autostart
// leaves without a word) and exits without reaching Application::Start.
func TestUpdateRefusalEveryLaunchAsksFirst(t *testing.T) {
	main := appMainSource(t)
	entry := definitionBody(t, "main.cpp", main, "int __stdcall wWinMain(")
	signOutRequireInOrder(t, "wWinMain", entry,
		regexp.QuoteMeta("primary = AppInstance::FindOrRegisterForKey(kInstanceKey);"),
		regexp.QuoteMeta("Launcher launcher(args, primary);"),
		regexp.QuoteMeta("urnw::instance::Launch(launcher);"),
		regexp.QuoteMeta("case urnw::instance::LaunchResult::Updating:"),
		regexp.QuoteMeta("if (!urnw::LaunchedByAutostart()) urnw::ShowUpdatingNotice();"),
		regexp.QuoteMeta("::ExitProcess(0);"),
		regexp.QuoteMeta("Application::Start("))
	// the launch loop runs for the first launch too, not only for a second one
	block := quitSourceFrom(t, "wWinMain", entry, "Launcher launcher(args, primary);", "Application::Start(")
	before := entry[:strings.Index(entry, "Launcher launcher(args, primary);")]
	if strings.HasSuffix(strings.TrimSpace(before), "if (!isPrimary) {") {
		t.Error("wWinMain runs instance::Launch only for a second launch: the first launch would " +
			"start the app during an update")
	}
	if !strings.Contains(block, "urnw::instance::Launch(launcher);") {
		t.Error("wWinMain does not drive the launch through instance::Launch")
	}
	launcher := quitSourceFrom(t, "main.cpp", main, "class Launcher {", "\n};\n")
	provideRequire(t, "Launcher", launcher,
		"bool UpdateInProgress() const { return urnw::UpdateInProgress(); }")
}

// The marker clears itself. A launch reads the file, judges it with the pure
// check (the installer looked at by process id and creation time), and deletes
// a stale one, so a crashed, failed or finished update refuses nothing later.
func TestUpdateRefusalAStaleMarkerIsDeleted(t *testing.T) {
	glue := stripComments(readAppSource(t, "SingleInstance.cpp"))
	check := definitionBody(t, "SingleInstance.cpp", glue, "bool UpdateInProgress() {")
	signOutRequireInOrder(t, "UpdateInProgress", check,
		regexp.QuoteMeta("const std::filesystem::path path = UpdateInProgressFile();"),
		regexp.QuoteMeta("update::Check(text, ProbeInstaller, NowSeconds());"),
		regexp.QuoteMeta("if (verdict == update::Verdict::Stale) {"),
		regexp.QuoteMeta("std::filesystem::remove(path, error);"),
		regexp.QuoteMeta("return verdict == update::Verdict::Updating;"))
	probe := definitionBody(t, "SingleInstance.cpp", glue,
		"update::InstallerState ProbeInstaller(const update::UpdateMarker& marker) {")
	signOutRequireInOrder(t, "ProbeInstaller", probe,
		regexp.QuoteMeta("::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,"),
		regexp.QuoteMeta("::GetLastError() == ERROR_INVALID_PARAMETER ? update::InstallerState::Ended"),
		regexp.QuoteMeta("if (created != marker.installerCreationTime) {"),
		regexp.QuoteMeta("state = update::InstallerState::Ended;"),
		regexp.QuoteMeta("::WaitForSingleObject(process, 0) == WAIT_OBJECT_0"),
		regexp.QuoteMeta("::CloseHandle(process);"))
}

// The notice is short and closes itself: this process runs URnetwork.exe from
// the folder the installer is replacing, so it must not stay for long.
func TestUpdateRefusalTheNoticeClosesItself(t *testing.T) {
	glue := stripComments(readAppSource(t, "SingleInstance.cpp"))
	notice := definitionBody(t, "SingleInstance.cpp", glue, "void ShowUpdatingNotice() {")
	signOutRequireInOrder(t, "ShowUpdatingNotice", notice,
		regexp.QuoteMeta("::WaitForSingleObject(dismissed, kUpdatingNoticeMs) == WAIT_TIMEOUT"),
		regexp.QuoteMeta("::EnumThreadWindows(thread, CloseNoticeBox, 0);"),
		regexp.QuoteMeta(`L"URnetwork is updating.\n\n"`),
		regexp.QuoteMeta("closer.join();"))
	provideRequire(t, "CloseNoticeBox",
		definitionBody(t, "SingleInstance.cpp", glue, "BOOL CALLBACK CloseNoticeBox(HWND window, LPARAM) {"),
		`std::wstring_view(name) == L"#32770"`, "::PostMessageW(window, WM_CLOSE, 0, 0);")
	if !regexp.MustCompile(`constexpr DWORD kUpdatingNoticeMs = \d{4,5};`).MatchString(glue) {
		t.Error("the updating notice has no bound of a few seconds")
	}
}

// Nothing relaunches the app while the installer runs, so the refusal blocks
// no relaunch of the installer's own: the MSI starts no URnetwork.exe and the
// updater starts nothing after it. If one is added, it has to be reconciled
// with the marker (it would be refused while msiexec still runs).
func TestUpdateRefusalNothingRelaunchesTheAppDuringTheInstall(t *testing.T) {
	packagePath := filepath.Join(repositoryRoot(t), "app", "installer", "Package.wxs")
	data, err := os.ReadFile(packagePath)
	if err != nil {
		t.Fatal(err)
	}
	installer := regexp.MustCompile(`(?s)<!--.*?-->`).ReplaceAllString(string(data), "")
	for _, launch := range []string{"WixShellExec", "LaunchApplication", `FileRef="URnetworkExe"`,
		`FileKey="URnetworkExe"`, "[#URnetworkExe]"} {
		if strings.Contains(installer, launch) {
			t.Errorf("Package.wxs starts the app (%s): a relaunch while msiexec runs is refused by "+
				"the update marker; reconcile it before adding one", launch)
		}
	}
	checker := stripComments(readAppSource(t, "UpdateChecker.cpp"))
	apply := definitionBody(t, "UpdateChecker.cpp", checker, "void UpdateChecker::RunApply() {")
	afterStart := apply[strings.Index(apply, "LaunchUpdateHelper(installFolder_ / kHelperName, offer.tag, &helper, launchError)"):]
	quitForbid(t, "UpdateChecker::RunApply after the installer starts", afterStart,
		"the updater relaunches nothing while the installer runs", "CreateProcess", "URnetwork.exe")
}
