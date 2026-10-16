// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"fmt"
	iofs "io/fs"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The call sites of the elevated update apply (app/src/Updater, the tray app's
// UpdateChecker and installer/Package.wxs). What each decision means runs in
// update_release_test.go; the helper, the tray and the installer need Windows,
// so these read their sources with every comment blanked. Each check returns
// the problems it found, so the negative controls below can run the same
// check on a rewritten source and require it to fail.

func readUpdaterSource(t *testing.T, name string) string {
	t.Helper()
	data, err := os.ReadFile(filepath.Join(repositoryRoot(t), "app", "src", "Updater", name))
	if err != nil {
		t.Fatal(err)
	}
	return string(data)
}

// The problems of `patterns` not matching in `text` in this order, each after
// the match of the one before it.
func applyOrderProblems(where, text string, patterns ...string) []string {
	from := 0
	for _, pattern := range patterns {
		location := regexp.MustCompile(pattern).FindStringIndex(text[from:])
		if location == nil {
			return []string{where + " is missing " + pattern + ", in this order after the patterns before it"}
		}
		from += location[1]
	}
	return nil
}

// The text from `opener` through the next "\n}\n", or "" when absent.
func applyDefinition(source, opener string) string {
	start := strings.Index(source, opener)
	if start < 0 {
		return ""
	}
	end := strings.Index(source[start:], "\n}\n")
	if end < 0 {
		return ""
	}
	return source[start : start+end+2]
}

// The body of the definition at `opener`, its whitespace folded to single
// spaces: what a whole-body pin compares. An order pin lets lines in between,
// and an early return inserted there keeps every pattern in place (the lead's
// G16 and G17).
func foldedBody(source, opener string) string {
	return strings.Join(strings.Fields(strings.TrimSuffix(strings.TrimPrefix(applyDefinition(source, opener), opener), "\n}")), " ")
}

func reportProblems(t *testing.T, problems []string) {
	t.Helper()
	for _, problem := range problems {
		t.Error(problem)
	}
}

// The helper is its own program: the static C runtime, imports resolved from
// System32 alone, no Common.lib and no SDK, built by the solution.
func checkUpdaterProject(project, solution, common string) []string {
	var problems []string
	for _, want := range []string{
		"<TargetName>URnetworkUpdate</TargetName>",
		"<RuntimeLibrary Condition=\"'$(Configuration)'=='Release'\">MultiThreaded</RuntimeLibrary>",
		"<RuntimeLibrary Condition=\"'$(Configuration)'=='Debug'\">MultiThreadedDebug</RuntimeLibrary>",
		"<AdditionalOptions>/DEPENDENTLOADFLAG:0x800 %(AdditionalOptions)</AdditionalOptions>",
		`<ClCompile Include="..\Common\InstallLocationWin32.cpp" />`,
		`<ClCompile Include="ShellStart.cpp" />`,
		"<AdditionalManifestFiles>Updater.manifest</AdditionalManifestFiles>",
	} {
		if !strings.Contains(project, want) {
			problems = append(problems, "Updater.vcxproj is missing "+want)
		}
	}
	settings := regexp.MustCompile(`(?s)<!--.*?-->`).ReplaceAllString(project, "")
	for _, forbidden := range []string{"MultiThreadedDLL", "MultiThreadedDebugDLL", "Common.vcxproj",
		"URnetworkSdk", "WindowsAppSDK", "Common.lib"} {
		if strings.Contains(settings, forbidden) {
			problems = append(problems, "Updater.vcxproj names "+forbidden+": the helper must load nothing from the app's folder")
		}
	}
	for _, config := range []string{"Release|x64", "Release|ARM64"} {
		if !strings.Contains(solution, "{A1B2C3D4-0006-4E5F-8A9B-000000000006}."+config+".Build.0 = "+config) {
			problems = append(problems, "URnetwork.sln does not build the helper for "+config)
		}
	}
	if !strings.Contains(solution, `"URnetworkUpdate", "src\Updater\Updater.vcxproj", "{A1B2C3D4-0006-4E5F-8A9B-000000000006}"`) {
		problems = append(problems, "URnetwork.sln does not list the helper's project")
	}
	for _, want := range []string{`<ClCompile Include="InstallLocationWin32.cpp" />`,
		`<ClInclude Include="InstallLocationWin32.h" />`, `<ClInclude Include="UpdateApply.h" />`} {
		if !strings.Contains(common, want) {
			problems = append(problems, "Common.vcxproj is missing "+want)
		}
	}
	return problems
}

func TestUpdateApplyHelperIsItsOwnProgram(t *testing.T) {
	root := repositoryRoot(t)
	solution, err := os.ReadFile(filepath.Join(root, "app", "URnetwork.sln"))
	if err != nil {
		t.Fatal(err)
	}
	reportProblems(t, checkUpdaterProject(readUpdaterSource(t, "Updater.vcxproj"), string(solution),
		readCommonSource(t, "Common.vcxproj")))
	manifest := readUpdaterSource(t, "Updater.manifest")
	if !strings.Contains(manifest, `<requestedExecutionLevel level="asInvoker" uiAccess="false" />`) {
		t.Error("Updater.manifest must run the helper as invoked: the tray elevates it, and the relaunch must not be")
	}
}

// Before anything else the helper limits where libraries load from. Its two
// elevated verbs, the update and the update channel, each take exactly one
// argument, and both drop the app's URNETWORK_* overrides first (UpdateApply.h
// IsAppOverrideName decides which), in one place; the relaunch is the user's
// app, in the user's environment, and keeps them. It starts the app only when
// it is not itself the elevated half of a split token. Nothing in it reads the
// environment or the user's storage.
func checkUpdaterEntry(main string, sources map[string]string) []string {
	entry := applyDefinition(main, "int WINAPI wWinMain(")
	problems := applyOrderProblems("wWinMain", entry,
		regexp.QuoteMeta("::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);"),
		regexp.QuoteMeta("::CommandLineToArgvW("),
		regexp.QuoteMeta(`const bool apply = args.size() == 3 && args[1] == L"--apply-update";`),
		regexp.QuoteMeta(`const bool setChannel = args.size() == 3 && args[1] == L"--set-update-channel";`),
		regexp.QuoteMeta("if (apply || setChannel) {"),
		regexp.QuoteMeta("DropAppOverrides();"),
		regexp.QuoteMeta("return apply ? urnw::updater::ApplyUpdate(args[2]) : urnw::updater::SetUpdateChannel(args[2]);"),
		regexp.QuoteMeta("if (args.size() == 1) return RelaunchApp();"),
		regexp.QuoteMeta("return static_cast<int>(urnw::update::Refusal::BadArguments);"))
	if strings.Index(entry, "SetDefaultDllDirectories") > strings.Index(entry, "CommandLineToArgvW") {
		problems = append(problems, "wWinMain loads shell32 before it limits the library search")
	}
	if count := strings.Count(entry, "DropAppOverrides();"); count != 1 {
		problems = append(problems, fmt.Sprintf("wWinMain drops the app's overrides %d times, want once, before either elevated verb", count))
	} else if branch := strings.Index(entry, "if (apply || setChannel) {"); branch < 0 ||
		strings.Index(entry, "DropAppOverrides();") < branch {
		problems = append(problems, "wWinMain drops the app's overrides before it knows the mode: the relaunch would start the app without the user's")
	}
	drop := applyDefinition(main, "void DropAppOverrides() {")
	problems = append(problems, applyOrderProblems("DropAppOverrides", drop,
		regexp.QuoteMeta("::GetEnvironmentStringsW();"),
		regexp.QuoteMeta("const std::wstring_view name = text.substr(0, equals);"),
		regexp.QuoteMeta("if (urnw::update::IsAppOverrideName(name)) names.emplace_back(name);"),
		regexp.QuoteMeta("::SetEnvironmentVariableW(name.c_str(), nullptr);"))...)
	elevated := applyDefinition(main, "bool IsFullyElevated() {")
	problems = append(problems, applyOrderProblems("IsFullyElevated", elevated,
		regexp.QuoteMeta("::GetTokenInformation(token, TokenElevationType, &type, sizeof(type), &size);"),
		regexp.QuoteMeta("return !read || type == TokenElevationTypeFull;"))...)
	relaunch := applyDefinition(main, "int RelaunchApp() {")
	problems = append(problems, applyOrderProblems("RelaunchApp", relaunch,
		regexp.QuoteMeta("if (IsFullyElevated()) return 0;"),
		regexp.QuoteMeta(`folder / L"URnetwork.exe";`),
		regexp.QuoteMeta(`L"\" --after-update";`),
		regexp.QuoteMeta("::CreateProcessW(app.c_str(), command.data(),"))...)
	for name, source := range sources {
		for _, forbidden := range []string{"GetEnvironmentVariable", "getenv", "StorageRoot(", "Paths.h",
			"LoadAppPrefs", "SHGetKnownFolderPath(FOLDERID_LocalAppData", "ExpandEnvironmentStrings", "LOCALAPPDATA"} {
			if strings.Contains(source, forbidden) {
				problems = append(problems, "the helper's "+name+" reads "+forbidden+
					": nothing a user can set may steer the elevated helper")
			}
		}
	}
	return problems
}

func updaterSources(t *testing.T) map[string]string {
	t.Helper()
	sources := map[string]string{}
	for _, name := range []string{"main.cpp", "ApplyUpdate.cpp", "Http.cpp", "HelperLog.cpp", "ShellStart.cpp"} {
		sources[name] = stripComments(readUpdaterSource(t, name))
	}
	sources["InstallLocationWin32.cpp"] = stripComments(readCommonSource(t, "InstallLocationWin32.cpp"))
	return sources
}

func TestUpdateApplyHelperStartsClean(t *testing.T) {
	sources := updaterSources(t)
	reportProblems(t, checkUpdaterEntry(sources["main.cpp"], sources))
}

// The helper's steps, in the order the spec gives them: who and where; the
// feed's list, fetched here; the release it offers; the download through the
// one allowed redirect; the digest through the handle that holds the file;
// the package's identity; the folders, the package and the log admin-only by
// their own ACL; the image moved out of the way; msiexec; the result; and,
// after an install that failed, the relaunch.
func checkApplyUpdate(apply string) []string {
	body := applyDefinition(apply, "int ApplyUpdate(std::wstring_view tagArgument) {")
	problems := applyOrderProblems("ApplyUpdate", body,
		regexp.QuoteMeta("if (!IsElevated()) {"),
		regexp.QuoteMeta("return static_cast<int>(Refusal::NotElevated);"),
		regexp.QuoteMeta("const fs::path executable = install::OwnExecutablePath();"),
		regexp.QuoteMeta("!install::AdminOnlyLocation(executable, why)"),
		regexp.QuoteMeta("return static_cast<int>(Refusal::NotInstalled);"),
		regexp.QuoteMeta("if (version::kCode == 0) {"),
		// the lock comes before the feed and the tag, so Busy is reported
		// before a bad tag; PrepareFolder of updates\ moves up into it
		regexp.QuoteMeta(`const fs::path updates = installFolder / L"updates";`),
		regexp.QuoteMeta("Security folderSecurity(kAdminOnlyFolderSddl);"),
		regexp.QuoteMeta("Security fileSecurity(kAdminOnlyFileSddl);"),
		regexp.QuoteMeta("TakeHelperLock(updates, folderSecurity, helperLock, log)"),
		regexp.QuoteMeta("const update::Feed& feed = ChannelFeed(installFolder, log);"),
		regexp.QuoteMeta("if (!update::IsTagArgument(feed, tag)) {"),
		regexp.QuoteMeta(`const fs::path tagFolder = updates / WidenAscii(tag);`),
		regexp.QuoteMeta("if (!PrepareFolder(tagFolder, folderSecurity, error)) {"),
		regexp.QuoteMeta(`log.Open(tagFolder / L"update-helper.log", fileSecurity.attributes());`),
		regexp.QuoteMeta("if (WriteResult(updates, result, fileSecurity, writeError)) {"),
		regexp.QuoteMeta("const std::wstring listUrl = WidenAscii(update::ReleaseListUrl(feed));"),
		regexp.QuoteMeta("HttpGet("),
		regexp.QuoteMeta("if (list.status == 403 || list.status == 429) {"),
		regexp.QuoteMeta("return refuse(Refusal::RateLimited,"),
		regexp.QuoteMeta("if (list.status != 200) {"),
		regexp.QuoteMeta("if (list.serverUnixSeconds == 0) {"),
		regexp.QuoteMeta("update::ParseReleaseList(body);"),
		regexp.QuoteMeta("update::SelectRelease(*releases, kArch, feed, list.serverUnixSeconds);"),
		regexp.QuoteMeta("if (!update::SelectionOffers(selection, tag, version::kCode)) {"),
		regexp.QuoteMeta("if (!update::IsFeedAssetUrl(feed, selection.tag, selection.assetName, selection.assetUrl)) {"),
		regexp.QuoteMeta("HttpGet(WidenAscii(selection.assetUrl), nullptr, 0, nullptr, redirect, error)"),
		regexp.QuoteMeta("if (redirect.status != 302 || !update::IsAllowedAssetRedirect(redirect.location) ||"),
		regexp.QuoteMeta("!HttpUrlHostIsOneOf(WidenAscii(redirect.location), update::kAssetRedirectHosts)) {"),
		regexp.QuoteMeta("GENERIC_WRITE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,"),
		regexp.QuoteMeta("fileSecurity.attributes(), CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));"),
		regexp.QuoteMeta("WidenAscii(redirect.location), nullptr, kMaxPackageBytes,"),
		regexp.QuoteMeta("Handle held(::CreateFileW(package.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,"),
		regexp.QuoteMeta("!SameFile(writtenId, heldId)"),
		regexp.QuoteMeta("const std::string actual = Sha256(held.get());"),
		regexp.QuoteMeta("!update::EqualsAsciiCaseless(actual, selection.digestHex)"),
		regexp.QuoteMeta("return refuse(Refusal::Digest,"),
		regexp.QuoteMeta("::MsiOpenDatabaseW(package.c_str(), MSIDBOPEN_READONLY, database.put())"),
		regexp.QuoteMeta(`PackageProperty(database.get(), L"UpgradeCode");`),
		regexp.QuoteMeta(`PackageProperty(database.get(), L"ProductVersion");`),
		regexp.QuoteMeta("!update::PackageMatches(*upgradeCode, *productVersion, selection.code)"),
		regexp.QuoteMeta("return refuse(Refusal::Package,"),
		regexp.QuoteMeta(`const fs::path installLog = tagFolder / L"install.log";`),
		regexp.QuoteMeta("::DeleteFileW(installLog.c_str());"),
		regexp.QuoteMeta("fileSecurity.attributes(), CREATE_NEW,"),
		regexp.QuoteMeta("!CheckAdminOnly({updates, tagFolder, package, installLog}, error)"),
		regexp.QuoteMeta("return refuse(Refusal::Staging,"),
		regexp.QuoteMeta("::MoveFileExW(executable.c_str(), running.c_str(), MOVEFILE_WRITE_THROUGH)"),
		regexp.QuoteMeta(`const fs::path msiexec = systemFolder / L"msiexec.exe";`),
		regexp.QuoteMeta("update::MsiexecCommandLine(msiexec.native(), package.native(),"),
		regexp.QuoteMeta("::CreateProcessW(msiexec.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,"),
		regexp.QuoteMeta("::WaitForSingleObject(installer.get(), INFINITE);"),
		regexp.QuoteMeta("::GetExitCodeProcess(installer.get(), &exitCode);"),
		regexp.QuoteMeta("held.Close();"),
		regexp.QuoteMeta("if (update::KeepsPackage(exitCode)) {"),
		// the earlier downloads, folders alone: a file there, the channel's
		// marker among them, stays
		regexp.QuoteMeta("if (folder.is_directory(ec) && folder.path().filename() != tagFolder.filename())"),
		regexp.QuoteMeta("return finish(exitCode);"),
		regexp.QuoteMeta("::DeleteFileW(package.c_str());"),
		regexp.QuoteMeta("const int ended = finish(exitCode);"),
		regexp.QuoteMeta("StartThroughShell(executable, shellError)"),
		regexp.QuoteMeta("return ended;"))
	// what everything under updates\ is created with: owned by Administrators,
	// SYSTEM and Administrators full, Users read and execute, nothing inherited;
	// the lock file is tighter still, with nothing for Users at all, so no
	// process without administrator rights can open it, and so hold it
	for _, want := range []string{
		`constexpr wchar_t kAdminOnlyFolderSddl[] = L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;BU)";`,
		`constexpr wchar_t kAdminOnlyFileSddl[] = L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;BU)";`,
		`constexpr wchar_t kLockFileSddl[] = L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)";`,
		`constexpr wchar_t kChannelMarkerSddl[] = L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x120080;;;BU)";`,
	} {
		if !strings.Contains(apply, want) {
			problems = append(problems, "ApplyUpdate.cpp does not create what it writes under updates\\ with "+want)
		}
	}
	// The lock: updates\ judged as it is found, before anything gives it its
	// security again, then prepared, then helper.lock opened for its data,
	// shared with nobody (the 0 share mode), without following a link, created
	// admin-only; a sharing violation is Busy; a thing there that is not a plain
	// file is refused; a lock already there is given its security again; and
	// when the folder was found open to others, whatever lies under the
	// channel marker's name goes before either verb reads the channel.
	lockFn := applyDefinition(apply, "std::optional<Refusal> TakeHelperLock(const fs::path& updates, Security& folderSecurity,")
	problems = append(problems, applyOrderProblems("TakeHelperLock", lockFn,
		regexp.QuoteMeta("const bool foundOpen = !FoundAsTheHelpers(updates);"),
		regexp.QuoteMeta("if (!PrepareFolder(updates, folderSecurity, error)) {"),
		regexp.QuoteMeta("Security lockSecurity(kLockFileSddl);"),
		regexp.QuoteMeta(`const fs::path lockPath = updates / L"helper.lock";`),
		regexp.QuoteMeta("FILE_READ_DATA | READ_CONTROL | WRITE_DAC | WRITE_OWNER | FILE_READ_ATTRIBUTES, 0,"),
		regexp.QuoteMeta("lockSecurity.attributes(), OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);"),
		regexp.QuoteMeta("if (opened == ERROR_SHARING_VIOLATION) {"),
		regexp.QuoteMeta("return Refusal::Busy;"),
		regexp.QuoteMeta("(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {"),
		regexp.QuoteMeta("::SetSecurityInfo("),
		regexp.QuoteMeta("if (foundOpen) {"),
		regexp.QuoteMeta("if (!RemoveMarkerName(install::ChannelMarkerPath(updates.parent_path()), removed, error)) {"),
		regexp.QuoteMeta("return Refusal::Staging;"),
		regexp.QuoteMeta("return std::nullopt;"))...)
	found := applyDefinition(apply, "bool FoundAsTheHelpers(const fs::path& updates) {")
	problems = append(problems, applyOrderProblems("FoundAsTheHelpers", found,
		regexp.QuoteMeta("install::ReadPathFacts(updates);"),
		regexp.QuoteMeta("if (folder.found == update::PathFacts::Found::Absent) return true;"),
		regexp.QuoteMeta("return update::IsHelpersFolder(folder, install::ReadDescriptorFacts(updates));"))...)
	// and nothing else: updates\ as it is found, judged by its descriptor alone
	if body := foldedBody(apply, "bool FoundAsTheHelpers(const fs::path& updates) {"); body != "const update::PathFacts folder = install::ReadPathFacts(updates); if (folder.found == update::PathFacts::Found::Absent) return true; return update::IsHelpersFolder(folder, install::ReadDescriptorFacts(updates));" {
		problems = append(problems, fmt.Sprintf("FoundAsTheHelpers is %q, want updates\\ as it is found judged by its descriptor alone, "+
			"or absent: a folder that was open to others is never taken for the helper's own", body))
	}
	// The lock is kept until the helper exits. TakeHelperLock closes the handle
	// only where it then refuses, and ApplyUpdate declares the handle in its own
	// scope and names it nowhere else but to take the lock: closed, moved or
	// assigned anywhere else, the lock would be let go while the installer may
	// still be running.
	closes := strings.Count(lockFn, "lock.Close();")
	refusing := len(regexp.MustCompile(`lock\.Close\(\);\s+return Refusal::\w+;`).FindAllString(lockFn, -1))
	if closes != refusing {
		problems = append(problems, fmt.Sprintf("TakeHelperLock closes the lock's handle %d times and refuses after %d of them: "+
			"the lock is to be held until the helper exits", closes, refusing))
	}
	if !strings.Contains(body, "\n  Handle helperLock;\n") {
		problems = append(problems, "ApplyUpdate does not declare helperLock in its own scope: the lock is to be held until the helper exits")
	}
	if named := strings.Count(body, "helperLock"); named != 2 {
		problems = append(problems, fmt.Sprintf("ApplyUpdate names helperLock %d times, want twice, where it declares it and where it takes "+
			"the lock: the lock is to be held until the helper exits", named))
	}
	prepare := applyDefinition(apply, "bool PrepareFolder(const fs::path& folder, Security& security, std::string& error) {")
	problems = append(problems, applyOrderProblems("PrepareFolder", prepare,
		regexp.QuoteMeta("if (::CreateDirectoryW(folder.c_str(), security.attributes())) return true;"),
		regexp.QuoteMeta("FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,"),
		regexp.QuoteMeta("(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {"),
		regexp.QuoteMeta("::SetSecurityInfo("),
		regexp.QuoteMeta("OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,"),
		regexp.QuoteMeta("owner, nullptr, dacl, nullptr);"),
		regexp.QuoteMeta("return true;"))...)
	write := applyDefinition(apply, "bool WriteResult(const fs::path& updates, const update::UpdateResult& result, Security& security,")
	problems = append(problems, applyOrderProblems("WriteResult", write,
		regexp.QuoteMeta("security.attributes(), CREATE_NEW,"),
		regexp.QuoteMeta("MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH"))...)
	check := applyDefinition(apply, "bool CheckAdminOnly(const std::vector<fs::path>& paths, std::string& error) {")
	problems = append(problems, applyOrderProblems("CheckAdminOnly", check,
		regexp.QuoteMeta("for (const fs::path& path : paths) {"),
		regexp.QuoteMeta("if (!install::AdminOnlyPath(path, why)) {"),
		regexp.QuoteMeta("return false;"))...)
	return problems
}

// What the helper's admin-only checks judge: a token without admin rights,
// and the rights that change a file or a folder.
func checkAdminOnlyPath(location string) []string {
	return applyOrderProblems("AdminOnlyPath", applyDefinition(location, "bool AdminOnlyPath(const fs::path& path, std::string& why) {"),
		regexp.QuoteMeta("HANDLE raw = NonAdminToken(why);"),
		regexp.QuoteMeta("if (!RightsFor(token.get(), path, rights, why)) return false;"),
		regexp.QuoteMeta("if ((rights & kWriteRights) == 0) return true;"),
		regexp.QuoteMeta("return false;"))
}

func TestUpdateApplyHelperChecksInOrder(t *testing.T) {
	sources := updaterSources(t)
	reportProblems(t, checkApplyUpdate(sources["ApplyUpdate.cpp"]))
	reportProblems(t, checkAdminOnlyPath(sources["InstallLocationWin32.cpp"]))
}

// Every file the helper is built from, by name, with comments blanked: all
// that lies in app/src/Updater, and the one source of Common its project
// compiles (updaterSources).
func helperFiles(t *testing.T) map[string]string {
	t.Helper()
	files := updaterSources(t)
	entries, err := os.ReadDir(filepath.Join(repositoryRoot(t), "app", "src", "Updater"))
	if err != nil {
		t.Fatal(err)
	}
	for _, entry := range entries {
		if entry.Type().IsRegular() {
			files[entry.Name()] = stripComments(readUpdaterSource(t, entry.Name()))
		}
	}
	// these two come from the listing alone: without them it saw nothing
	for _, want := range []string{"ApplyUpdate.h", "Updater.vcxproj"} {
		if _, ok := files[want]; !ok {
			t.Fatalf("the helper's files miss %s: the listing did not see app/src/Updater", want)
		}
	}
	return files
}

// The helper names no mutex, in any of its files. A named mutex is a lock any
// process on the machine can create first and hold, and for as long as it
// does, every helper ends in Busy.
func checkHelperHasNoMutex(files map[string]string) []string {
	var problems []string
	for name, text := range files {
		if strings.Contains(text, "CreateMutex") {
			problems = append(problems, "the helper's "+name+" names CreateMutex: the helper's lock is a file in updates\\, "+
				"not a mutex any process can create and hold")
		}
	}
	return problems
}

func TestUpdateApplyHelperHasNoMutex(t *testing.T) {
	reportProblems(t, checkHelperHasNoMutex(helperFiles(t)))
}

// After an install that failed, the helper starts its relaunch through the
// desktop's shell, so it runs as the signed-in user, never elevated:
// Explorer's Shell.Application object, as WixUnelevatedShellExec does.
func checkShellStart(shell string) []string {
	start := applyDefinition(shell, "bool StartThroughShell(const std::filesystem::path& program, std::string& error) {")
	problems := applyOrderProblems("StartThroughShell", start,
		regexp.QuoteMeta("::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,"),
		regexp.QuoteMeta("windows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &desktop, SWFO_NEEDDISPATCH,"),
		regexp.QuoteMeta("services->QueryService(SID_STopLevelBrowser, __uuidof(IShellBrowser),"),
		regexp.QuoteMeta("browser->QueryActiveShellView(view.GetAddressOf());"),
		regexp.QuoteMeta("view->GetItemObject(SVGIO_BACKGROUND, __uuidof(IDispatch),"),
		regexp.QuoteMeta("folderView->get_Application(application.GetAddressOf());"),
		regexp.QuoteMeta("hr = application.As(&shell);"),
		regexp.QuoteMeta("hr = shell->ShellExecute(file.get(), arguments, directory, operation, show);"))
	for _, forbidden := range []string{"CreateProcess", "ShellExecuteEx", "::ShellExecuteW"} {
		if strings.Contains(shell, forbidden) {
			problems = append(problems, "ShellStart.cpp starts the relaunch with "+forbidden+
				": from the elevated helper that would run it elevated")
		}
	}
	return problems
}

func TestUpdateApplyHelperRelaunchesThroughTheShell(t *testing.T) {
	reportProblems(t, checkShellStart(updaterSources(t)["ShellStart.cpp"]))
}

// Which repository's releases the helper installs is decided by the channel's
// feed alone, and that feed is one compiled in: the channel may at most name
// an id in ReleaseSelection.h's table (FeedById), never a repository, owner,
// name or URL read at run time, and nothing in the user's registry hive can
// choose it. ChannelFeed reads the machine-wide marker through the one reader
// and resolves it through FeedOfMarker, which reaches the developer row by
// its id alone.
func checkChannelIsAnIndex(apply string, sources map[string]string) []string {
	var problems []string
	runner := regexp.MustCompile(`(?s)#if defined\(URN_UPDATE_RUNNER_TEST_REPO_ID\)\n#define URN_UPDATER_STRINGIZE_.*?#endif\n`)
	rest := runner.ReplaceAllString(apply, "")
	if rest == apply {
		problems = append(problems, "ApplyUpdate.cpp has no runner test feed block to set apart")
	}
	for _, built := range []string{"Feed{", ".numericRepoId =", ".owner =", ".repo =", ".tagPrefix ="} {
		if strings.Contains(rest, built) {
			problems = append(problems, "ApplyUpdate.cpp builds a feed at run time ("+built+
				"): the channel may only name one compiled in")
		}
	}
	channel := applyDefinition(apply, "const update::Feed& ChannelFeed(")
	if !regexp.MustCompile(`^const update::Feed& ChannelFeed\(\[\[maybe_unused\]\] const fs::path& installFolder,\s*` +
		`\[\[maybe_unused\]\] HelperLog& log\) \{\s*#if defined\(URN_UPDATE_RUNNER_TEST_REPO_ID\)\s*return kRunnerTestFeed;\s*` +
		`#else\s*std::string why;\s*const update::ChannelMarker marker = install::ReadChannelMarker\(installFolder, why\);\s*` +
		`if \(marker == update::ChannelMarker::Rejected\) log\.Line\("the channel marker is not trusted: \{\}", why\);\s*` +
		`return update::FeedOfMarker\(marker\);\s*#endif\s*\}\s*$`).MatchString(channel) {
		problems = append(problems, "ChannelFeed is not the marker, read through ReadChannelMarker and resolved by FeedOfMarker: "+
			"the channel may name only a row of the table, read where only an administrator can write")
	}
	for name, source := range sources {
		for _, user := range []string{"HKEY_CURRENT_USER", "HKCU", "HKEY_USERS", "RegOpenCurrentUser"} {
			if strings.Contains(source, user) {
				problems = append(problems, "the helper's "+name+" reads "+user+
					": what it installs is decided where only an administrator can write")
			}
		}
	}
	return problems
}

// The update channel's verb, in SPEC-A2c 4.2's order: who and where, as for an
// update; the argument, an id of the table; the plan from the marker as it
// is; the question, for the developer channel, before anything is written;
// the lock, after the question; the marker read and planned again under the
// lock; the write; and the marker read back through the tray's own reader.
// Its lock is held for the verb's whole scope, as the apply's is. What it
// removes is a file, an empty folder or a link, and nothing recursively; the
// marker is created nowhere else, with its own security, and the cleanup after
// an install removes folders alone, so the first developer update keeps it.
func checkChannelVerb(apply, location string) []string {
	verb := applyDefinition(apply, "int SetUpdateChannel(std::wstring_view channelArgument) {")
	problems := applyOrderProblems("SetUpdateChannel", verb,
		regexp.QuoteMeta("if (!IsElevated()) {"),
		regexp.QuoteMeta("return static_cast<int>(Refusal::NotElevated);"),
		regexp.QuoteMeta("const fs::path executable = install::OwnExecutablePath();"),
		regexp.QuoteMeta("!install::AdminOnlyLocation(executable, why)"),
		regexp.QuoteMeta("return static_cast<int>(Refusal::NotInstalled);"),
		regexp.QuoteMeta("if (version::kCode == 0) {"),
		regexp.QuoteMeta("return static_cast<int>(Refusal::DevBuild);"),
		// the channel verb takes an id of the table
		regexp.QuoteMeta("const update::Feed* asked = update::FeedOfChannelArgument(AsciiTag(channelArgument));"),
		regexp.QuoteMeta("return static_cast<int>(Refusal::BadArguments);"),
		regexp.QuoteMeta("const fs::path marker = install::ChannelMarkerPath(installFolder);"),
		regexp.QuoteMeta("update::ChannelPlan plan = update::PlanOf(*asked, install::ReadChannelMarker(installFolder, why));"),
		regexp.QuoteMeta("if (plan == update::ChannelPlan::Nothing) {"),
		regexp.QuoteMeta("return 0;"),
		regexp.QuoteMeta("if (plan == update::ChannelPlan::AskThenCreate && !AdministratorConfirms(*asked, log)) {"),
		regexp.QuoteMeta("return static_cast<int>(Refusal::Declined);"),
		regexp.QuoteMeta("Security markerSecurity(kChannelMarkerSddl);"),
		regexp.QuoteMeta("TakeHelperLock(updates, folderSecurity, helperLock, log)"),
		regexp.QuoteMeta("plan = update::PlanOf(*asked, install::ReadChannelMarker(installFolder, why));"),
		regexp.QuoteMeta("if (plan == update::ChannelPlan::Nothing) {"),
		regexp.QuoteMeta("return 0;"),
		regexp.QuoteMeta("if (!RemoveMarkerName(marker, removed, error)) {"),
		regexp.QuoteMeta("return static_cast<int>(Refusal::Staging);"),
		regexp.QuoteMeta("if (plan == update::ChannelPlan::AskThenCreate) {"),
		regexp.QuoteMeta("markerSecurity.attributes(), CREATE_NEW,"),
		regexp.QuoteMeta("if (update::PlanOf(*asked, install::ReadChannelMarker(installFolder, why)) !="),
		regexp.QuoteMeta("RemoveMarkerName(marker, removed, error);"),
		regexp.QuoteMeta("return static_cast<int>(Refusal::Staging);"),
		regexp.QuoteMeta("return 0;"))
	if !strings.Contains(verb, "\n  Handle helperLock;\n") {
		problems = append(problems, "SetUpdateChannel does not declare helperLock in its own scope: the lock is to be held until the verb ends")
	}
	if named := strings.Count(verb, "helperLock"); named != 2 {
		problems = append(problems, fmt.Sprintf("SetUpdateChannel names helperLock %d times, want twice, where it declares it and where it takes the lock", named))
	}
	for _, owned := range []string{"remove_all", "MoveFile", "ReplaceFile", "TRUNCATE_EXISTING", "CREATE_ALWAYS", "OPEN_ALWAYS"} {
		if strings.Contains(verb, owned) || strings.Contains(applyDefinition(apply, "bool RemoveMarkerName("), owned) {
			problems = append(problems, "the channel verb uses "+owned+
				": the channel verb removes a file, an empty folder or a link, and nothing else, and never replaces the marker")
		}
	}
	remove := applyDefinition(apply, "bool RemoveMarkerName(const fs::path& marker, bool& removed, std::string& error) {")
	problems = append(problems, applyOrderProblems("RemoveMarkerName", remove,
		regexp.QuoteMeta("const DWORD attributes = ::GetFileAttributesW(marker.c_str());"),
		regexp.QuoteMeta("if (missing == ERROR_FILE_NOT_FOUND || missing == ERROR_PATH_NOT_FOUND) return true;"),
		regexp.QuoteMeta("const BOOL gone = (attributes & FILE_ATTRIBUTE_DIRECTORY) ? ::RemoveDirectoryW(marker.c_str())"),
		regexp.QuoteMeta(": ::DeleteFileW(marker.c_str());"),
		regexp.QuoteMeta("removed = true;"))...)
	// the marker is created by the channel verb alone, with its own security,
	// and its name is spelled in one place
	if count := strings.Count(apply, "kChannelMarkerSddl"); count != 2 {
		problems = append(problems, fmt.Sprintf("ApplyUpdate.cpp names kChannelMarkerSddl %d times, want twice: "+
			"its definition and the channel verb's, which alone creates the marker", count))
	}
	if count := strings.Count(apply, "install::ChannelMarkerPath("); count != 2 {
		problems = append(problems, fmt.Sprintf("ApplyUpdate.cpp reaches the channel marker %d times, want twice: the lock's removal and the channel verb", count))
	}
	// the question
	ask := applyDefinition(apply, "bool AdministratorConfirms(const update::Feed& feed, HelperLog& log) {")
	problems = append(problems, applyOrderProblems("AdministratorConfirms", ask,
		regexp.QuoteMeta("if (!::GetUserObjectInformationW(::GetProcessWindowStation(), UOI_FLAGS, &station,"),
		regexp.QuoteMeta("!(station.dwFlags & WSF_VISIBLE)) {"),
		regexp.QuoteMeta("return false;"),
		regexp.QuoteMeta(`const std::wstring repository = WidenAscii(feed.owner) + L"/" + WidenAscii(feed.repo);`),
		regexp.QuoteMeta("const std::wstring text = std::vformat(kChannelQuestion, std::make_wformat_args(repository));"),
		regexp.QuoteMeta("::MessageBoxW(nullptr, text.c_str(), kChannelCaption,"),
		regexp.QuoteMeta("MB_OKCANCEL | MB_DEFBUTTON2 | MB_ICONWARNING | MB_SETFOREGROUND |"),
		regexp.QuoteMeta("if (answer == IDOK) {"),
		regexp.QuoteMeta("return true;"),
		regexp.QuoteMeta("return false;"))...)
	// and nothing else: no answer but OK says yes, a box that could not be shown
	// included, and nothing returns early
	if body := foldedBody(apply, "bool AdministratorConfirms(const update::Feed& feed, HelperLog& log) {"); body != "USEROBJECTFLAGS station{}; if (!::GetUserObjectInformationW(::GetProcessWindowStation(), UOI_FLAGS, &station, sizeof(station), nullptr) || !(station.dwFlags & WSF_VISIBLE)) { log.Line(\"declined: this window station is not visible, so nobody can be asked\"); return false; } const std::wstring repository = WidenAscii(feed.owner) + L\"/\" + WidenAscii(feed.repo); const std::wstring text = std::vformat(kChannelQuestion, std::make_wformat_args(repository)); const int answer = ::MessageBoxW(nullptr, text.c_str(), kChannelCaption, MB_OKCANCEL | MB_DEFBUTTON2 | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST); if (answer == IDOK) { log.Line(\"confirmed: the administrator chose OK for the {} channel\", feed.id); return true; } log.Line(\"declined: the question for the {} channel was answered {}\", feed.id, answer); return false;" {
		problems = append(problems, fmt.Sprintf("AdministratorConfirms is %q, want the window station checked first, the row's repository "+
			"asked about, and OK alone taken for yes", body))
	}
	if count := strings.Count(apply, "github.com/{0}"); count != 1 {
		problems = append(problems, fmt.Sprintf("the question names github.com/{0} %d times, want once: the repository is the row's", count))
	}
	for _, literal := range []string{"Ryanmello07", "urnetwork-windows", "1324127805"} {
		if strings.Contains(apply, literal) {
			problems = append(problems, "ApplyUpdate.cpp names "+literal+": the question names the row's repository, never one of its own")
		}
	}
	// the reader: its two paths from its parameter and two literals, and its
	// facts in order
	problems = append(problems, applyOrderProblems("ChannelMarkerPath",
		applyDefinition(location, "fs::path ChannelMarkerPath(const fs::path& installFolder) {"),
		regexp.QuoteMeta(`return installFolder / L"updates" / L"developer-channel";`))...)
	reader := applyDefinition(location, "update::ChannelMarker ReadChannelMarker(const fs::path& installFolder, std::string& why, bool* present) {")
	problems = append(problems, applyOrderProblems("ReadChannelMarker", reader,
		regexp.QuoteMeta(`const fs::path updates = installFolder / L"updates";`),
		regexp.QuoteMeta("const fs::path marker = ChannelMarkerPath(installFolder);"),
		regexp.QuoteMeta("update::PathFacts folderFacts = ReadPathFacts(updates);"),
		regexp.QuoteMeta("folderDescriptor = ReadDescriptorFacts(updates);"),
		regexp.QuoteMeta("markerFacts = ReadPathFacts(marker);"),
		regexp.QuoteMeta("markerDescriptor = ReadDescriptorFacts(marker);"),
		regexp.QuoteMeta("update::JudgeMarker(folderFacts, folderDescriptor, markerFacts, markerDescriptor);"),
		regexp.QuoteMeta("if (present) *present = judged.present;"),
		regexp.QuoteMeta("return judged.marker;"))...)
	problems = append(problems, applyOrderProblems("ReadPathFacts", applyDefinition(location, "update::PathFacts ReadPathFacts(const fs::path& path) {"),
		regexp.QuoteMeta("::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)"),
		regexp.QuoteMeta("error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND"))...)
	problems = append(problems, applyOrderProblems("ReadDescriptorFacts", applyDefinition(location, "update::DescriptorFacts ReadDescriptorFacts(const fs::path& path) {"),
		regexp.QuoteMeta("constexpr SECURITY_INFORMATION kInfo = OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;"),
		regexp.QuoteMeta("facts.daclProtected = (control & SE_DACL_PROTECTED) != 0;"),
		regexp.QuoteMeta("facts.read = !facts.owner.empty();"))...)
	return problems
}

func TestUpdateApplyTheChannelVerbChecksInOrder(t *testing.T) {
	sources := updaterSources(t)
	reportProblems(t, checkChannelVerb(sources["ApplyUpdate.cpp"], sources["InstallLocationWin32.cpp"]))
}

func TestUpdateApplyTheChannelIsOnlyAnIndex(t *testing.T) {
	sources := updaterSources(t)
	reportProblems(t, checkChannelIsAnIndex(sources["ApplyUpdate.cpp"], sources))
}

// The marker alone selects the feed: FeedOfMarker, which the tray (Start,
// SyncChannel) and the helper (ChannelFeed) both call, is pinned whole, its
// whitespace folded (the lead's G17). A conditional inside it, a build-time
// default under any name among them, is reported here; update_release_test.go
// runs what it does, with controls.
func checkMarkerSelectsTheFeedAlone(channel string) []string {
	if body := foldedBody(channel, "inline const Feed& FeedOfMarker(ChannelMarker marker) {"); body != "if (marker == ChannelMarker::Trusted) { if (const Feed* developer = FeedById(kDeveloperChannelId)) return *developer; } return kOfficialFeed;" {
		return []string{fmt.Sprintf("UpdateChannel.h's FeedOfMarker is %q, want the developer row for a trusted marker alone, "+
			"while the table has it, and the official feed for anything else", body)}
	}
	return nil
}

func TestUpdateApplyTheMarkerSelectsTheFeedAlone(t *testing.T) {
	channel := stripComments(readCommonSource(t, "UpdateChannel.h"))
	reportProblems(t, checkMarkerSelectsTheFeedAlone(channel))
	// the review's A1: a default under a name no list knows, inside the function
	const first = "  if (marker == ChannelMarker::Trusted) {\n    if (const Feed* developer = FeedById(kDeveloperChannelId)) return *developer;\n"
	if strings.Count(channel, first) != 1 {
		t.Fatalf("negative control: %q is not in UpdateChannel.h exactly once", first)
	}
	changed := strings.Replace(channel, first, "#if defined(URN_DEVELOPER_BY_DEFAULT)\n"+
		"  if (marker == ChannelMarker::Absent) marker = ChannelMarker::Trusted;\n#endif\n"+first, 1)
	if problems := strings.Join(checkMarkerSelectsTheFeedAlone(changed), "\n"); !strings.Contains(problems, "FeedOfMarker is") {
		t.Fatalf("negative control was not detected: a default inside FeedOfMarker; the check reported:\n%s", problems)
	}
}

// No build may judge or read the marker its own way (the lead's G19, the
// review's N1): UpdateChannel.h holds no preprocessor conditional, and
// InstallLocationWin32.cpp none but its WIN32_LEAN_AND_MEAN guard. So a
// default under any name, read by an #if in JudgeMarker or in
// ReadChannelMarker, which the tray and the helper both call, fails here.
// FeedOfMarker is pinned whole besides (checkMarkerSelectsTheFeedAlone), and
// commit 2's test refuses three spellings of a default in the build inputs.
var preprocessorConditional = regexp.MustCompile(`(?m)^[ \t]*(?:#|%:)[ \t]*(?:if|ifdef|ifndef|elif|elifdef|elifndef|else|endif)\b.*$`)

// InstallLocationWin32.cpp's one conditional, before it includes windows.h.
const leanAndMeanGuard = "#ifndef WIN32_LEAN_AND_MEAN\n#define WIN32_LEAN_AND_MEAN\n#endif\n"

func checkTheChannelBuildsAlike(channel, location string) []string {
	var problems []string
	for _, line := range preprocessorConditional.FindAllString(channel, -1) {
		problems = append(problems, fmt.Sprintf("UpdateChannel.h has a preprocessor conditional, %q: "+
			"no build may judge the marker its own way", strings.TrimSpace(line)))
	}
	if strings.Count(location, leanAndMeanGuard) != 1 {
		problems = append(problems, fmt.Sprintf("InstallLocationWin32.cpp does not hold its WIN32_LEAN_AND_MEAN guard "+
			"exactly once, as %q", leanAndMeanGuard))
	}
	for _, line := range preprocessorConditional.FindAllString(strings.Replace(location, leanAndMeanGuard, "", 1), -1) {
		problems = append(problems, fmt.Sprintf("InstallLocationWin32.cpp has a preprocessor conditional besides its "+
			"WIN32_LEAN_AND_MEAN guard, %q: no build may read the marker its own way", strings.TrimSpace(line)))
	}
	return problems
}

func TestUpdateApplyTheChannelBuildsAlike(t *testing.T) {
	channel := stripComments(readCommonSource(t, "UpdateChannel.h"))
	location := stripComments(readCommonSource(t, "InstallLocationWin32.cpp"))
	reportProblems(t, checkTheChannelBuildsAlike(channel, location))
	replace := func(t *testing.T, text, old, replacement string) string {
		t.Helper()
		if strings.Count(text, old) != 1 {
			t.Fatalf("negative control: %q is not in the source exactly once", old)
		}
		return strings.Replace(text, old, replacement, 1)
	}
	const absent = "  if (marker.found == Found::Absent) return {ChannelMarker::Absent, {}};\n"
	const reader = "update::ChannelMarker ReadChannelMarker(const fs::path& installFolder, std::string& why, bool* present) {\n"
	for _, tc := range []struct {
		name, want string
		check      func(t *testing.T) []string
	}{
		// the review's X1: a default inside JudgeMarker
		{"a default inside JudgeMarker", "UpdateChannel.h has a preprocessor conditional", func(t *testing.T) []string {
			return checkTheChannelBuildsAlike(replace(t, channel, absent, "#if defined(URN_DEVELOPER_BY_DEFAULT)\n"+
				"  if (marker.found == Found::Absent) return {ChannelMarker::Trusted, {}, false};\n#endif\n"+absent), location)
		}},
		// the review's X2: a default at the top of ReadChannelMarker
		{"a default at the top of ReadChannelMarker", "InstallLocationWin32.cpp has a preprocessor conditional besides", func(t *testing.T) []string {
			return checkTheChannelBuildsAlike(channel, replace(t, location, reader, reader+"#if defined(URN_DEVELOPER_BY_DEFAULT)\n"+
				"  if (::GetFileAttributesW((installFolder / L\"updates\").c_str()) == INVALID_FILE_ATTRIBUTES) return update::ChannelMarker::Trusted;\n"+
				"#endif\n"))
		}},
		// the guard made to carry a default of its own
		{"a guard that defines more than WIN32_LEAN_AND_MEAN", "does not hold its WIN32_LEAN_AND_MEAN guard exactly once", func(t *testing.T) []string {
			return checkTheChannelBuildsAlike(channel, replace(t, location, leanAndMeanGuard,
				"#ifndef WIN32_LEAN_AND_MEAN\n#define WIN32_LEAN_AND_MEAN\n#define URN_DEVELOPER_BY_DEFAULT\n#endif\n"))
		}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			problems := strings.Join(tc.check(t), "\n")
			if !strings.Contains(problems, tc.want) {
				t.Fatalf("negative control was not detected by %q; the check reported:\n%s", tc.want, problems)
			}
		})
	}
}

// Every response the helper reads came with redirects refused and a
// certificate the machine's own trust accepts for the host.
func checkUpdaterHttp(http string) []string {
	get := applyDefinition(http, "bool HttpGet(")
	problems := applyOrderProblems("HttpGet", get,
		regexp.QuoteMeta("parts.nScheme != INTERNET_SCHEME_HTTPS"),
		regexp.QuoteMeta("WINHTTP_FLAG_SECURE));"),
		regexp.QuoteMeta("DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;"),
		regexp.QuoteMeta("WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy))"),
		regexp.QuoteMeta("::WinHttpReceiveResponse(request.get(), nullptr)"),
		regexp.QuoteMeta("if (!MachineTrusts(request.get(), host, error)) return false;"),
		regexp.QuoteMeta("WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER"),
		regexp.QuoteMeta("if (status != 200 || !sink) return true;"),
		regexp.QuoteMeta("if (total > maxBytes) {"))
	trust := applyDefinition(http, "bool MachineTrusts(")
	problems = append(problems, applyOrderProblems("MachineTrusts", trust,
		regexp.QuoteMeta("WINHTTP_OPTION_SERVER_CERT_CONTEXT"),
		regexp.QuoteMeta("::CertGetCertificateChain(HCCE_LOCAL_MACHINE, certificate, nullptr,"),
		regexp.QuoteMeta("https.dwAuthType = AUTHTYPE_SERVER;"),
		regexp.QuoteMeta("https.pwszServerName = const_cast<wchar_t*>(host.c_str());"),
		regexp.QuoteMeta("::CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &status);"),
		regexp.QuoteMeta("if (!checked || status.dwError != 0) {"),
		regexp.QuoteMeta("return false;"))...)
	return problems
}

func TestUpdateApplyHelperTrustsOnlyTheMachine(t *testing.T) {
	reportProblems(t, checkUpdaterHttp(updaterSources(t)["Http.cpp"]))
}

// The UpgradeCode the helper requires is the one the package carries.
func TestUpdateApplyUpgradeCodeIsThePackages(t *testing.T) {
	document := parseXML(t, filepath.Join(repositoryRoot(t), "app", "installer", "Package.wxs"))
	packages := document.descendants(wixNamespace, "Package")
	if len(packages) != 1 {
		t.Fatalf("Package.wxs has %d Package elements", len(packages))
	}
	upgradeCode, _ := packages[0].attribute("UpgradeCode")
	want := `inline constexpr std::string_view kUpgradeCode = "{` + strings.ToUpper(upgradeCode) + `}";`
	if !strings.Contains(stripComments(readCommonSource(t, "UpdateApply.h")), want) {
		t.Errorf("UpdateApply.h does not require Package.wxs's UpgradeCode: want %s", want)
	}
}

// A runner test's feed reaches a build only through the four
// /p:UrnUpdateRunnerTest* properties, never from the environment, and no file
// in this repository a build reads sets one: every build script, MSBuild
// project, props, targets and response file, and workflow. A helper built
// with it says so in its FileDescription, and the release build and the
// payload check refuse to package one, whatever passed the properties.
//
// No build has a default update channel either: every build follows the
// official feed until an administrator turns the developer one on, for the
// whole PC. A default would be a second thing that picks the feed, set where
// a build host's environment reaches it (MSBuild reads variables as
// properties), and the tray app and the helper are two projects that would
// both have to get it. What this refuses is three spellings of one:
// URN_UPDATE_FEED_DEFAULT, UrnUpdateFeedDefault and UrnUpdateChannelDefault,
// whatever the value and in any case, in every build input below, the
// helper's own project included. A default under another name is not
// refused here; FeedOfMarker, the one function that turns the marker into a
// feed, is pinned whole instead (checkMarkerSelectsTheFeedAlone).
func checkRunnerFeedIsTestOnly(inputs map[string]string, project, resources, apply string,
	sources map[string]string) []string {
	var problems []string
	for name, text := range inputs {
		for _, setting := range []string{"URN_UPDATE_FEED_DEFAULT", "UrnUpdateFeedDefault", "UrnUpdateChannelDefault"} {
			if strings.Contains(strings.ToLower(text), strings.ToLower(setting)) {
				problems = append(problems, name+" names "+setting+
					": no build picks the update channel, an administrator does")
			}
		}
		if name == "app/src/Updater/Updater.vcxproj" {
			continue
		}
		for _, setting := range []string{"UrnUpdateRunnerTest", "URN_UPDATE_RUNNER_TEST"} {
			if strings.Contains(text, setting) {
				problems = append(problems, name+" sets "+setting+": official builds must poll the official feed")
			}
		}
	}
	if !strings.Contains(project, `<ItemDefinitionGroup Condition="'$(UrnUpdateRunnerTestRepoId)'!=''">`) {
		problems = append(problems, "Updater.vcxproj defines a runner feed without its property")
	}
	if regexp.MustCompile(`<UrnUpdateRunnerTest[A-Za-z]*>`).MatchString(project) {
		problems = append(problems, "Updater.vcxproj gives a runner feed property a value of its own")
	}
	if !strings.Contains(project, "$([System.Environment]::GetEnvironmentVariable(`UrnUpdateRunnerTestRepoId`))") {
		problems = append(problems, "Updater.vcxproj takes a runner feed from the environment")
	}
	if !strings.Contains(project, "<ResourceCompile>\n      <PreprocessorDefinitions>URN_UPDATE_RUNNER_TEST_REPO_ID=$(UrnUpdateRunnerTestRepoId);") {
		problems = append(problems, "Updater.vcxproj does not tell Updater.rc that a runner feed is compiled in")
	}
	if !strings.Contains(resources, "#if defined(URN_UPDATE_RUNNER_TEST_REPO_ID)\n"+
		`            VALUE "FileDescription",  "URnetwork update (runner test feed)"`+"\n#else\n"+
		`            VALUE "FileDescription",  "URnetwork update"`+"\n#endif") {
		problems = append(problems, "Updater.rc does not mark a helper built with a runner feed")
	}
	build := inputs["app/build.ps1"]
	if marked := strings.Index(build, `$helperDescription = [Diagnostics.FileVersionInfo]::GetVersionInfo($helper).FileDescription`); marked < 0 ||
		!strings.Contains(build, `if ($helperDescription -ne "URnetwork update") {`) ||
		strings.Index(build, "dotnet @wixArgs") < marked {
		problems = append(problems, "app/build.ps1 packages an update helper without checking that it is the official feed's")
	}
	verify := inputs["app/tools/verify-msi-payload.ps1"]
	if !strings.Contains(verify, `$description = [Diagnostics.FileVersionInfo]::GetVersionInfo($extracted).FileDescription`) ||
		!strings.Contains(verify, `if ($description -ne "URnetwork update") {`) {
		problems = append(problems, "verify-msi-payload.ps1 passes an MSI without checking its update helper's feed")
	}
	if !strings.Contains(apply, "#if defined(URN_UPDATE_RUNNER_TEST_REPO_ID)\n  return kRunnerTestFeed;\n#else") {
		problems = append(problems, "ApplyUpdate.cpp reads the runner feed outside its define")
	}
	if strings.Count(apply, "kRunnerTestFeed") != 2 {
		problems = append(problems, "ApplyUpdate.cpp uses kRunnerTestFeed somewhere other than the channel's feed")
	}
	for name, source := range sources {
		if name != "ApplyUpdate.cpp" && strings.Contains(source, "URN_UPDATE_RUNNER_TEST") {
			problems = append(problems, name+" reads a runner feed: only the helper's channel may")
		}
	}
	return problems
}

// Every file in the repository a build reads, by kind, keyed by its slash
// path: build and test scripts, MSBuild projects, props, targets and
// response files, the solution, and workflows. Build output folders are
// skipped.
func buildInputs(t *testing.T) map[string]string {
	t.Helper()
	root := repositoryRoot(t)
	kinds := map[string]bool{".props": true, ".targets": true, ".rsp": true, ".vcxproj": true, ".sln": true,
		".wixproj": true, ".ps1": true, ".psm1": true, ".cmd": true, ".bat": true, ".sh": true, ".yml": true,
		".yaml": true}
	skip := map[string]bool{".git": true, "build": true, "bin": true, "obj": true, "packages": true,
		"node_modules": true, "vcpkg_installed": true, ".vs": true}
	inputs := map[string]string{}
	err := filepath.WalkDir(root, func(path string, entry iofs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if entry.IsDir() {
			if path != root && skip[entry.Name()] {
				return filepath.SkipDir
			}
			return nil
		}
		if !kinds[strings.ToLower(filepath.Ext(entry.Name()))] {
			return nil
		}
		data, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		relative, err := filepath.Rel(root, path)
		if err != nil {
			return err
		}
		inputs[filepath.ToSlash(relative)] = string(data)
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"app/build.ps1", "app/Directory.Build.props", "app/URnetwork.sln",
		"app/src/Updater/Updater.vcxproj", "app/tools/verify-msi-payload.ps1"} {
		if _, ok := inputs[want]; !ok {
			t.Fatalf("the build inputs miss %s: the walk did not see the repository", want)
		}
	}
	return inputs
}

func TestUpdateApplyRunnerFeedIsTestOnly(t *testing.T) {
	sources := updaterSources(t)
	appSources := appSourceFiles(t, ".cpp", ".h")
	for name, source := range appSources {
		sources["App/"+name] = stripComments(source)
	}
	reportProblems(t, checkRunnerFeedIsTestOnly(buildInputs(t), readUpdaterSource(t, "Updater.vcxproj"),
		readUpdaterSource(t, "Updater.rc"), sources["ApplyUpdate.cpp"], sources))
}

// Each check above fails on a source that drops the defence it pins.
func TestUpdateApplyWiringRejectsWeakerHelpers(t *testing.T) {
	root := repositoryRoot(t)
	solutionData, err := os.ReadFile(filepath.Join(root, "app", "URnetwork.sln"))
	if err != nil {
		t.Fatal(err)
	}
	project := readUpdaterSource(t, "Updater.vcxproj")
	resources := readUpdaterSource(t, "Updater.rc")
	solution := string(solutionData)
	common := readCommonSource(t, "Common.vcxproj")
	sources := updaterSources(t)
	inputs := buildInputs(t)
	replace := func(text, old, replacement string) string {
		if strings.Count(text, old) != 1 {
			t.Fatalf("negative control: %q is not in the source exactly once", old)
		}
		return strings.Replace(text, old, replacement, 1)
	}
	with := func(name, text string) map[string]string {
		copied := map[string]string{}
		for key, value := range sources {
			copied[key] = value
		}
		copied[name] = text
		return copied
	}
	withInput := func(name, text string) map[string]string {
		copied := map[string]string{}
		for key, value := range inputs {
			copied[key] = value
		}
		copied[name] = text
		return copied
	}
	runnerFeed := func(changed map[string]string) []string {
		return checkRunnerFeedIsTestOnly(changed, project, resources, sources["ApplyUpdate.cpp"], sources)
	}
	apply := func(old, replacement string) []string {
		return checkApplyUpdate(replace(sources["ApplyUpdate.cpp"], old, replacement))
	}
	// Rewrites `old` inside the definition that starts at `opener`.
	inside := func(source, opener, old, replacement string) string {
		definition := applyDefinition(source, opener)
		if definition == "" {
			t.Fatalf("negative control: %q is not defined", opener)
		}
		return replace(source, definition, replace(definition, old, replacement))
	}
	helper := helperFiles(t)
	mutexIn := func(name, old, replacement string) []string {
		changed := map[string]string{}
		for key, value := range helper {
			changed[key] = value
		}
		changed[name] = replace(helper[name], old, replacement)
		return checkHelperHasNoMutex(changed)
	}
	for _, tc := range []struct {
		name  string
		check func() []string
	}{
		{"the UpgradeCode check skipped", func() []string {
			return apply("!update::PackageMatches(*upgradeCode, *productVersion, selection.code)", "false")
		}},
		{"the digest compared to nothing", func() []string {
			return apply("!update::EqualsAsciiCaseless(actual, selection.digestHex)", "false")
		}},
		{"any offered release installed", func() []string {
			return apply("if (!update::SelectionOffers(selection, tag, version::kCode)) {", "if (selection.code == 0) {")
		}},
		{"a redirect anywhere", func() []string {
			return apply("if (redirect.status != 302 || !update::IsAllowedAssetRedirect(redirect.location) ||",
				"if (redirect.status != 302 ||")
		}},
		{"hashed after the handle is gone", func() []string {
			return apply("const std::string actual = Sha256(held.get());", "const std::string actual = HashPath(package);")
		}},
		{"the image left in the installer's way", func() []string {
			return apply("::MoveFileExW(executable.c_str(), running.c_str(), MOVEFILE_WRITE_THROUGH)", "false")
		}},
		{"a user-writable location", func() []string {
			return checkApplyUpdate(inside(sources["ApplyUpdate.cpp"], "int ApplyUpdate(std::wstring_view tagArgument) {",
				"!install::AdminOnlyLocation(executable, why)", "false"))
		}},
		{"GitHub's limit taken for a broken list", func() []string {
			return apply("if (list.status == 403 || list.status == 429) {", "if (false) {")
		}},
		{"the repair source left to what it inherits", func() []string {
			return apply("fileSecurity.attributes(), CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));",
				"nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));")
		}},
		{"msiexec's log left to msiexec", func() []string {
			return apply("fileSecurity.attributes(), CREATE_NEW,\n", "nullptr, CREATE_NEW,\n")
		}},
		{"what the helper writes owned by whoever creates it", func() []string {
			// anchored on the whole file SDDL: the lock file's SDDL now shares
			// the O:BAD:P(A;;FA;;;SY) prefix, so this drops the owner from
			// kAdminOnlyFileSddl in particular
			return apply(`L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;BU)";`, `L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;BU)";`)
		}},
		{"a folder inheriting what is above it", func() []string {
			return apply(`L"O:BAD:P(A;OICI;FA;;;SY)`, `L"O:BAD:(A;OICI;FA;;;SY)`)
		}},
		{"an existing folder's security kept", func() []string {
			return apply("const DWORD set = ::SetSecurityInfo(", "const DWORD set = ERROR_SUCCESS; (void)(")
		}},
		// The lock, each control restoring one rejected design.
		{"the lock taken as a global mutex again", func() []string {
			return mutexIn("ApplyUpdate.cpp", "const HANDLE raw = ::CreateFileW(",
				`const HANDLE raw = ::CreateMutexW(nullptr, FALSE, L"Global\\URnetworkUpdateHelper"); const HANDLE unused = ::CreateFileW(`)
		}},
		{"a global mutex taken in another file of the helper", func() []string {
			return mutexIn("main.cpp", "    DropAppOverrides();\n",
				"    DropAppOverrides();\n    ::CreateMutexW(nullptr, FALSE, L\"Global\\\\URnetworkUpdateHelper\");\n")
		}},
		{"the lock file shared for read", func() []string {
			return apply("FILE_READ_DATA | READ_CONTROL | WRITE_DAC | WRITE_OWNER | FILE_READ_ATTRIBUTES, 0,",
				"FILE_READ_DATA | READ_CONTROL | WRITE_DAC | WRITE_OWNER | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,")
		}},
		{"the lock file lets Users open it for data", func() []string {
			return apply(`constexpr wchar_t kLockFileSddl[] = L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)";`,
				`constexpr wchar_t kLockFileSddl[] = L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;BU)";`)
		}},
		{"the lock taken before the folder is prepared", func() []string {
			// the same text, moved below the lock: only its place changes
			prepared := "  std::string error;\n  if (!PrepareFolder(updates, folderSecurity, error)) {\n    log.Line(\"refused: {}\", error);\n    return Refusal::Staging;\n  }\n"
			moved := replace(sources["ApplyUpdate.cpp"], prepared+"  Security lockSecurity(kLockFileSddl);",
				"  Security lockSecurity(kLockFileSddl);")
			return checkApplyUpdate(replace(moved, "  return std::nullopt;\n}", prepared+"  return std::nullopt;\n}"))
		}},
		{"a link to the lock file followed", func() []string {
			return apply("lockSecurity.attributes(), OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);",
				"lockSecurity.attributes(), OPEN_ALWAYS, 0, nullptr);")
		}},
		{"the lock let go as soon as it is taken", func() []string {
			return apply("  return std::nullopt;\n}", "  lock.Close();\n  return std::nullopt;\n}")
		}},
		{"the lock let go while the installer runs", func() []string {
			return apply("  ::WaitForSingleObject(installer.get(), INFINITE);\n",
				"  helperLock.Close();\n  ::WaitForSingleObject(installer.get(), INFINITE);\n")
		}},
		{"the lock's handle ending before the helper does", func() []string {
			return checkApplyUpdate(inside(sources["ApplyUpdate.cpp"], "int ApplyUpdate(std::wstring_view tagArgument) {",
				"  Handle helperLock;\n  if (const std::optional<Refusal> refusal = TakeHelperLock(updates, folderSecurity, helperLock, log)) {\n    return static_cast<int>(*refusal);\n  }\n",
				"  {\n    Handle helperLock;\n    if (const std::optional<Refusal> refusal = TakeHelperLock(updates, folderSecurity, helperLock, log)) {\n      return static_cast<int>(*refusal);\n    }\n  }\n"))
		}},
		{"the rights not checked before msiexec", func() []string {
			return apply("!CheckAdminOnly({updates, tagFolder, package, installLog}, error)", "false")
		}},
		{"a non-admin's rights not judged", func() []string {
			return checkAdminOnlyPath(replace(sources["InstallLocationWin32.cpp"],
				"if ((rights & kWriteRights) == 0) return true;", "return true;"))
		}},
		{"no relaunch after an install that failed", func() []string {
			return apply("} else if (StartThroughShell(executable, shellError)) {", "} else if (false) {")
		}},
		{"the relaunch before the report", func() []string {
			moved := replace(sources["ApplyUpdate.cpp"], "  const int ended = finish(exitCode);\n", "")
			return checkApplyUpdate(replace(moved, "  return ended;\n}", "  const int ended = finish(exitCode);\n  return ended;\n}"))
		}},
		{"the relaunch started elevated by the helper", func() []string {
			return checkShellStart(replace(sources["ShellStart.cpp"],
				"hr = shell->ShellExecute(file.get(), arguments, directory, operation, show);",
				"hr = ::CreateProcessW(program.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, nullptr, nullptr) ? S_OK : E_FAIL;"))
		}},
		{"redirects followed", func() []string {
			return checkUpdaterHttp(replace(sources["Http.cpp"],
				"DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;", "DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;"))
		}},
		{"the user's roots trusted", func() []string {
			return checkUpdaterHttp(replace(sources["Http.cpp"],
				"::CertGetCertificateChain(HCCE_LOCAL_MACHINE, certificate, nullptr,",
				"::CertGetCertificateChain(nullptr, certificate, nullptr,"))
		}},
		{"no certificate check", func() []string {
			return checkUpdaterHttp(replace(sources["Http.cpp"],
				"if (!MachineTrusts(request.get(), host, error)) return false;", ""))
		}},
		{"the dynamic C runtime", func() []string {
			return checkUpdaterProject(replace(project,
				"<RuntimeLibrary Condition=\"'$(Configuration)'=='Release'\">MultiThreaded</RuntimeLibrary>",
				"<RuntimeLibrary Condition=\"'$(Configuration)'=='Release'\">MultiThreadedDLL</RuntimeLibrary>"),
				solution, common)
		}},
		{"imports searched in the app's folder", func() []string {
			return checkUpdaterProject(replace(project, "/DEPENDENTLOADFLAG:0x800 ", ""), solution, common)
		}},
		{"libraries loaded before the search is limited", func() []string {
			main := replace(sources["main.cpp"],
				"  ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);\n", "")
			main = replace(main, "  if (!argv) return",
				"  ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);\n  if (!argv) return")
			return checkUpdaterEntry(main, with("main.cpp", main))
		}},
		{"an app override read", func() []string {
			changed := inside(sources["ApplyUpdate.cpp"], "int ApplyUpdate(std::wstring_view tagArgument) {",
				"  const fs::path installFolder = executable.parent_path();",
				"  const fs::path installFolder = _wgetenv(L\"URNETWORK_APP_ROOT\");")
			return checkUpdaterEntry(sources["main.cpp"], with("ApplyUpdate.cpp", changed))
		}},
		{"the user's overrides dropped for the relaunch too", func() []string {
			main := replace(sources["main.cpp"], "    DropAppOverrides();\n", "")
			main = replace(main, "  int argc = 0;", "  DropAppOverrides();\n  int argc = 0;")
			return checkUpdaterEntry(main, with("main.cpp", main))
		}},
		{"the relaunch started elevated", func() []string {
			main := replace(sources["main.cpp"], "  if (IsFullyElevated()) return 0;\n", "")
			return checkUpdaterEntry(main, with("main.cpp", main))
		}},
		{"the relaunch's elevation test inverted", func() []string {
			main := replace(sources["main.cpp"], "return !read || type == TokenElevationTypeFull;",
				"return !read || type == TokenElevationTypeLimited;")
			return checkUpdaterEntry(main, with("main.cpp", main))
		}},
		{"a channel read from the user's registry", func() []string {
			changed := replace(sources["ApplyUpdate.cpp"], "#else\n  std::string why;\n",
				"#else\n  ::RegGetValueW(HKEY_CURRENT_USER, L\"Software\\\\URnetwork\", L\"Channel\", RRF_RT_REG_SZ, nullptr, nullptr, nullptr);\n  std::string why;\n")
			return checkChannelIsAnIndex(changed, with("ApplyUpdate.cpp", changed))
		}},
		{"a feed built at run time", func() []string {
			changed := replace(sources["ApplyUpdate.cpp"], "  return update::FeedOfMarker(marker);\n#endif",
				"  static const update::Feed stored{.id = \"stored\", .numericRepoId = 1, .owner = \"someone\", .repo = \"else\"};\n  return stored;\n#endif")
			return checkChannelIsAnIndex(changed, with("ApplyUpdate.cpp", changed))
		}},
		{"build.ps1 passes a runner feed", func() []string {
			return runnerFeed(withInput("app/build.ps1", inputs["app/build.ps1"]+"\n/p:UrnUpdateRunnerTestRepoId=1\n"))
		}},
		{"build.ps1 passes another feed", func() []string {
			return runnerFeed(withInput("app/build.ps1", inputs["app/build.ps1"]+"\n/p:UrnUpdateFeedDefault=beta\n"))
		}},
		// No build-time default channel, each way the rejected design could
		// come back (DESIGN-v2's /p:UrnUpdateFeedDefault=beta).
		{"a props file names the official feed as the default", func() []string {
			return runnerFeed(withInput("app/Directory.Build.props", inputs["app/Directory.Build.props"]+
				"\n<PropertyGroup><UrnUpdateFeedDefault>official</UrnUpdateFeedDefault></PropertyGroup>\n"))
		}},
		{"the helper's project maps a feed default", func() []string {
			return runnerFeed(withInput("app/src/Updater/Updater.vcxproj", replace(inputs["app/src/Updater/Updater.vcxproj"],
				"  <Target Name=\"UrnCheckRunnerTestFeed\" BeforeTargets=\"ClCompile\">\n",
				"  <ItemDefinitionGroup Condition=\"'$(UrnUpdateFeedDefault)'!=''\">\n"+
					"    <ClCompile>\n      <PreprocessorDefinitions>URN_UPDATE_FEED_DEFAULT_DEVELOPER;%(PreprocessorDefinitions)</PreprocessorDefinitions>\n    </ClCompile>\n"+
					"  </ItemDefinitionGroup>\n"+
					"  <Target Name=\"UrnCheckRunnerTestFeed\" BeforeTargets=\"ClCompile\">\n")))
		}},
		{"a workflow passes a channel default in another case", func() []string {
			return runnerFeed(withInput(".github/workflows/extra.yml", "run: msbuild /p:urnupdatechanneldefault=developer"))
		}},
		{"build.ps1 sets a feed default in the environment", func() []string {
			return runnerFeed(withInput("app/build.ps1", inputs["app/build.ps1"]+"\n$env:UrnUpdateFeedDefault = 'developer'\n"))
		}},
		{"a workflow passes a runner feed", func() []string {
			return runnerFeed(withInput(".github/workflows/extra.yml", "run: msbuild /p:UrnUpdateRunnerTestRepoId=1"))
		}},
		{"a props file sets the runner feed", func() []string {
			return runnerFeed(withInput("app/Directory.Build.props", inputs["app/Directory.Build.props"]+
				"\n<PropertyGroup><UrnUpdateRunnerTestRepoId>1</UrnUpdateRunnerTestRepoId></PropertyGroup>\n"))
		}},
		{"a response file sets the runner feed", func() []string {
			return runnerFeed(withInput("app/Directory.Build.rsp", "/p:UrnUpdateRunnerTestRepoId=1"))
		}},
		{"build.ps1 packages a runner helper", func() []string {
			return runnerFeed(withInput("app/build.ps1", replace(inputs["app/build.ps1"],
				`if ($helperDescription -ne "URnetwork update") {`, `if ($false) {`)))
		}},
		{"the payload check passes a runner helper", func() []string {
			return runnerFeed(withInput("app/tools/verify-msi-payload.ps1", replace(inputs["app/tools/verify-msi-payload.ps1"],
				`if ($description -ne "URnetwork update") {`, `if ($false) {`)))
		}},
		{"a runner helper that does not say so", func() []string {
			return checkRunnerFeedIsTestOnly(inputs, project, replace(resources, " (runner test feed)", ""),
				sources["ApplyUpdate.cpp"], sources)
		}},
		{"the runner feed from the environment", func() []string {
			return checkRunnerFeedIsTestOnly(inputs,
				strings.ReplaceAll(project, "$([System.Environment]::GetEnvironmentVariable(`UrnUpdateRunnerTestRepoId`))", ""),
				resources, sources["ApplyUpdate.cpp"], sources)
		}},
		{"Updater.vcxproj gives the runner feed a value", func() []string {
			return checkRunnerFeedIsTestOnly(inputs, project+
				"\n<PropertyGroup><UrnUpdateRunnerTestRepoId>1</UrnUpdateRunnerTestRepoId></PropertyGroup>\n",
				resources, sources["ApplyUpdate.cpp"], sources)
		}},
		{"the runner feed as the default", func() []string {
			changed := replace(sources["ApplyUpdate.cpp"], "  return update::FeedOfMarker(marker);\n", "  return kRunnerTestFeed;\n")
			return checkRunnerFeedIsTestOnly(inputs, project, resources, changed, sources)
		}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if problems := tc.check(); len(problems) == 0 {
				t.Fatal("negative control was not detected")
			}
		})
	}
}

// The update channel's verb, its question and its reader: each control puts
// back one rejected design, and must be reported for that reason.
func TestUpdateApplyTheChannelVerbRejectsWeakerHelpers(t *testing.T) {
	sources := updaterSources(t)
	apply := sources["ApplyUpdate.cpp"]
	location := sources["InstallLocationWin32.cpp"]
	main := sources["main.cpp"]
	replace := func(text, old, replacement string) string {
		if strings.Count(text, old) != 1 {
			t.Fatalf("negative control: %q is not in the source exactly once", old)
		}
		return strings.Replace(text, old, replacement, 1)
	}
	inside := func(source, opener, old, replacement string) string {
		definition := applyDefinition(source, opener)
		if definition == "" {
			t.Fatalf("negative control: %q is not defined", opener)
		}
		return replace(source, definition, replace(definition, old, replacement))
	}
	verb := func(old, replacement string) []string {
		return checkChannelVerb(replace(apply, old, replacement), location)
	}
	reader := func(old, replacement string) []string {
		return checkChannelVerb(apply, replace(location, old, replacement))
	}
	const opener = "int SetUpdateChannel(std::wstring_view channelArgument) {"
	question := "  if (plan == update::ChannelPlan::AskThenCreate && !AdministratorConfirms(*asked, log)) {\n" +
		"    return static_cast<int>(Refusal::Declined);\n  }\n"
	for _, tc := range []struct {
		name, want string
		check      func() []string
	}{
		// the feed named by the caller
		{"an apply that takes a feed id after its tag", "wWinMain is missing const bool apply", func() []string {
			changed := replace(main, `const bool apply = args.size() == 3 && args[1] == L"--apply-update";`,
				`const bool apply = args.size() == 4 && args[1] == L"--apply-update";`)
			changed = replace(changed, "urnw::updater::ApplyUpdate(args[2])", "urnw::updater::ApplyUpdate(args[2], args[3])")
			return checkUpdaterEntry(changed, sources)
		}},
		{"the channel resolved by its owner and its repository", "SetUpdateChannel is missing const update::Feed", func() []string {
			return verb("const update::Feed* asked = update::FeedOfChannelArgument(AsciiTag(channelArgument));",
				"const update::Feed* asked = nullptr;\n  for (const update::Feed& row : update::kFeeds) {\n"+
					"    if (WidenAscii(row.owner) + L\"/\" + WidenAscii(row.repo) == channelArgument) asked = &row;\n  }")
		}},
		// the channel read from a store the user can write
		{"the marker's path from the user's profile", "ChannelMarkerPath is missing", func() []string {
			return reader(`return installFolder / L"updates" / L"developer-channel";`,
				`return fs::path(_wgetenv(L"LOCALAPPDATA")) / L"URnetwork" / L"developer-channel";`)
		}},
		{"the reader skipping the folder's descriptor", "ReadChannelMarker is missing folderDescriptor = ReadDescriptorFacts", func() []string {
			return reader("    folderDescriptor = ReadDescriptorFacts(updates);\n", "")
		}},
		{"the reader skipping the marker's descriptor", "ReadChannelMarker is missing markerDescriptor = ReadDescriptorFacts", func() []string {
			return reader("      markerDescriptor = ReadDescriptorFacts(marker);\n", "")
		}},
		{"the reader trusting a marker by its name", "ReadChannelMarker is missing update::JudgeMarker", func() []string {
			return reader("update::JudgeMarker(folderFacts, folderDescriptor, markerFacts, markerDescriptor);",
				"update::MarkerJudgement{markerFacts.found == update::PathFacts::Found::Present ? "+
					"update::ChannelMarker::Trusted : update::ChannelMarker::Absent, {}};")
		}},
		// a lock anybody can hold, or a marker it keeps
		{"the channel verb writing without its second read", "SetUpdateChannel is missing plan = update::PlanOf", func() []string {
			return verb("  plan = update::PlanOf(*asked, install::ReadChannelMarker(installFolder, why));\n", "")
		}},
		{"the channel verb's lock let go before it writes", "SetUpdateChannel does not declare helperLock in its own scope", func() []string {
			return checkChannelVerb(inside(apply, opener,
				"  Handle helperLock;\n  if (const std::optional<Refusal> refusal = TakeHelperLock(updates, folderSecurity, helperLock, log)) {\n"+
					"    return static_cast<int>(*refusal);\n  }\n",
				"  {\n    Handle helperLock;\n    if (const std::optional<Refusal> refusal = TakeHelperLock(updates, folderSecurity, helperLock, log)) {\n"+
					"      return static_cast<int>(*refusal);\n    }\n  }\n"), location)
		}},
		{"the folder's descriptor noted after PrepareFolder has set it", `TakeHelperLock is missing if \(!PrepareFolder`, func() []string {
			moved := replace(apply, "  const bool foundOpen = !FoundAsTheHelpers(updates);\n", "")
			moved = replace(moved, "    log.Line(\"refused: {}\", error);\n    return Refusal::Staging;\n  }\n  Security lockSecurity(kLockFileSddl);\n",
				"    log.Line(\"refused: {}\", error);\n    return Refusal::Staging;\n  }\n  const bool foundOpen = !FoundAsTheHelpers(updates);\n  Security lockSecurity(kLockFileSddl);\n")
			return checkApplyUpdate(moved)
		}},
		{"the marker kept when the folder was found open", `TakeHelperLock is missing if \(foundOpen\)`, func() []string {
			return checkApplyUpdate(replace(apply, "  if (foundOpen) {\n", "  if (false) {\n"))
		}},
		{"the lock file made with the marker's security", "TakeHelperLock is missing Security lockSecurity", func() []string {
			return checkApplyUpdate(replace(apply, "  Security lockSecurity(kLockFileSddl);\n", "  Security lockSecurity(kChannelMarkerSddl);\n"))
		}},
		// the fork accepted while the channel says official
		{"ChannelFeed giving the developer row whatever the marker", "ChannelFeed is not the marker", func() []string {
			changed := replace(apply, "  return update::FeedOfMarker(marker);\n", "  return *update::FeedById(update::kDeveloperChannelId);\n")
			return checkChannelIsAnIndex(changed, sources)
		}},
		// the question weakened
		{"OK the default button", `AdministratorConfirms is missing MB_OKCANCEL \| MB_DEFBUTTON2`, func() []string {
			return verb("MB_OKCANCEL | MB_DEFBUTTON2 |", "MB_OKCANCEL | MB_DEFBUTTON1 |")
		}},
		{"the question skipped for the developer channel", `SetUpdateChannel is missing if \(plan == update::ChannelPlan::AskThenCreate && !AdministratorConfirms`, func() []string {
			return verb(question, "")
		}},
		{"the marker written before the question", "SetUpdateChannel is missing Security markerSecurity", func() []string {
			moved := replace(apply, question, "")
			moved = replace(moved, "  if (update::PlanOf(*asked, install::ReadChannelMarker(installFolder, why)) !=\n",
				question+"  if (update::PlanOf(*asked, install::ReadChannelMarker(installFolder, why)) !=\n")
			return checkChannelVerb(moved, location)
		}},
		{"the question asked with no look at the window station", `AdministratorConfirms is missing !\(station\.dwFlags & WSF_VISIBLE\)`, func() []string {
			return verb("      !(station.dwFlags & WSF_VISIBLE)) {", "      false) {")
		}},
		// the review's M1: a box that could not be shown taken for OK
		{"an answer that is not OK taken for yes", `AdministratorConfirms is "`, func() []string {
			return verb("  if (answer == IDOK) {\n", "  if (answer == 0) return true;\n  if (answer == IDOK) {\n")
		}},
		// the review's M3: an open plain updates\ taken for the helper's own
		{"a plain folder taken for the helper's own, whatever its descriptor", `FoundAsTheHelpers is "`, func() []string {
			return checkApplyUpdate(replace(apply, "  if (folder.found == update::PathFacts::Found::Absent) return true;\n  return update::IsHelpersFolder(",
				"  if (folder.found == update::PathFacts::Found::Absent) return true;\n  if (update::IsPlainFolder(folder)) return true;\n  return update::IsHelpersFolder("))
		}},
		{"the question naming a repository of its own", "never one of its own", func() []string {
			return verb(`const std::wstring repository = WidenAscii(feed.owner) + L"/" + WidenAscii(feed.repo);`,
				`const std::wstring repository = L"Ryanmello07/urnetwork-windows";`)
		}},
		// an elevated delete without a bound
		{"the channel verb removing recursively", "removes a file, an empty folder or a link, and nothing else", func() []string {
			return verb(": ::DeleteFileW(marker.c_str());", ": (std::filesystem::remove_all(marker), TRUE);")
		}},
		{"the cleanup after an install removing files too", `ApplyUpdate is missing if \(folder\.is_directory\(ec\)`, func() []string {
			return checkApplyUpdate(replace(apply, "if (folder.is_directory(ec) && folder.path().filename() != tagFolder.filename())",
				"if (folder.path().filename() != tagFolder.filename())"))
		}},
		{"the marker made by the apply too", "names kChannelMarkerSddl 3 times", func() []string {
			return checkChannelVerb(inside(apply, "int ApplyUpdate(std::wstring_view tagArgument) {",
				"  const update::Feed& feed = ChannelFeed(installFolder, log);\n",
				"  Security markerSecurity(kChannelMarkerSddl);\n  const update::Feed& feed = ChannelFeed(installFolder, log);\n"), location)
		}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			problems := strings.Join(tc.check(), "\n")
			if !strings.Contains(problems, tc.want) && !regexp.MustCompile(tc.want).MatchString(problems) {
				t.Fatalf("negative control was not detected by %q; the check reported:\n%s", tc.want, problems)
			}
		})
	}
}

// The tray app hands an elevated process nothing but a tag: on an installed
// copy it starts the helper beside it and waits; on any other copy, and when
// the user asks for the installer, it shows the checked installer and
// elevates nothing. It never builds msiexec's arguments, and it does not quit
// for the installer: the installer closes it, and only then, as it exits,
// does it record the helper in the update marker. Its wait ends with the
// helper or with the app's own exit, whichever is first.
func checkTrayRunsTheHelper(checker string) []string {
	var problems []string
	start := applyDefinition(checker, "void UpdateChecker::Start() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::Start", start,
		regexp.QuoteMeta("const fs::path exe = install::OwnExecutablePath();"),
		regexp.QuoteMeta("installed_ = !exe.empty() && install::AdminOnlyLocation(installFolder_ / kHelperName, why);"),
		regexp.QuoteMeta("snapshot_.installed = installed_;"),
		regexp.QuoteMeta("worker_ = std::thread("))...)
	apply := applyDefinition(checker, "void UpdateChecker::RunApply(std::uint64_t generation, bool manual) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunApply", apply,
		regexp.QuoteMeta("const bool viaHelper = installed_ && !manual;"),
		regexp.QuoteMeta("const auto cancelled = [this] {\n    std::lock_guard lock(mutex_);\n    return stop_;\n  };"),
		regexp.QuoteMeta("const fs::path dir = UpdatesDir() / offer.tag;"),
		regexp.QuoteMeta("const std::string actual = Sha256File(msiPath);"),
		regexp.QuoteMeta("fail(Failure::Checksum);"),
		regexp.QuoteMeta("if (!viaHelper) {"),
		regexp.QuoteMeta("s.phase = Phase::ManualInstall;"),
		regexp.QuoteMeta("RevealInExplorer(msiW);"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta("LaunchUpdateHelper(installFolder_ / kHelperName, offer.tag, &helper, &refusal,"),
		`fail\(refusal == kElevationRefusedUnsigned && update::HandsOverInstaller\(\*offer\.feed\)\s+\? Failure::Unsigned\s+: Failure::Elevation\);`,
		regexp.QuoteMeta("[helper] { return ::WaitForSingleObject(helper, 250) == WAIT_OBJECT_0; }, cancelled);"),
		regexp.QuoteMeta("if (waited == update::HelperWait::AppExiting) {"),
		regexp.QuoteMeta("RecordUpdateInProgress(helper, NoteVpn());"),
		regexp.QuoteMeta("::CloseHandle(helper);"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta(`ReadResult(installFolder_ / L"updates" / L"last-result.json");`),
		regexp.QuoteMeta("s.phase = Phase::Result;"))...)
	if portable := strings.Index(apply, "if (!viaHelper) {"); portable >= 0 {
		branch := apply[portable:]
		if end := strings.Index(branch, "\n  }\n"); end >= 0 {
			branch = branch[:end]
		}
		for _, forbidden := range []string{"LaunchUpdateHelper", "runas", "ShellExecute"} {
			if strings.Contains(branch, forbidden) {
				problems = append(problems, "the installer branch of RunApply runs "+forbidden+
					": a copy outside an admin-only install, or a user who asked for the installer, elevates nothing")
			}
		}
	}
	launch := applyDefinition(checker,
		"bool LaunchUpdateHelper(fs::path const& helperPath, std::wstring const& tag, HANDLE* helper,")
	problems = append(problems, applyOrderProblems("LaunchUpdateHelper", launch,
		regexp.QuoteMeta(`const std::wstring params = L"--apply-update " + tag;`),
		regexp.QuoteMeta(`sei.lpVerb = L"runas";`),
		regexp.QuoteMeta("sei.lpFile = helperPath.c_str();"),
		regexp.QuoteMeta("sei.lpParameters = params.c_str();"),
		regexp.QuoteMeta("*refusal = ::GetLastError();"),
		regexp.QuoteMeta("*helper = sei.hProcess;"))...)
	if strings.Contains(launch, "RecordUpdateInProgress") {
		problems = append(problems, "LaunchUpdateHelper records the helper as it starts: launches during the "+
			"helper's download would be refused while this app still runs to serve them")
	}
	if !strings.Contains(checker, "constexpr DWORD kElevationRefusedUnsigned = ERROR_DS_REFERRAL;") {
		problems = append(problems, "UpdateChecker.cpp does not name the elevation's answer for an unsigned program")
	}
	if !strings.Contains(checker, `constexpr wchar_t kHelperName[] = L"URnetworkUpdate.exe";`) {
		problems = append(problems, "UpdateChecker.cpp does not name the helper URnetworkUpdate.exe")
	}
	for _, forbidden := range []string{"msiexec", "/passive", "/l*v", "SetInstallerStartedHandler",
		"InstallerStarted"} {
		if strings.Contains(checker, forbidden) {
			problems = append(problems, "UpdateChecker.cpp has "+forbidden+
				": only the elevated helper builds msiexec's command line, and the app does not quit for it")
		}
	}
	return problems
}

func TestUpdateApplyTrayRunsTheHelper(t *testing.T) {
	reportProblems(t, checkTrayRunsTheHelper(stripComments(readAppSource(t, "UpdateChecker.cpp"))))
}

// The helper's report is read from the admin-only install folder only, and
// shown until the user dismisses it, for as long as it is still true of the
// build that reads it (UpdateResult.h ViewOfReport): a 3010 until Windows has
// restarted, a failure until this build reaches that release. After the
// helper this app waited on ends, it is that run's report only when its tag,
// its exit code and its time say so; otherwise the exit code alone.
func checkTrayReadsTheReport(checker string) []string {
	var problems []string
	show := applyDefinition(checker, "void UpdateChecker::ShowLastResult() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::ShowLastResult", show,
		regexp.QuoteMeta("if (!installed_) return;"),
		regexp.QuoteMeta(`ReadResult(installFolder_ / L"updates" / L"last-result.json");`),
		regexp.QuoteMeta("if (SeenResult() == result->finishedUtc) return;"),
		regexp.QuoteMeta("const std::optional<std::int64_t> finished = update::ParseUtcSecond(result->finishedUtc);"),
		regexp.QuoteMeta("const bool restartedSince = finished && BootUnixSeconds() > *finished;"),
		`update::ViewOfReport\(\*result, version::kCode,\s*false, restartedSince\);`,
		regexp.QuoteMeta("if (view == update::ReportView::Hidden) {"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta("s.phase = Phase::Result;"),
		regexp.QuoteMeta(".view = view};"))...)
	boot := applyDefinition(checker, "std::int64_t BootUnixSeconds() {")
	problems = append(problems, applyOrderProblems("BootUnixSeconds", boot,
		regexp.QuoteMeta("return NowUnixSeconds() - static_cast<std::int64_t>(::GetTickCount64() / 1000);"))...)
	read := applyDefinition(checker, "std::optional<update::UpdateResult> ReadResult(fs::path const& file) {")
	problems = append(problems, applyOrderProblems("ReadResult", read,
		regexp.QuoteMeta("return update::ParseUpdateResult(text);"))...)
	apply := applyDefinition(checker, "void UpdateChecker::RunApply(std::uint64_t generation, bool manual) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunApply", apply,
		regexp.QuoteMeta("const std::int64_t started = NowUnixSeconds();"),
		regexp.QuoteMeta("LaunchUpdateHelper(installFolder_ / kHelperName, offer.tag, &helper, &refusal,"),
		regexp.QuoteMeta("const std::int64_t ended = read ? static_cast<std::int64_t>(exitCode) : 1603;"),
		regexp.QuoteMeta(`ReadResult(installFolder_ / L"updates" / L"last-result.json");`),
		regexp.QuoteMeta("if (report && update::IsReportOfRun(*report, outcome.tag, ended, started)) {"),
		`result\.view = update::ViewOfReport\(outcome, version::kCode,\s*true,\s*false\);`,
		regexp.QuoteMeta("s.phase = Phase::Result;"))...)
	worker := applyDefinition(checker, "void UpdateChecker::WorkerLoop() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::WorkerLoop", worker,
		regexp.QuoteMeta("ShowLastResult();"), regexp.QuoteMeta("CleanupStaleFiles();"))...)
	dismiss := applyDefinition(checker, "void UpdateChecker::DismissResult() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::DismissResult", dismiss,
		regexp.QuoteMeta("if (!finished.empty()) SaveAppPref(kResultSeenPrefKey, Narrow(finished));"))...)
	check := applyDefinition(checker, "bool UpdateChecker::RunCheck(std::uint64_t generation) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunCheck", check,
		regexp.QuoteMeta("if (snapshot_.phase != Phase::None && snapshot_.phase != Phase::Result) {"))...)
	return problems
}

func TestUpdateApplyTrayReadsTheReport(t *testing.T) {
	reportProblems(t, checkTrayReadsTheReport(stripComments(readAppSource(t, "UpdateChecker.cpp"))))
}

// A release the helper did not install is not a dead end. The report's
// banner offers that release's installer while a check still offers it
// (OffersInstaller), and can be closed; the portable path then runs on any
// copy, reusing the checked download the tray keeps until the update has
// taken; a Windows that elevates only signed programs is offered the
// installer too. The installer shown from the user's folder is checked again
// before every "Show file", since any of the user's processes can write it.
// A release the helper refused as no longer offered is followed by a check,
// run there, so the banner says what the feed offers now
// (update_offer_wiring_test.go pins what that check's answer does).
func checkTrayShowsTheInstaller(checker, window, connect string) []string {
	var problems []string
	offers := applyDefinition(checker, "bool UpdateChecker::OffersInstaller(Snapshot const& snapshot) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::OffersInstaller", offers,
		regexp.QuoteMeta("return snapshot.phase == Phase::Result &&"),
		regexp.QuoteMeta("snapshot.result.view == update::ReportView::NotInstalled && snapshot.offeredCode != 0 &&"),
		regexp.QuoteMeta("snapshot.offeredCode == snapshot.code && update::HandsOverInstaller(*snapshot.feed);"))...)
	worker := applyDefinition(checker, "void UpdateChecker::WorkerLoop() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::WorkerLoop", worker,
		regexp.QuoteMeta("if (applyRequested_ || manualRequested_) {"),
		regexp.QuoteMeta("const bool manual = manualRequested_ && !applyRequested_;"),
		regexp.QuoteMeta("RunApply(generation, manual);"),
		regexp.QuoteMeta("if (revealRequested_) {"),
		regexp.QuoteMeta("RunReveal(generation);"))...)
	apply := applyDefinition(checker, "void UpdateChecker::RunApply(std::uint64_t generation, bool manual) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunApply", apply,
		regexp.QuoteMeta("(manual && snapshot_.phase == Phase::Result);"),
		regexp.QuoteMeta("const bool viaHelper = installed_ && !manual;"),
		regexp.QuoteMeta("if (IsCheckedDownload(msiPath, offer.digestHex)) {"),
		regexp.QuoteMeta("LaunchUpdateHelper("),
		regexp.QuoteMeta("if (update::KeepsPackage(ended)) fs::remove_all(dir, ec);"),
		regexp.QuoteMeta("if (ended == static_cast<std::int64_t>(update::Refusal::NotOffered)) {"),
		regexp.QuoteMeta("RunCheck(generation);"),
		regexp.QuoteMeta("s.offeredCode = offer_.code;"))...)
	if handOff := strings.Index(apply, "LaunchUpdateHelper("); handOff >= 0 {
		if kept := strings.Index(apply, "if (update::KeepsPackage(ended)) fs::remove_all(dir, ec);"); kept > handOff &&
			strings.Contains(apply[handOff:kept], "remove_all(dir") {
			problems = append(problems, "UpdateChecker::RunApply deletes the checked download before the helper's report says the update took")
		}
	}
	reveal := applyDefinition(checker, "void UpdateChecker::RunReveal(std::uint64_t generation) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunReveal", reveal,
		regexp.QuoteMeta("feedGeneration_ != generation || offer_.code != snapshot_.code ||"),
		regexp.QuoteMeta("digest = offer_.digestHex;"),
		regexp.QuoteMeta("const std::string actual = Sha256File(installer);"),
		regexp.QuoteMeta("if (digest.empty() || actual.empty() || !update::EqualsAsciiCaseless(actual, digest)) {"),
		regexp.QuoteMeta("fs::remove(installer, ec);"),
		regexp.QuoteMeta("s.failure = Failure::Checksum;"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta("RevealInExplorer(installer);"))...)
	action := applyDefinition(window, "void MainWindow::OnUpdateBannerAction() {")
	problems = append(problems, applyOrderProblems("MainWindow::OnUpdateBannerAction", action,
		regexp.QuoteMeta("if (updateSnapshot_.failure == Failure::Unsigned && updateSnapshot_.phase == Phase::Failed)"),
		regexp.QuoteMeta("urnw::pages::Updates().ShowInstaller();"),
		regexp.QuoteMeta("urnw::pages::Updates().BeginApply();"),
		regexp.QuoteMeta("case Phase::ManualInstall:"),
		regexp.QuoteMeta("urnw::pages::Updates().RevealInstaller();"),
		regexp.QuoteMeta("case Phase::Result:"),
		regexp.QuoteMeta("if (urnw::UpdateChecker::OffersInstaller(updateSnapshot_))"),
		regexp.QuoteMeta("urnw::pages::Updates().ShowInstaller();"),
		regexp.QuoteMeta("urnw::pages::Updates().DismissResult();"))...)
	if strings.Contains(window, "UpdateChecker::RevealInExplorer(") {
		problems = append(problems, "MainWindow shows the installer from the user's folder without checking it again")
	}
	problems = append(problems, applyOrderProblems("MainWindow's update banner", window,
		regexp.QuoteMeta("UpdateBar().Closed("),
		regexp.QuoteMeta("if (args.Reason() != Microsoft::UI::Xaml::Controls::InfoBarCloseReason::CloseButton) return;"),
		regexp.QuoteMeta("self->updateSnapshot_.phase == urnw::UpdateChecker::Phase::Result"),
		regexp.QuoteMeta("urnw::pages::Updates().DismissResult();"))...)
	banner := applyDefinition(connect, offerBannerRenderer)
	problems = append(problems, applyOrderProblems("ConnectPage::RenderUpdateBanner", banner,
		regexp.QuoteMeta("bar.IsClosable(urnw::UpdateChecker::OffersInstaller(snap));"),
		regexp.QuoteMeta(`action = urnw::UpdateChecker::OffersInstaller(snap) ? winrt::hstring{L"Show the installer"}`),
		regexp.QuoteMeta("case Failure::Unsigned:"),
		regexp.QuoteMeta(`action = winrt::hstring{L"Download the installer"};`))...)
	return problems
}

func TestUpdateApplyTrayShowsTheInstallerWhenTheHelperCannot(t *testing.T) {
	reportProblems(t, checkTrayShowsTheInstaller(stripComments(readAppSource(t, "UpdateChecker.cpp")),
		stripComments(readAppSource(t, "MainWindow.xaml.cpp")), stripComments(readAppSource(t, "ConnectPage.cpp"))))
}

// The banner says what the update does: the VPN disconnects while it
// installs and is not reconnected by itself; the helper's run is its own
// stage; a copy that does not update itself is one outside Program Files;
// and how it went, as the report reads for this build.
func checkTraySaysWhatTheUpdateDoes(connect string) []string {
	var problems []string
	banner := applyDefinition(connect, offerBannerRenderer)
	problems = append(problems, applyOrderProblems("ConnectPage::RenderUpdateBanner", banner,
		regexp.QuoteMeta(`message += L" The VPN disconnects while it installs; connect again once URnetwork is back.";`),
		regexp.QuoteMeta(`message = L"This copy of URnetwork is not installed in Program Files, so it does not "`),
		regexp.QuoteMeta("case Stage::Helper:"),
		regexp.QuoteMeta(`message = L"Installing as administrator: the update is downloaded again and checked "`),
		regexp.QuoteMeta("case Failure::Held:"))...)
	result := applyDefinition(connect, "winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity ConnectPage::ApplyUpdateResult(")
	problems = append(problems, applyOrderProblems("ConnectPage::ApplyUpdateResult", result,
		regexp.QuoteMeta(`L"The VPN was disconnected for the update; connect again to protect this device.";`),
		regexp.QuoteMeta("const bool reconnect = urnw::update::SaysVpnDisconnected(result.vpn);"),
		regexp.QuoteMeta("switch (result.view) {"),
		regexp.QuoteMeta("case ReportView::Installed:"),
		regexp.QuoteMeta("if (reconnect) message = kReconnect;"),
		regexp.QuoteMeta("case ReportView::RestartApp:"),
		regexp.QuoteMeta("case ReportView::RestartWindows:"),
		regexp.QuoteMeta("if (urnw::update::OutcomeOf(result.exitCode) != urnw::update::Outcome::Refused) {"),
		regexp.QuoteMeta("current, vpn, instead);"),
		regexp.QuoteMeta("case Refusal::RateLimited:"))...)
	return problems
}

func TestUpdateApplyTraySaysWhatTheUpdateDoes(t *testing.T) {
	reportProblems(t, checkTraySaysWhatTheUpdateDoes(stripComments(readAppSource(t, "ConnectPage.cpp"))))
}

// The report says "The VPN was disconnected for the update" as the note or the
// record of its update says (fold-in ii, and the lead's G8, G8a, G10 and
// G10a). The tray learns whether a VPN is connected from each tunnel status
// the service pushes (AppController::OnTunnelState), and the update checker
// keeps a note of it (UpdateMarker.h VpnNote). At the hand-off to the helper
// the note starts from the last status pushed, else the state the service
// answered (SdkHost::AdoptedServiceState, through the probe AppController
// hands it, never the sign-in state), else nothing; a VPN up at any moment
// from there counts, and a teardown does not clear it. It is read where it is
// used: a helper that ends while the app still runs has its report read with
// it, and when the installer closes the app, the note goes into the
// update-in-progress record (Common/UpdateMarker.h, update_marker_test.go),
// the launch that finds that update over takes the record it deletes, and the
// report of that update reads it (VpnOfReport). A record that does not say (an
// older build's three numbers) or is another update's keeps the line: not
// telling someone their VPN is off is worse than telling someone who had none.
// The record is taken once, so a later launch that shows the report again has
// none, and leaves the line out: the first showing said what the record said.
// So the record's VPN starts unknown, the report's is the record's word or
// none, the one decision is SaysVpnDisconnected, and the line is named only
// where it is asked for. No preference holds it:
// checkNothingSavedDecidesTheOffer pins three.
func checkTrayRemembersTheVpn(controller, checker, header, glue, connect, marker string) []string {
	var problems []string
	tunnel := applyDefinition(controller, "void AppController::OnTunnelState(const proto::TunnelStatus& status) {")
	problems = append(problems, applyOrderProblems("AppController::OnTunnelState", tunnel,
		regexp.QuoteMeta("const bool isUp = (status.state == proto::TunnelState::Up);"),
		regexp.QuoteMeta("connected_ = isUp;"),
		regexp.QuoteMeta("updates_.SetVpnConnected(isUp);"))...)
	if count := strings.Count(controller, "SetVpnConnected("); count != 1 {
		problems = append(problems, fmt.Sprintf("AppController.cpp tells the update checker about the VPN in %d places, want the tunnel status's one", count))
	}
	// each status into the note, under its lock, and nothing else: the whole
	// body, with its whitespace folded (the lead's G16)
	const pushedOpener = "void UpdateChecker::SetVpnConnected(bool connected) {"
	pushed := strings.Join(strings.Fields(strings.TrimSuffix(strings.TrimPrefix(applyDefinition(checker, pushedOpener), pushedOpener), "\n}")), " ")
	if pushed != "std::lock_guard lock(vpnMutex_); vpn_.StatusPushed(connected);" {
		problems = append(problems, fmt.Sprintf("UpdateChecker::SetVpnConnected is %q, want each status into the note, under its lock, and nothing else", pushed))
	}
	// at the hand-off, the service's answer, for when nothing was pushed (G10a)
	handOffVpn := applyDefinition(checker, "update::VpnNote UpdateChecker::HandOffVpn() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::HandOffVpn", handOffVpn,
		regexp.QuoteMeta("const std::optional<bool> answered = vpnAnswer_ ? vpnAnswer_() : std::nullopt;"),
		regexp.QuoteMeta("std::lock_guard lock(vpnMutex_);"),
		regexp.QuoteMeta("vpn_.HandOff(answered);"))...)
	noteVpn := applyDefinition(checker, "update::VpnAtStart UpdateChecker::NoteVpn() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::NoteVpn", noteVpn,
		regexp.QuoteMeta("std::lock_guard lock(vpnMutex_);"),
		regexp.QuoteMeta("return vpn_.Note();"))...)
	// the service's word, not the sign-in state (G10a): the probe's whole body,
	// with its whitespace folded (the lead's G16)
	const probeOpener = "updates_.SetVpnAnswerProbe([this]() -> std::optional<bool> {"
	probe := strings.Join(strings.Fields(strings.TrimSuffix(strings.TrimPrefix(applyDefinitionUntil(controller, probeOpener, "});"), probeOpener), "});")), " ")
	if probe != "const std::optional<proto::TunnelState> state = sdk_.AdoptedServiceState(); if (!state) return std::nullopt; return *state == proto::TunnelState::Up;" {
		problems = append(problems, fmt.Sprintf("AppController's probe of the service's answer is %q, want the state the service told this process and nothing else, never the sign-in state", probe))
	}
	if count := strings.Count(controller, "SetVpnAnswerProbe("); count != 1 {
		problems = append(problems, fmt.Sprintf("AppController.cpp hands the update checker the service's answer in %d places, want one", count))
	}
	// the sticky note: a VPN up at any moment from the hand-off counts (G10);
	// update_marker_test.go runs it
	vpnNote := applyDefinitionUntil(marker, "struct VpnNote {", "\n};\n")
	problems = append(problems, applyOrderProblems("UpdateMarker.h VpnNote", vpnNote,
		regexp.QuoteMeta("upSinceHandOff = upSinceHandOff || up;"),
		regexp.QuoteMeta("source = pushed ? VpnSource::Pushed : answered ? VpnSource::Answered : VpnSource::Unknown;"),
		regexp.QuoteMeta("const std::optional<bool> state = pushed ? pushed : answered;"),
		regexp.QuoteMeta("upSinceHandOff = state.value_or(false);"),
		regexp.QuoteMeta("if (upSinceHandOff) return VpnAtStart::Connected;"),
		regexp.QuoteMeta("return source == VpnSource::Unknown ? VpnAtStart::Unknown : VpnAtStart::NotConnected;"))...)
	// what a record this launch took does not say keeps the line
	if !strings.Contains(marker, "VpnAtStart vpn = VpnAtStart::Unknown;") {
		problems = append(problems, "UpdateMarker.h does not keep the VPN unknown until a record says it: "+
			"an older build's three-field record reads as no VPN, and its report drops the line")
	}
	// the record this launch took decides; without one the report is shown again
	ofReport := applyDefinition(marker, "inline std::optional<VpnAtStart> VpnOfReport(const std::optional<UpdateMarker>& record,")
	if !strings.Contains(ofReport, "if (!record) return std::nullopt;") {
		problems = append(problems, "UpdateMarker.h's VpnOfReport gives a report this launch took no record for a VPN: "+
			"the record is taken once, so a report shown again would say the VPN was disconnected after an update with none")
	}
	problems = append(problems, applyOrderProblems("UpdateMarker.h VpnOfReport", ofReport,
		regexp.QuoteMeta("if (!record) return std::nullopt;"),
		regexp.QuoteMeta("if (!finished || *finished < record->writtenAt) return VpnAtStart::Unknown;"),
		regexp.QuoteMeta("return record->vpn;"))...)
	says := applyDefinition(marker, "inline bool SaysVpnDisconnected(std::optional<VpnAtStart> vpn) {")
	if !strings.Contains(says, "return vpn.has_value() && *vpn != VpnAtStart::NotConnected;") {
		problems = append(problems, "UpdateMarker.h's SaysVpnDisconnected does not decide by the record this launch took: "+
			"the line stays for every VPN but a recorded none, and goes when there is no record")
	}
	if !strings.Contains(header, "std::optional<update::VpnAtStart> vpn") {
		problems = append(problems, "UpdateChecker.h's Result cannot carry a report with no record: "+
			"a report shown again reads as one whose record was known, or unknown, and says the line")
	}
	if !strings.Contains(header, "vpn = update::VpnAtStart::Unknown;") {
		problems = append(problems, "UpdateChecker.h's Result does not start with the VPN unknown: a road that never says it would drop the line")
	}
	apply := applyDefinition(checker, "void UpdateChecker::RunApply(std::uint64_t generation, bool manual) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunApply", apply,
		regexp.QuoteMeta("const std::int64_t started = NowUnixSeconds();"),
		regexp.QuoteMeta("const update::VpnNote handOff = HandOffVpn();"),
		regexp.QuoteMeta("LaunchUpdateHelper(installFolder_ / kHelperName, offer.tag, &helper, &refusal,"),
		regexp.QuoteMeta("if (waited == update::HelperWait::AppExiting) {"),
		regexp.QuoteMeta("RecordUpdateInProgress(helper, NoteVpn());"),
		regexp.QuoteMeta("Result result{.version = offer.version, .exitCode = ended};"),
		regexp.QuoteMeta("result.vpn = NoteVpn();"),
		regexp.QuoteMeta("s.result = result;"))...)
	// handed off once, and read where it is used, not once at the hand-off (G10):
	// every call counts, a bare one too (the lead's G16)
	if calls := strings.Count(checker, "HandOffVpn(") - strings.Count(checker, "UpdateChecker::HandOffVpn("); calls != 1 {
		problems = append(problems, fmt.Sprintf("UpdateChecker.cpp calls HandOffVpn %d times, want once, as the update begins", calls))
	}
	if calls := strings.Count(checker, "NoteVpn())") + strings.Count(checker, "= NoteVpn();"); calls != 2 {
		problems = append(problems, fmt.Sprintf("UpdateChecker.cpp reads the VPN's note in %d places, want two: the record and the report", calls))
	}
	// the note as it is, so one not known is written as three numbers (G10)
	record := applyDefinition(glue, "void RecordUpdateInProgress(void* installerProcess, update::VpnAtStart vpn) {")
	problems = append(problems, applyOrderProblems("RecordUpdateInProgress", record,
		regexp.QuoteMeta(".writtenAt = NowSeconds(),"),
		regexp.QuoteMeta(".vpn = vpn};"),
		regexp.QuoteMeta("file << update::FormatUpdateMarker(marker);"))...)
	check := applyDefinition(glue, "bool UpdateInProgress() {")
	// every record it takes is kept for the report, whatever it says: the
	// statement whole, followed directly by the lock and the store (the lead's
	// G11). An older build's three-field record must keep the line (G8).
	keeps := regexp.QuoteMeta("if (const std::optional<update::UpdateMarker> ended = update::ParseUpdateMarker(*text)) {") + `\s*` +
		regexp.QuoteMeta("std::lock_guard lock(g_endedUpdateMutex);") + `\s*` + regexp.QuoteMeta("g_endedUpdate = ended;")
	if !regexp.MustCompile(keeps).MatchString(check) {
		problems = append(problems, "UpdateInProgress does not keep every record it takes, whatever it says: a record it drops leaves its report with no record, and an older build's would lose the line")
	}
	problems = append(problems, applyOrderProblems("UpdateInProgress", check,
		regexp.QuoteMeta("if (verdict == update::Verdict::Stale) {"),
		regexp.QuoteMeta("update::ParseUpdateMarker(*text)"),
		regexp.QuoteMeta("std::lock_guard lock(g_endedUpdateMutex);"),
		regexp.QuoteMeta("g_endedUpdate = ended;"),
		regexp.QuoteMeta("std::filesystem::remove(path, error);"))...)
	take := applyDefinition(glue, "std::optional<update::UpdateMarker> TakeEndedUpdate() {")
	problems = append(problems, applyOrderProblems("TakeEndedUpdate", take,
		regexp.QuoteMeta("std::lock_guard lock(g_endedUpdateMutex);"),
		regexp.QuoteMeta("return std::exchange(g_endedUpdate, std::nullopt);"))...)
	show := applyDefinition(checker, "void UpdateChecker::ShowLastResult() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::ShowLastResult", show,
		regexp.QuoteMeta("const std::optional<std::int64_t> finished = update::ParseUtcSecond(result->finishedUtc);"),
		regexp.QuoteMeta("const std::optional<update::UpdateMarker> began = TakeEndedUpdate();"),
		regexp.QuoteMeta("const std::optional<update::VpnAtStart> vpn = update::VpnOfReport(began, finished);"),
		regexp.QuoteMeta(".view = view};"),
		regexp.QuoteMeta("s.result.vpn = vpn;"))...)
	if count := strings.Count(checker, "TakeEndedUpdate()"); count != 1 {
		problems = append(problems, fmt.Sprintf("UpdateChecker.cpp takes the ended update's record %d times, want once, for the report it shows at launch", count))
	}
	// the line, named only where it is asked for
	result := applyDefinition(connect, "winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity ConnectPage::ApplyUpdateResult(")
	problems = append(problems, applyOrderProblems("ConnectPage::ApplyUpdateResult", result,
		regexp.QuoteMeta("const auto& result = snap.result;"),
		regexp.QuoteMeta("const bool reconnect = urnw::update::SaysVpnDisconnected(result.vpn);"),
		regexp.QuoteMeta("const std::wstring vpn = reconnect ? std::wstring{L\" \"} + kReconnect : std::wstring{};"),
		regexp.QuoteMeta("if (reconnect) message = kReconnect;"))...)
	var unasked []string
	for _, line := range strings.Split(result, "\n") {
		if !strings.Contains(line, "kReconnect") || strings.Contains(line, "constexpr wchar_t kReconnect[]") {
			continue
		}
		if !strings.Contains(line, "if (reconnect)") && !strings.Contains(line, "reconnect ?") {
			unasked = append(unasked, strings.TrimSpace(line))
		}
	}
	if len(unasked) > 0 {
		problems = append(problems, fmt.Sprintf("ConnectPage::ApplyUpdateResult names kReconnect without asking whether a VPN was connected: %q", unasked))
	}
	if count := strings.Count(result, "kReconnect"); count != 3 {
		problems = append(problems, fmt.Sprintf("ConnectPage::ApplyUpdateResult names kReconnect %d times, want three: its definition, the install's message, and the line after a sentence", count))
	}
	return problems
}

func TestUpdateApplyTrayRemembersTheVpn(t *testing.T) {
	reportProblems(t, checkTrayRemembersTheVpn(appControllerSource(t), stripComments(readAppSource(t, "UpdateChecker.cpp")),
		stripComments(readAppSource(t, "UpdateChecker.h")), stripComments(readAppSource(t, "SingleInstance.cpp")),
		stripComments(readAppSource(t, "ConnectPage.cpp")), stripComments(readCommonSource(t, "UpdateMarker.h"))))
}

// Each part of the note about the VPN, taken away, fails the check, and so do
// the line said whatever the VPN, each road back to leaving the line out when
// a record does not say, and each road back to saying it for a report shown
// again, with no record.
func TestUpdateApplyTrayRemembersTheVpnRejectsWeakerSources(t *testing.T) {
	controller := appControllerSource(t)
	checker := stripComments(readAppSource(t, "UpdateChecker.cpp"))
	header := stripComments(readAppSource(t, "UpdateChecker.h"))
	glue := stripComments(readAppSource(t, "SingleInstance.cpp"))
	connect := stripComments(readAppSource(t, "ConnectPage.cpp"))
	marker := stripComments(readCommonSource(t, "UpdateMarker.h"))
	replace := func(text, old, replacement string) string {
		if strings.Count(text, old) != 1 {
			t.Fatalf("negative control: %q is not in the source exactly once", old)
		}
		return strings.Replace(text, old, replacement, 1)
	}
	for _, tc := range []struct {
		name, want                                         string
		controller, checker, header, glue, connect, marker string
	}{
		{"the sources as they are", "", controller, checker, header, glue, connect, marker},
		{"the tunnel's state never reaches the checker", "AppController::OnTunnelState is missing updates_\\.SetVpnConnected",
			replace(controller, "  updates_.SetVpnConnected(isUp);\n", ""), checker, header, glue, connect, marker},
		{"the checker told the VPN is always up", "AppController::OnTunnelState is missing updates_\\.SetVpnConnected\\(isUp\\)",
			replace(controller, "  updates_.SetVpnConnected(isUp);\n", "  updates_.SetVpnConnected(true);\n"), checker, header, glue, connect, marker},
		{"read once at the hand-off, for the record", `UpdateChecker::RunApply is missing RecordUpdateInProgress\(helper, NoteVpn\(\)\);`,
			controller, replace(checker, "RecordUpdateInProgress(helper, NoteVpn());", "RecordUpdateInProgress(helper, handOff.Note());"),
			header, glue, connect, marker},
		{"read once at the hand-off, for the report", `UpdateChecker::RunApply is missing result\.vpn = NoteVpn\(\);`,
			controller, replace(checker, "  result.vpn = NoteVpn();\n", "  result.vpn = handOff.Note();\n"),
			header, glue, connect, marker},
		{"known only from pushes", "UpdateChecker::HandOffVpn is missing",
			controller, replace(checker, "const std::optional<bool> answered = vpnAnswer_ ? vpnAnswer_() : std::nullopt;",
				"const std::optional<bool> answered = std::nullopt;"), header, glue, connect, marker},
		{"the sign-in state used as the VPN's state", "AppController's probe of the service's answer is",
			replace(controller, "    const std::optional<proto::TunnelState> state = sdk_.AdoptedServiceState();\n"+
				"    if (!state) return std::nullopt;\n    return *state == proto::TunnelState::Up;\n",
				"    return sdk_.HasSession();\n"), checker, header, glue, connect, marker},
		{"the probe guarded by the sign-in state", "AppController's probe of the service's answer is",
			replace(controller, "    const std::optional<proto::TunnelState> state = sdk_.AdoptedServiceState();\n",
				"    if (!sdk_.HasSession()) return false;\n    const std::optional<proto::TunnelState> state = sdk_.AdoptedServiceState();\n"),
			checker, header, glue, connect, marker},
		{"a teardown clears the note in the checker", "UpdateChecker::SetVpnConnected is",
			controller, replace(checker, "  vpn_.StatusPushed(connected);\n",
				"  vpn_.StatusPushed(connected);\n  if (!connected) vpn_.upSinceHandOff = false;\n"),
			header, glue, connect, marker},
		{"a second hand-off as the app exits", "UpdateChecker.cpp calls HandOffVpn 2 times",
			controller, replace(checker, "    RecordUpdateInProgress(helper, NoteVpn());\n",
				"    HandOffVpn();\n    RecordUpdateInProgress(helper, NoteVpn());\n"),
			header, glue, connect, marker},
		{"a teardown clears the note", "UpdateMarker.h VpnNote is missing",
			controller, checker, header, glue, connect, replace(marker, "upSinceHandOff = upSinceHandOff || up;", "upSinceHandOff = up;")},
		{"the VPN never noted", `UpdateChecker::RunApply is missing const update::VpnNote handOff = HandOffVpn\(\);`,
			controller, replace(checker, "  const update::VpnNote handOff = HandOffVpn();\n", "  const update::VpnNote handOff;\n"),
			header, glue, connect, marker},
		{"the live report without the VPN", `UpdateChecker::RunApply is missing result\.vpn = NoteVpn\(\);`,
			controller, replace(checker, "  result.vpn = NoteVpn();\n", ""),
			header, glue, connect, marker},
		{"the record without the VPN", "RecordUpdateInProgress is missing",
			controller, checker, header, replace(glue, ".writtenAt = NowSeconds(),\n                                    .vpn = vpn};",
				".writtenAt = NowSeconds()};"), connect, marker},
		{"the writer reads a note not known as no VPN", "RecordUpdateInProgress is missing",
			controller, checker, header, replace(glue, ".vpn = vpn};",
				".vpn = vpn == update::VpnAtStart::Unknown ? update::VpnAtStart::NotConnected : vpn};"), connect, marker},
		{"an older build's record dropped as the launch takes it", "UpdateInProgress does not keep every record it takes",
			controller, checker, header, replace(glue, "if (const std::optional<update::UpdateMarker> ended = update::ParseUpdateMarker(*text)) {",
				"if (const std::optional<update::UpdateMarker> ended = update::ParseUpdateMarker(*text);\n        ended && ended->vpn != update::VpnAtStart::Unknown) {"), connect, marker},
		{"the ended update's record not kept", "UpdateInProgress is missing g_endedUpdate = ended;",
			controller, checker, header, replace(glue, "      g_endedUpdate = ended;\n", ""), connect, marker},
		{"the record taken twice", "TakeEndedUpdate is missing return std::exchange",
			controller, checker, header, replace(glue, "return std::exchange(g_endedUpdate, std::nullopt);", "return g_endedUpdate;"), connect, marker},
		{"the record's word taken for any report", `UpdateChecker::ShowLastResult is missing const std::optional<update::VpnAtStart> vpn = update::VpnOfReport\(began, finished\);`,
			controller, replace(checker, "const std::optional<update::VpnAtStart> vpn = update::VpnOfReport(began, finished);",
				"const std::optional<update::VpnAtStart> vpn = began ? std::optional<update::VpnAtStart>(began->vpn) : std::nullopt;"), header, glue, connect, marker},
		{"the report at launch without the VPN", `UpdateChecker::ShowLastResult is missing s\.result\.vpn = vpn;`,
			controller, replace(checker, "    s.result.vpn = vpn;\n", ""), header, glue, connect, marker},
		{"the line shown whatever the VPN", "ConnectPage::ApplyUpdateResult is missing const bool reconnect = urnw::update::SaysVpnDisconnected",
			controller, checker, header, glue, replace(connect, "  const bool reconnect = urnw::update::SaysVpnDisconnected(result.vpn);\n",
				"  const bool reconnect = true;\n"), marker},
		{"the line left out whenever a VPN is not known to have been connected", "ConnectPage::ApplyUpdateResult is missing const bool reconnect = urnw::update::SaysVpnDisconnected",
			controller, checker, header, glue, replace(connect, "  const bool reconnect = urnw::update::SaysVpnDisconnected(result.vpn);\n",
				"  const bool reconnect = result.vpn == urnw::update::VpnAtStart::Connected;\n"), marker},
		{"an older build's record read as no VPN", "an older build's three-field record reads as no VPN",
			controller, checker, header, glue, connect, replace(marker, "VpnAtStart vpn = VpnAtStart::Unknown;", "VpnAtStart vpn = VpnAtStart::NotConnected;")},
		{"the line left out when the VPN is not known", "SaysVpnDisconnected does not decide by the record",
			controller, checker, header, glue, connect, replace(marker, "return vpn.has_value() && *vpn != VpnAtStart::NotConnected;", "return vpn == VpnAtStart::Connected;")},
		{"a report that starts as no VPN", "UpdateChecker.h's Result does not start with the VPN unknown",
			controller, checker, replace(header, "std::optional<update::VpnAtStart> vpn = update::VpnAtStart::Unknown;", "std::optional<update::VpnAtStart> vpn = update::VpnAtStart::NotConnected;"),
			glue, connect, marker},
		{"a report that cannot say it has no record", "UpdateChecker.h's Result cannot carry a report with no record",
			controller, checker, replace(header, "std::optional<update::VpnAtStart> vpn = update::VpnAtStart::Unknown;", "update::VpnAtStart vpn = update::VpnAtStart::Unknown;"),
			glue, connect, marker},
		{"a report shown again says the VPN was disconnected", "UpdateMarker.h's VpnOfReport gives a report this launch took no record for a VPN",
			controller, checker, header, glue, connect, replace(marker, "if (!record) return std::nullopt;", "if (!record) return VpnAtStart::Unknown;")},
		{"no record keeps the line", "SaysVpnDisconnected does not decide by the record",
			controller, checker, header, glue, connect, replace(marker, "return vpn.has_value() && *vpn != VpnAtStart::NotConnected;",
				"return !vpn.has_value() || *vpn != VpnAtStart::NotConnected;")},
		{"another update's record taken for the report's", `UpdateMarker.h VpnOfReport is missing if \(!finished`,
			controller, checker, header, glue, connect, replace(marker, "if (!finished || *finished < record->writtenAt) return VpnAtStart::Unknown;", "(void)finished;")},
		{"an install's message with the line unasked", "names kReconnect without asking whether a VPN was connected",
			controller, checker, header, glue, replace(connect, "      if (reconnect) message = kReconnect;\n", "      message = kReconnect;\n"), marker},
		{"a restart's message with the line unasked", "names kReconnect without asking whether a VPN was connected",
			controller, checker, header, glue, replace(connect, `current + L". Quit URnetwork from its tray icon and start it again." + vpn;`,
				`current + L". Quit URnetwork from its tray icon and start it again. " + kReconnect;`), marker},
	} {
		t.Run(tc.name, func(t *testing.T) {
			problems := strings.Join(checkTrayRemembersTheVpn(tc.controller, tc.checker, tc.header, tc.glue, tc.connect, tc.marker), "\n")
			if tc.want == "" {
				if problems != "" {
					t.Fatalf("a source that is no weaker was reported:\n%s", problems)
				}
				return
			}
			if !strings.Contains(problems, tc.want) && !regexp.MustCompile(tc.want).MatchString(problems) {
				t.Fatalf("negative control was not detected by %q; the check reported:\n%s", tc.want, problems)
			}
			t.Logf("fires: %s", tc.name)
		})
	}
}

// What the service tells this process of the tunnel, by a push or by an
// answer, is what an update's note of the VPN starts from when no status was
// pushed (the lead's G10a): SdkHost marks the state known as it adopts one
// (AdoptServiceFacts), and hands it on only then (AdoptedServiceState).
func checkTheServiceAnswersTheVpn(sdkHeader, sdkSource string) []string {
	var problems []string
	adopt := applyDefinition(sdkSource, "void SdkHost::AdoptServiceFacts(const proto::TunnelStatus& st) {")
	problems = append(problems, applyOrderProblems("SdkHost::AdoptServiceFacts", adopt,
		regexp.QuoteMeta("lastServiceState_.store(st.state);"),
		regexp.QuoteMeta("serviceStateKnown_.store(true, std::memory_order_release);"))...)
	if count := strings.Count(sdkSource, "serviceStateKnown_.store("); count != 1 {
		problems = append(problems, fmt.Sprintf("SdkHost.cpp marks the service's state known in %d places, want one, as it adopts a status", count))
	}
	answered := applyDefinitionUntil(sdkHeader, "std::optional<proto::TunnelState> AdoptedServiceState() const {", "\n  }\n")
	problems = append(problems, applyOrderProblems("SdkHost::AdoptedServiceState", answered,
		regexp.QuoteMeta("if (!serviceStateKnown_.load(std::memory_order_acquire)) return std::nullopt;"),
		regexp.QuoteMeta("return lastServiceState_.load();"))...)
	return problems
}

func TestUpdateApplyTheServiceAnswersTheVpn(t *testing.T) {
	reportProblems(t, checkTheServiceAnswersTheVpn(stripComments(readAppSource(t, "SdkHost.h")),
		stripComments(readAppSource(t, "SdkHost.cpp"))))
}

// An answer never marked known, none handed on, or a state handed on before
// the service said any, fails the check.
func TestUpdateApplyTheServiceAnswersTheVpnRejectsWeakerSources(t *testing.T) {
	sdkHeader := stripComments(readAppSource(t, "SdkHost.h"))
	sdkSource := stripComments(readAppSource(t, "SdkHost.cpp"))
	replace := func(text, old, replacement string) string {
		if strings.Count(text, old) != 1 {
			t.Fatalf("negative control: %q is not in the source exactly once", old)
		}
		return strings.Replace(text, old, replacement, 1)
	}
	for _, tc := range []struct {
		name, want, sdkHeader, sdkSource string
	}{
		{"the sources as they are", "", sdkHeader, sdkSource},
		{"an answer never marked known: known only from pushes", "SdkHost::AdoptServiceFacts is missing",
			sdkHeader, replace(sdkSource, "  serviceStateKnown_.store(true, std::memory_order_release);\n", "")},
		{"no answer handed on", "SdkHost::AdoptedServiceState is missing",
			replace(sdkHeader, "return lastServiceState_.load();", "return std::nullopt;"), sdkSource},
		{"a state handed on before the service said any", "SdkHost::AdoptedServiceState is missing",
			replace(sdkHeader, "    if (!serviceStateKnown_.load(std::memory_order_acquire)) return std::nullopt;\n", ""), sdkSource},
	} {
		t.Run(tc.name, func(t *testing.T) {
			problems := strings.Join(checkTheServiceAnswersTheVpn(tc.sdkHeader, tc.sdkSource), "\n")
			if tc.want == "" {
				if problems != "" {
					t.Fatalf("a source that is no weaker was reported:\n%s", problems)
				}
				return
			}
			if !strings.Contains(problems, tc.want) {
				t.Fatalf("negative control was not detected by %q; the check reported:\n%s", tc.want, problems)
			}
			t.Logf("fires: %s", tc.name)
		})
	}
}

// The relaunch after an update is the one launch the update marker must not
// turn away: it carries --after-update and waits, bounded, for the update to
// end before it asks like every launch. The wait is InstanceHandover.h's
// AwaitUpdateEnd (instance_handover_test.go runs it against a fake clock),
// bound here to the marker, the steady clock and Sleep.
func checkRelaunchWaits(main, glue, handover string) []string {
	var problems []string
	for _, want := range []string{
		`inline constexpr std::wstring_view kAfterUpdateArgument = L"--after-update";`,
		`inline constexpr std::chrono::milliseconds kAfterUpdateBudget{120000};`,
		`inline constexpr std::chrono::milliseconds kAfterUpdatePoll{500};`,
	} {
		if !strings.Contains(handover, want) {
			problems = append(problems, "InstanceHandover.h no longer has "+want)
		}
	}
	wait := applyDefinition(handover, "bool AwaitUpdateEnd(Updating&& updating, Now&& now, Sleep&& sleep,")
	problems = append(problems, applyOrderProblems("instance::AwaitUpdateEnd", wait,
		regexp.QuoteMeta("const std::chrono::milliseconds deadline = now() + budget;"),
		regexp.QuoteMeta("while (updating()) {"),
		regexp.QuoteMeta("if (now() >= deadline) return false;"),
		regexp.QuoteMeta("sleep(kAfterUpdatePoll);"),
		regexp.QuoteMeta("return true;"))...)
	entry := applyDefinition(main, "int __stdcall wWinMain(")
	problems = append(problems, applyOrderProblems("wWinMain", entry,
		regexp.QuoteMeta("if (urnw::LaunchedAfterUpdate()) urnw::AwaitUpdateEnd();"),
		regexp.QuoteMeta("urnw::CreateExitingSignal();"),
		regexp.QuoteMeta("urnw::instance::Launch(launcher);"))...)
	problems = append(problems, applyOrderProblems("LaunchedAfterUpdate",
		applyDefinition(glue, "bool LaunchedAfterUpdate() {"),
		regexp.QuoteMeta("return instance::HasArgument(::GetCommandLineW(), instance::kAfterUpdateArgument);"))...)
	problems = append(problems, applyOrderProblems("AwaitUpdateEnd",
		applyDefinition(glue, "void AwaitUpdateEnd() {"),
		regexp.QuoteMeta("const bool ended = instance::AwaitUpdateEnd("),
		regexp.QuoteMeta("[] { return UpdateInProgress(); },"),
		regexp.QuoteMeta("std::chrono::steady_clock::now().time_since_epoch());"),
		regexp.QuoteMeta("[](std::chrono::milliseconds pause) { ::Sleep(static_cast<DWORD>(pause.count())); });"))...)
	return problems
}

func TestUpdateApplyTheRelaunchWaitsForTheUpdate(t *testing.T) {
	reportProblems(t, checkRelaunchWaits(appMainSource(t), stripComments(readAppSource(t, "SingleInstance.cpp")),
		stripComments(readCommonSource(t, "InstanceHandover.h"))))
}

// Each tray check fails on a source that drops what it pins.
func TestUpdateApplyWiringRejectsWeakerTrays(t *testing.T) {
	checker := stripComments(readAppSource(t, "UpdateChecker.cpp"))
	window := stripComments(readAppSource(t, "MainWindow.xaml.cpp"))
	connect := stripComments(readAppSource(t, "ConnectPage.cpp"))
	main := appMainSource(t)
	glue := stripComments(readAppSource(t, "SingleInstance.cpp"))
	handover := stripComments(readCommonSource(t, "InstanceHandover.h"))
	replace := func(text, old, replacement string) string {
		if strings.Count(text, old) != 1 {
			t.Fatalf("negative control: %q is not in the source exactly once", old)
		}
		return strings.Replace(text, old, replacement, 1)
	}
	runs := func(old, replacement string) []string {
		return checkTrayRunsTheHelper(replace(checker, old, replacement))
	}
	reads := func(old, replacement string) []string {
		return checkTrayReadsTheReport(replace(checker, old, replacement))
	}
	shows := func(old, replacement string) []string {
		return checkTrayShowsTheInstaller(replace(checker, old, replacement), window, connect)
	}
	says := func(old, replacement string) []string {
		return checkTraySaysWhatTheUpdateDoes(replace(connect, old, replacement))
	}
	for _, tc := range []struct {
		name  string
		check func() []string
	}{
		{"a portable copy runs the helper", func() []string {
			return runs("const bool viaHelper = installed_ && !manual;", "const bool viaHelper = !manual;")
		}},
		{"the helper started from the installer branch", func() []string {
			return runs("    RevealInExplorer(msiW);\n    return;\n",
				"    LaunchUpdateHelper(installFolder_ / kHelperName, offer.tag, &helper, &refusal, launchError);\n    return;\n")
		}},
		{"any copy treated as installed", func() []string {
			return runs("installed_ = !exe.empty() && install::AdminOnlyLocation(installFolder_ / kHelperName, why);",
				"installed_ = !exe.empty();")
		}},
		{"the tray builds msiexec's arguments", func() []string {
			return runs(`const std::wstring params = L"--apply-update " + tag;`,
				`const std::wstring params = L"/i msiexec --apply-update " + tag;`)
		}},
		{"a wait the app's teardown cannot end", func() []string {
			return runs("  const auto cancelled = [this] {\n    std::lock_guard lock(mutex_);\n    return stop_;\n  };",
				"  const auto cancelled = [this] {\n    std::lock_guard lock(mutex_);\n    return false;\n  };")
		}},
		{"a wait that never looks at the app's exit", func() []string {
			apply := applyDefinition(checker, "void UpdateChecker::RunApply(std::uint64_t generation, bool manual) {")
			return checkTrayRunsTheHelper(replace(checker, apply, replace(apply,
				"[helper] { return ::WaitForSingleObject(helper, 250) == WAIT_OBJECT_0; }, cancelled);",
				"[helper] { return ::WaitForSingleObject(helper, 250) == WAIT_OBJECT_0; }, [] { return false; });")))
		}},
		{"the marker written as the helper starts", func() []string {
			launch := applyDefinition(checker, "bool LaunchUpdateHelper(fs::path const& helperPath, std::wstring const& tag, HANDLE* helper,")
			return checkTrayRunsTheHelper(replace(checker, launch, replace(launch,
				"    *helper = sei.hProcess;\n", "    RecordUpdateInProgress(sei.hProcess);\n    *helper = sei.hProcess;\n")))
		}},
		{"no marker when the app exits for the installer", func() []string {
			return runs("    RecordUpdateInProgress(helper, NoteVpn());\n", "")
		}},
		{"an unsigned helper taken for a declined prompt", func() []string {
			return runs("fail(refusal == kElevationRefusedUnsigned && update::HandsOverInstaller(*offer.feed)\n             ? Failure::Unsigned\n             : Failure::Elevation);",
				"fail(Failure::Elevation);")
		}},
		{"a report read on a portable copy", func() []string {
			return reads("void UpdateChecker::ShowLastResult() {\n  if (!installed_) return;\n",
				"void UpdateChecker::ShowLastResult() {\n")
		}},
		{"a report dropped by the next check", func() []string {
			return reads("if (snapshot_.phase != Phase::None && snapshot_.phase != Phase::Result) {",
				"if (snapshot_.phase != Phase::None) {")
		}},
		{"a report shown whatever build reads it", func() []string {
			return reads("if (view == update::ReportView::Hidden) {", "if (false) {")
		}},
		{"a restart asked for after Windows restarted", func() []string {
			return reads("const bool restartedSince = finished && BootUnixSeconds() > *finished;",
				"const bool restartedSince = false;")
		}},
		{"an earlier attempt's report taken as this one's", func() []string {
			return reads("if (report && update::IsReportOfRun(*report, outcome.tag, ended, started)) {",
				"if (report && report->tag == outcome.tag) {")
		}},
		{"a release that did not install with no way out", func() []string {
			return shows("snapshot.offeredCode == snapshot.code && update::HandsOverInstaller(*snapshot.feed);", "false;")
		}},
		{"the checked download deleted as the helper starts", func() []string {
			return shows("  if (!helper) return;\n", "  if (!helper) return;\n  fs::remove_all(dir, ec);\n")
		}},
		{"a release no longer offered left on the banner", func() []string {
			return shows("if (ended == static_cast<std::int64_t>(update::Refusal::NotOffered)) {", "if (false) {")
		}},
		{"the shown installer not checked again", func() []string {
			return shows("if (digest.empty() || actual.empty() || !update::EqualsAsciiCaseless(actual, digest)) {",
				"if (false) {")
		}},
		{"Show file without a second check", func() []string {
			return checkTrayShowsTheInstaller(checker, replace(window, "      urnw::pages::Updates().RevealInstaller();\n",
				"      urnw::UpdateChecker::RevealInExplorer(updateSnapshot_.installerPath);\n"), connect)
		}},
		{"a report banner that cannot be closed", func() []string {
			return checkTrayShowsTheInstaller(checker, replace(window, "UpdateBar().Closed(", "UpdateBar().Opening("), connect)
		}},
		{"an unsigned helper's way out leading to the helper again", func() []string {
			return checkTrayShowsTheInstaller(checker, replace(window,
				"if (updateSnapshot_.failure == Failure::Unsigned && updateSnapshot_.phase == Phase::Failed)", "if (false)"), connect)
		}},
		{"the VPN's disconnect not said", func() []string {
			return says(`message += L" The VPN disconnects while it installs; connect again once URnetwork is back.";`, "")
		}},
		{"an install shown without the VPN to reconnect", func() []string {
			return says("if (reconnect) message = kReconnect;", "if (reconnect) message.clear();")
		}},
		{"the helper's run shown as starting the installer", func() []string {
			return says("case Stage::Helper:", "case Stage::Idle:")
		}},
		{"the relaunch refused by its own update", func() []string {
			return checkRelaunchWaits(replace(main, "if (urnw::LaunchedAfterUpdate()) urnw::AwaitUpdateEnd();", ""),
				glue, handover)
		}},
		{"the relaunch's own wait instead of the tested one", func() []string {
			return checkRelaunchWaits(main, replace(glue, "const bool ended = instance::AwaitUpdateEnd(",
				"const bool ended = AwaitTheUpdateSomehow("), handover)
		}},
		{"a relaunch that waits ten seconds", func() []string {
			return checkRelaunchWaits(main, glue, replace(handover, "kAfterUpdateBudget{120000};", "kAfterUpdateBudget{10000};"))
		}},
		{"a relaunch that waits one poll", func() []string {
			return checkRelaunchWaits(main, glue, replace(handover, "  while (updating()) {\n", "  if (updating()) {\n"))
		}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if problems := tc.check(); len(problems) == 0 {
				t.Fatal("negative control was not detected")
			}
		})
	}
}

// The worker follows the feed it was started for. A channel change bumps the
// generation and drops the offer, every change a check or an apply makes to
// the snapshot is for its own generation, a check of the old feed publishes
// nothing, and an apply of it stops at the latest at the hand-off to the
// helper. The helper's report is not the feed's to drop: once the helper ran,
// what it did is shown whatever the feed is now.
func checkTrayFollowsTheFeed(checker string) []string {
	var problems []string
	unscoped := regexp.MustCompile(`\bMutate\(`)
	changed := applyDefinition(checker, "void UpdateChecker::ChannelChanged() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::ChannelChanged", changed,
		regexp.QuoteMeta("std::lock_guard lock(mutex_);"),
		regexp.QuoteMeta("++feedGeneration_;"),
		regexp.QuoteMeta("offer_ = Offer{};"),
		regexp.QuoteMeta("if (snapshot_.phase != Phase::Result) {"),
		regexp.QuoteMeta("snapshot_.offeredCode = 0;"),
		regexp.QuoteMeta("checkRequested_ = true;"),
		regexp.QuoteMeta("cv_.notify_all();"))...)
	scoped := applyDefinition(checker, "bool UpdateChecker::MutateFor(std::uint64_t generation,")
	problems = append(problems, applyOrderProblems("UpdateChecker::MutateFor", scoped,
		regexp.QuoteMeta("std::lock_guard lock(mutex_);"),
		regexp.QuoteMeta("if (feedGeneration_ != generation) return false;"),
		regexp.QuoteMeta("fn(snapshot_);"))...)
	worker := applyDefinition(checker, "void UpdateChecker::WorkerLoop() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::WorkerLoop", worker,
		regexp.QuoteMeta("if (applyRequested_ || manualRequested_) {"),
		regexp.QuoteMeta("const std::uint64_t generation = feedGeneration_;"),
		regexp.QuoteMeta("lock.unlock();"),
		regexp.QuoteMeta("RunApply(generation, manual);"),
		regexp.QuoteMeta("if (revealRequested_) {"),
		regexp.QuoteMeta("const std::uint64_t generation = feedGeneration_;"),
		regexp.QuoteMeta("lock.unlock();"),
		regexp.QuoteMeta("RunReveal(generation);"),
		regexp.QuoteMeta("const std::uint64_t generation = feedGeneration_;"),
		regexp.QuoteMeta("lock.unlock();"),
		regexp.QuoteMeta("RunCheck(generation);"))...)
	if unscoped.MatchString(worker) {
		problems = append(problems, "UpdateChecker::WorkerLoop changes the snapshot without a generation")
	}
	check := applyDefinition(checker, "bool UpdateChecker::RunCheck(std::uint64_t generation) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunCheck", check,
		regexp.QuoteMeta("if (feedGeneration_ != generation) {"),
		regexp.QuoteMeta("return false;"),
		regexp.QuoteMeta("snapshot_.newestCode = newestCode;"),
		regexp.QuoteMeta("offer_ = offer;"),
		regexp.QuoteMeta("snapshot_.offeredCode = offer_.code;"))...)
	if unscoped.MatchString(check) {
		problems = append(problems, "UpdateChecker::RunCheck changes the snapshot without its generation")
	}
	failed := applyDefinition(checker, "void UpdateChecker::CheckFailed(std::uint64_t generation) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::CheckFailed", failed,
		regexp.QuoteMeta("MutateFor(generation,"))...)
	apply := applyDefinition(checker, "void UpdateChecker::RunApply(std::uint64_t generation, bool manual) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunApply", apply,
		regexp.QuoteMeta("if (!actionable || offer_.code == 0 || feedGeneration_ != generation) return;"),
		regexp.QuoteMeta("if (!MutateFor(generation, [](Snapshot& s) { s.stage = Stage::Installing; })) {"),
		regexp.QuoteMeta("abandoned(dir);"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta("LaunchUpdateHelper(installFolder_ / kHelperName, offer.tag, &helper, &refusal,"),
		regexp.QuoteMeta("Mutate([this, &result](Snapshot& s) {"))...)
	if handOff := strings.Index(apply, "LaunchUpdateHelper("); handOff >= 0 && unscoped.MatchString(apply[:handOff]) {
		problems = append(problems,
			"UpdateChecker::RunApply changes the snapshot without its generation before the hand-off to the helper")
	}
	reveal := applyDefinition(checker, "void UpdateChecker::RunReveal(std::uint64_t generation) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunReveal", reveal,
		regexp.QuoteMeta("feedGeneration_ != generation"),
		regexp.QuoteMeta("MutateFor(generation, [](Snapshot& s) {"))...)
	if unscoped.MatchString(reveal) {
		problems = append(problems, "UpdateChecker::RunReveal changes the snapshot without its generation")
	}
	return problems
}

// The tray app follows the machine's update channel. Start reads the marker
// before the worker exists, on an installed copy with a code alone; SyncChannel
// reads it again on the worker, and nothing else, a feed other than the one
// followed, by id, being a change of channel (ChannelChanged), taken with the
// generation; every check and every apply reads it first, and a check takes
// its feed under the lock that judges its generation; the Developer and the
// Settings screens ask for a read as they are shown. RunCheck, RunApply and
// RunReveal name no feed: an offer carries its own, and is asked about it. A
// developer build's installer is never handed over to run, and an unsigned
// helper offers no installer for one. A change of channel is the helper's:
// this app starts it elevated and shown, from an installed copy with a code
// alone, with an id of the table, records no update and hands off no VPN, and
// then says what the marker says. Both snapshots built from scratch keep the
// channel.
func checkTrayFollowsTheChannel(checker, window string, appSources map[string]string) []string {
	var problems []string
	problems = append(problems, applyOrderProblems("UpdateChecker::Start", applyDefinition(checker, "void UpdateChecker::Start() {"),
		regexp.QuoteMeta("installed_ = !exe.empty() && install::AdminOnlyLocation(installFolder_ / kHelperName, why);"),
		regexp.QuoteMeta("if (installed_ && version::kCode != 0) {"),
		regexp.QuoteMeta("install::ReadChannelMarker(installFolder_, channelWhy, &markerPresent);"),
		regexp.QuoteMeta("feed_ = &update::FeedOfMarker(marker);"),
		regexp.QuoteMeta("snapshot_.feed = feed_;"),
		regexp.QuoteMeta("worker_ = std::thread("))...)
	sync := applyDefinition(checker, "void UpdateChecker::SyncChannel() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::SyncChannel", sync,
		regexp.QuoteMeta("if (!installed_ || version::kCode == 0) return;"),
		regexp.QuoteMeta("install::ReadChannelMarker(installFolder_, why, &markerPresent);"),
		regexp.QuoteMeta("const update::Feed& feed = update::FeedOfMarker(marker);"),
		regexp.QuoteMeta("std::lock_guard lock(mutex_);"),
		regexp.QuoteMeta("changed = feed.id != feed_->id;"),
		regexp.QuoteMeta("if (changed) feed_ = &feed;"),
		regexp.QuoteMeta("ChannelChanged();"))...)
	change := applyDefinition(checker, "void UpdateChecker::RunChannelChange(const update::Feed& asked) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunChannelChange", change,
		regexp.QuoteMeta("if (!installed_ || version::kCode == 0) {"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta("LaunchChannelHelper(installFolder_ / kHelperName, asked, &helper, &refusal, launchError);"),
		regexp.QuoteMeta("update::AwaitHelper("),
		regexp.QuoteMeta("if (waited == update::HelperWait::AppExiting) {"),
		regexp.QuoteMeta(".markerAfter = install::ReadChannelMarker(installFolder_, why),"),
		regexp.QuoteMeta("const update::ChangeLine line = update::ChangeLineOf(change);"),
		regexp.QuoteMeta("SyncChannel();"),
		regexp.QuoteMeta("s.change = line;"))...)
	for where, body := range map[string]string{"UpdateChecker::SyncChannel": sync, "UpdateChecker::RunChannelChange": change} {
		for _, forbidden := range []string{"LoadAppPrefs", "SaveAppPref", "app_prefs", "HandOffVpn", "NoteVpn",
			"RecordUpdateInProgress", "LaunchUpdateHelper", "--apply-update"} {
			if strings.Contains(body, forbidden) {
				problems = append(problems, where+" names "+forbidden+
					": the channel is read from the marker alone, and a change of channel installs nothing")
			}
		}
	}
	launch := applyDefinition(checker, "bool LaunchChannelHelper(fs::path const& helperPath, update::Feed const& feed, HANDLE* helper,")
	problems = append(problems, applyOrderProblems("LaunchChannelHelper", launch,
		regexp.QuoteMeta(`const std::wstring params = L"--set-update-channel " + Widen(feed.id);`),
		regexp.QuoteMeta("sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS;"),
		regexp.QuoteMeta(`sei.lpVerb = L"runas";`),
		regexp.QuoteMeta("sei.lpFile = helperPath.c_str();"),
		regexp.QuoteMeta("sei.lpParameters = params.c_str();"),
		regexp.QuoteMeta("sei.nShow = SW_SHOWNORMAL;"),
		regexp.QuoteMeta("*refusal = ::GetLastError();"),
		regexp.QuoteMeta("*helper = sei.hProcess;"))...)
	check := applyDefinition(checker, "bool UpdateChecker::RunCheck(std::uint64_t generation) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunCheck", check,
		regexp.QuoteMeta("SyncChannel();"),
		regexp.QuoteMeta("std::lock_guard lock(mutex_);"),
		regexp.QuoteMeta("if (feedGeneration_ != generation) return false;"),
		regexp.QuoteMeta("followed = feed_;"),
		regexp.QuoteMeta("const update::Feed& feed = *followed;"),
		regexp.QuoteMeta("sel.assetName, &feed};"))...)
	apply := applyDefinition(checker, "void UpdateChecker::RunApply(std::uint64_t generation, bool manual) {")
	if !strings.HasPrefix(strings.TrimSpace(strings.TrimPrefix(apply, "void UpdateChecker::RunApply(std::uint64_t generation, bool manual) {")), "SyncChannel();") {
		problems = append(problems, "UpdateChecker::RunApply does not read the update channel first")
	}
	reveal := applyDefinition(checker, "void UpdateChecker::RunReveal(std::uint64_t generation) {")
	for where, body := range map[string]string{"UpdateChecker::RunCheck": check, "UpdateChecker::RunApply": apply,
		"UpdateChecker::RunReveal": reveal} {
		for _, feed := range []string{"kOfficialFeed", "FeedById(", "kDeveloperChannelId", "FeedOfMarker("} {
			if strings.Contains(body, feed) {
				problems = append(problems, where+" names "+feed+": it acts on the feed it follows, and an offer on its own")
			}
		}
	}
	// a developer build is installed through the helper alone
	for where, pattern := range map[string]string{
		"UpdateChecker::OffersInstaller": `snapshot\.offeredCode == snapshot\.code && update::HandsOverInstaller\(\*snapshot\.feed\);`,
		"UpdateChecker::RunApply":        `if \(!viaHelper\) \{\s+if \(!update::HandsOverInstaller\(\*offer\.feed\)\) \{`,
		"UpdateChecker::RunReveal":       `offer_\.code != snapshot_\.code \|\|\s+!update::HandsOverInstaller\(\*offer_\.feed\)\) \{`,
	} {
		if !regexp.MustCompile(pattern).MatchString(map[string]string{
			"UpdateChecker::OffersInstaller": applyDefinition(checker, "bool UpdateChecker::OffersInstaller(Snapshot const& snapshot) {"),
			"UpdateChecker::RunApply":        apply, "UpdateChecker::RunReveal": reveal}[where]) {
			problems = append(problems, where+" may hand a developer build over as an installer to run: only an official release is")
		}
	}
	// the channel requested, refused at once during this app's own update
	problems = append(problems, applyOrderProblems("UpdateChecker::RequestChannel",
		applyDefinition(checker, "void UpdateChecker::RequestChannel(const update::Feed& feed) {"),
		regexp.QuoteMeta("std::lock_guard lock(mutex_);"),
		regexp.QuoteMeta("const bool ownUpdate = snapshot_.phase == Phase::Applying || applyRequested_ || manualRequested_;"),
		regexp.QuoteMeta(".ownUpdateRunning = true};"),
		regexp.QuoteMeta("snapshot_.change = update::ChangeLineOf(refused);"),
		regexp.QuoteMeta("channelRequested_ = &feed;"),
		regexp.QuoteMeta("snapshot_.change = update::ChangeLine::Asking;"))...)
	worker := applyDefinition(checker, "void UpdateChecker::WorkerLoop() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::WorkerLoop", worker,
		regexp.QuoteMeta("if (channelRequested_) {"),
		regexp.QuoteMeta("RunChannelChange(*asked);"),
		regexp.QuoteMeta("if (syncRequested_) {"),
		regexp.QuoteMeta("SyncChannel();"))...)
	// both snapshots built from scratch keep the channel, and every snapshot
	// takes the feed when it changes
	rebuilds := regexp.MustCompile(`snapshot_ = Snapshot\{[^}]*\};`).FindAllString(checker, -1)
	if len(rebuilds) != 2 {
		problems = append(problems, fmt.Sprintf("UpdateChecker.cpp builds a snapshot from scratch %d times, want the two that keep the channel", len(rebuilds)))
	}
	for _, rebuild := range rebuilds {
		if !strings.Contains(rebuild, ".feed = feed_,") || !strings.Contains(rebuild, ".marker = snapshot_.marker,") ||
			!strings.Contains(rebuild, ".markerPresent = snapshot_.markerPresent") {
			problems = append(problems, "a snapshot built from scratch drops the update channel: "+rebuild)
		}
	}
	problems = append(problems, applyOrderProblems("UpdateChecker::ChannelChanged", applyDefinition(checker, "void UpdateChecker::ChannelChanged() {"),
		regexp.QuoteMeta("if (snapshot_.phase != Phase::Result) {"),
		regexp.QuoteMeta("snapshot_.feed = feed_;"),
		regexp.QuoteMeta("snapshot_.change = update::ChangeLine::None;"))...)
	// S5 names the row the marker selects (the A2c review's C11): the snapshot's
	// feed follows the marker a moment later, SyncChannel writing the marker and
	// ChannelChanged the feed, and a screen drawn between the two would name the
	// official repository on the developer channel
	if sentence := applyDefinition(checker, "std::wstring UpdateChecker::ChannelSentenceText("); !strings.Contains(sentence,
		`return Format("upd_channel_developer", RepositoryOf(update::FeedOfMarker(snapshot.marker)));`) ||
		strings.Contains(sentence, "snapshot.feed") {
		problems = append(problems, "UpdateChecker::ChannelSentenceText does not name the row the marker selects in S5")
	}
	// the screens that show the channel ask for it to be read
	if !strings.Contains(window, `if (tag == L"developer" || tag == L"settings") urnw::pages::Updates().RequestChannelSync();`) {
		problems = append(problems, "MainWindow does not ask for the update channel as the Developer or the Settings screen is shown")
	}
	// the developer row is reached by its id alone
	for name, source := range appSources {
		for _, named := range []string{"kDeveloperFeed", "Ryanmello07", "1324127805"} {
			if strings.Contains(source, named) {
				problems = append(problems, "App/"+name+" names "+named+": the app reaches the developer row through FeedById alone")
			}
		}
		// the tray reads the marker through the one reader and never reaches its
		// path: the helper alone makes or removes it (SPEC-A2c 8.3)
		if strings.Contains(source, "ChannelMarkerPath(") {
			problems = append(problems, "App/"+name+" names ChannelMarkerPath(: the tray reads the marker through ReadChannelMarker "+
				"alone, and only the elevated helper makes or removes it")
		}
	}
	// What every surface draws is the marker as read (the lead's G17):
	// ChannelLinesOf's whole body, its whitespace folded, and the snapshot's
	// marker, and whether something lies under its name, written where they
	// are read, in Start and in SyncChannel, and nowhere else. A write
	// elsewhere would show what was asked.
	if body := foldedBody(checker, "update::ChannelLines UpdateChecker::ChannelLinesOf(Snapshot const& snapshot) {"); body != "return update::LinesOf( {.installed = snapshot.installed, .codeZero = version::kCode == 0, .developerBuild = update::IsBetaVersion(version::kString), .marker = snapshot.marker, .markerPresent = snapshot.markerPresent, .developerRow = update::FeedById(update::kDeveloperChannelId) != nullptr, .change = snapshot.change, .foundNothingNewer = snapshot.lastCheck == CheckOutcome::NoUpdate, .releaseCounts = snapshot.newestCode != 0, .releaseWaiting = NamesWaiting(snapshot)});" {
		problems = append(problems, fmt.Sprintf("UpdateChecker::ChannelLinesOf is %q, want LinesOf's view of the snapshot, its marker as read", body))
	}
	start := applyDefinition(checker, "void UpdateChecker::Start() {")
	for _, write := range regexp.MustCompile(`(\w*)(?:\.|->)(marker(?:Present)?)\s*=([^=][^;,}]*)`).FindAllStringSubmatch(checker, -1) {
		switch to, field, from := write[1], write[2], strings.TrimSpace(write[3]); {
		case to == "snapshot_" && from == field:
			// as read, in Start and SyncChannel (counted below)
		case to == "" && from == "snapshot_."+field:
			// a snapshot rebuilt from scratch keeps the one it had
		case to == "" && from == "snapshot."+field:
			// ChannelLinesOf's view, pinned whole above
		default:
			problems = append(problems, fmt.Sprintf("UpdateChecker.cpp sets a marker as %q: the snapshot's marker is written where Start "+
				"and SyncChannel read it and copied as it is, so a screen shows the marker as read, never what was asked", write[0]))
		}
	}
	for _, write := range []string{"snapshot_.marker = marker;", "snapshot_.markerPresent = markerPresent;"} {
		if strings.Count(checker, write) != 2 || strings.Count(start, write) != 1 || strings.Count(sync, write) != 1 {
			problems = append(problems, "UpdateChecker.cpp does not write "+write+" exactly where Start and SyncChannel read the marker")
		}
	}
	return problems
}

func TestUpdateApplyTrayFollowsTheChannel(t *testing.T) {
	appSources := map[string]string{}
	for name, source := range appSourceFiles(t, ".cpp", ".h") {
		appSources[name] = stripComments(source)
	}
	reportProblems(t, checkTrayFollowsTheChannel(stripComments(readAppSource(t, "UpdateChecker.cpp")),
		stripComments(readAppSource(t, "MainWindow.xaml.cpp")), appSources))
}

// Each part of the tray's channel, taken away, is reported for its reason.
func TestUpdateApplyTrayFollowsTheChannelRejectsWeakerSources(t *testing.T) {
	checker := stripComments(readAppSource(t, "UpdateChecker.cpp"))
	window := stripComments(readAppSource(t, "MainWindow.xaml.cpp"))
	appSources := map[string]string{}
	for name, source := range appSourceFiles(t, ".cpp", ".h") {
		appSources[name] = stripComments(source)
	}
	replace := func(text, old, replacement string) string {
		if strings.Count(text, old) != 1 {
			t.Fatalf("negative control: %q is not in the source exactly once", old)
		}
		return strings.Replace(text, old, replacement, 1)
	}
	follows := func(changed string) []string { return checkTrayFollowsTheChannel(changed, window, appSources) }
	for _, tc := range []struct {
		name, want string
		check      func() []string
	}{
		{"a marker read on a portable copy", `UpdateChecker::SyncChannel is missing if \(!installed_`, func() []string {
			return follows(replace(checker, "  if (!installed_ || version::kCode == 0) return;\n  std::string why;\n", "  std::string why;\n"))
		}},
		{"the channel read from app_prefs.json", "UpdateChecker::SyncChannel names LoadAppPrefs", func() []string {
			return follows(replace(checker, "  const update::Feed& feed = update::FeedOfMarker(marker);\n",
				"  const update::Feed& feed = update::FeedOfMarker(marker);\n  (void)LoadAppPrefs();\n"))
		}},
		{"a feed changed without its generation", "UpdateChecker::SyncChannel is missing ChannelChanged", func() []string {
			return follows(replace(checker, "    ChannelChanged();\n    return;\n", "    return;\n"))
		}},
		{"a check that names its feed", "UpdateChecker::RunCheck names kOfficialFeed", func() []string {
			return follows(replace(checker, "  const update::Feed& feed = *followed;\n", "  const update::Feed& feed = update::kOfficialFeed;\n"))
		}},
		{"a check that takes its feed outside the lock", `UpdateChecker::RunCheck is missing followed = feed_;`, func() []string {
			return follows(replace(checker, "    followed = feed_;\n", ""))
		}},
		{"an apply that does not read the channel first", "UpdateChecker::RunApply does not read the update channel first", func() []string {
			return follows(replace(checker, "  SyncChannel();\n  Offer offer;\n", "  Offer offer;\n"))
		}},
		{"a developer build handed over as an installer", "UpdateChecker::RunApply may hand a developer build over", func() []string {
			return follows(replace(checker, "    if (!update::HandsOverInstaller(*offer.feed)) {\n", "    if (false) {\n"))
		}},
		{"a developer build's installer shown again", "UpdateChecker::RunReveal may hand a developer build over", func() []string {
			return follows(replace(checker, " ||\n          !update::HandsOverInstaller(*offer_.feed)) {", ") {"))
		}},
		{"a developer build's report offering its installer", "UpdateChecker::OffersInstaller may hand a developer build over", func() []string {
			return follows(replace(checker, " && update::HandsOverInstaller(*snapshot.feed);", ";"))
		}},
		{"a channel change from a portable copy", `UpdateChecker::RunChannelChange is missing if \(!installed_`, func() []string {
			return follows(replace(checker, "  if (!installed_ || version::kCode == 0) {\n    LogWarn(\"update: this copy has no update channel; the helper is not started\");",
				"  if (false) {\n    LogWarn(\"update: this copy has no update channel; the helper is not started\");"))
		}},
		{"the line said from the exit code", `UpdateChecker::RunChannelChange is missing \.markerAfter = install::ReadChannelMarker`, func() []string {
			return follows(replace(checker, ".markerAfter = install::ReadChannelMarker(installFolder_, why),", ".markerAfter = asked.id == update::kOfficialFeed.id ? update::ChannelMarker::Absent : update::ChannelMarker::Trusted,"))
		}},
		{"a change of channel that hands off the VPN", "UpdateChecker::RunChannelChange names HandOffVpn", func() []string {
			return follows(replace(checker, "  LogInfo(\"update: asking the update helper for the {} channel\", asked.id);\n",
				"  LogInfo(\"update: asking the update helper for the {} channel\", asked.id);\n  (void)HandOffVpn();\n"))
		}},
		{"the channel's question started hidden", `LaunchChannelHelper is missing sei\.nShow = SW_SHOWNORMAL;`, func() []string {
			return follows(replace(checker, "  sei.nShow = SW_SHOWNORMAL;\n", "  sei.nShow = SW_HIDE;\n"))
		}},
		{"a channel request queued during this app's own update", `UpdateChecker::RequestChannel is missing const bool ownUpdate`, func() []string {
			return follows(replace(checker, "    const bool ownUpdate = snapshot_.phase == Phase::Applying || applyRequested_ || manualRequested_;\n",
				"    const bool ownUpdate = false;\n"))
		}},
		{"a snapshot built from scratch without the channel", "a snapshot built from scratch drops the update channel", func() []string {
			return follows(replace(checker, "                           .feed = feed_,\n                           .marker = snapshot_.marker,\n"+
				"                           .markerPresent = snapshot_.markerPresent};", "};"))
		}},
		{"a snapshot built from scratch without what lies under the marker's name", "a snapshot built from scratch drops the update channel", func() []string {
			return follows(replace(checker, "                             .markerPresent = snapshot_.markerPresent,\n", ""))
		}},
		// S14 said whatever the reader found under the marker's name
		{"the screens told something lies under the marker's name, always", `UpdateChecker.cpp sets a marker as "snapshot_.markerPresent = true`, func() []string {
			return follows(replace(checker, "    snapshot_.markerPresent = markerPresent;\n    changed = feed.id != feed_->id;\n",
				"    snapshot_.markerPresent = true;\n    changed = feed.id != feed_->id;\n"))
		}},
		{"a report's banner left on the old feed", `UpdateChecker::ChannelChanged is missing snapshot_\.feed = feed_;`, func() []string {
			return follows(replace(checker, "    snapshot_.feed = feed_;\n    snapshot_.change = update::ChangeLine::None;\n", "    snapshot_.change = update::ChangeLine::None;\n"))
		}},
		{"the screens that show the channel never asking for it", "MainWindow does not ask for the update channel", func() []string {
			return checkTrayFollowsTheChannel(checker, replace(window, `  if (tag == L"developer" || tag == L"settings") urnw::pages::Updates().RequestChannelSync();`+"\n", ""), appSources)
		}},
		{"the developer row named in the app", "names kDeveloperFeed", func() []string {
			changed := map[string]string{}
			for name, source := range appSources {
				changed[name] = source
			}
			changed["UpdateChecker.cpp"] += "\nconst urnw::update::Feed& kChannel = urnw::update::kDeveloperFeed;\n"
			return checkTrayFollowsTheChannel(checker, window, changed)
		}},
		// the review's M2: every screen told what was asked
		{"the screens told what was asked, not what the marker says", `UpdateChecker.cpp sets a marker as "s.marker = asked.id`, func() []string {
			return follows(replace(checker, "  Mutate([line, &code](Snapshot& s) {\n    s.change = line;\n    s.changeCode = code;\n  });",
				"  Mutate([line, &code, exitCode, &asked](Snapshot& s) {\n    s.change = line;\n    s.changeCode = code;\n"+
					"    if (exitCode == 0) s.marker = asked.id == update::kOfficialFeed.id ? update::ChannelMarker::Absent : update::ChannelMarker::Trusted;\n  });"))
		}},
		// the review's M4 and M4d: every screen blind to the marker
		{"the screens blind to the marker", `UpdateChecker::ChannelLinesOf is "`, func() []string {
			return follows(replace(checker, "       .marker = snapshot.marker,\n", "       .marker = update::ChannelMarker::Absent,\n"))
		}},
		{"the marker's line dropped from the screens' view", `UpdateChecker::ChannelLinesOf is "`, func() []string {
			return follows(replace(checker, "       .marker = snapshot.marker,\n", ""))
		}},
		// the review's C11: S5 naming the snapshot's feed
		{"S5 naming the snapshot's feed", "does not name the row the marker selects in S5", func() []string {
			return follows(replace(checker, "RepositoryOf(update::FeedOfMarker(snapshot.marker))", "RepositoryOf(*snapshot.feed)"))
		}},
		// the review's B9: the tray deleting the marker itself
		{"the tray reaching the marker's path", "names ChannelMarkerPath(", func() []string {
			changed := map[string]string{}
			for name, source := range appSources {
				changed[name] = source
			}
			changed["UpdateChecker.cpp"] = replace(changed["UpdateChecker.cpp"], "void UpdateChecker::SyncChannel() {\n",
				"void UpdateChecker::SyncChannel() {\n  if (false) ::DeleteFileW(install::ChannelMarkerPath(installFolder_).c_str());\n")
			return checkTrayFollowsTheChannel(checker, window, changed)
		}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			problems := strings.Join(tc.check(), "\n")
			if !strings.Contains(problems, tc.want) && !regexp.MustCompile(tc.want).MatchString(problems) {
				t.Fatalf("negative control was not detected by %q; the check reported:\n%s", tc.want, problems)
			}
		})
	}
}

func TestUpdateApplyTrayFollowsTheFeed(t *testing.T) {
	reportProblems(t, checkTrayFollowsTheFeed(stripComments(readAppSource(t, "UpdateChecker.cpp"))))
}

// GitHub's answer to too many requests is honoured: a refused request's
// Retry-After or X-RateLimit-Reset holds every later request, manual or
// automatic, the cadence never schedules one before it, and the helper,
// which asks GitHub itself, is not started during it either.
func checkTrayHonoursGitHub(checker string) []string {
	var problems []string
	fetch := applyDefinition(checker, "bool FetchUrl(std::wstring const& url, const wchar_t* accept,")
	problems = append(problems, applyOrderProblems("FetchUrl", fetch,
		regexp.QuoteMeta("if (status != 200) {"),
		regexp.QuoteMeta(`headers.retryAfterSeconds = NumericHeader(request.h, L"Retry-After", 0);`),
		regexp.QuoteMeta(`headers.rateLimitResetUnixSeconds = NumericHeader(request.h, L"X-RateLimit-Reset", 0);`),
		regexp.QuoteMeta(`headers.rateLimitExhausted = NumericHeader(request.h, L"X-RateLimit-Remaining", -1) == 0;`),
		regexp.QuoteMeta("return false;"))...)
	check := applyDefinition(checker, "bool UpdateChecker::RunCheck(std::uint64_t generation) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunCheck", check,
		regexp.QuoteMeta("held = steady_clock::now() < holdUntil_;"),
		regexp.QuoteMeta("if (held) {"),
		regexp.QuoteMeta("CheckFailed(generation);"),
		regexp.QuoteMeta("return false;"),
		regexp.QuoteMeta("FetchUrl("),
		regexp.QuoteMeta("if (!fetched) {"),
		regexp.QuoteMeta(".retryAfterSeconds = headers.retryAfterSeconds,"),
		regexp.QuoteMeta(".resetUnixSeconds = headers.rateLimitResetUnixSeconds,"),
		regexp.QuoteMeta(".exhausted = headers.rateLimitExhausted,"),
		regexp.QuoteMeta(".serverUnixSeconds = headers.serverUnixSeconds};"),
		regexp.QuoteMeta("const std::int64_t wait = update::NextCheckDelaySeconds(0, limit);"),
		regexp.QuoteMeta("holdUntil_ = steady_clock::now() + std::chrono::seconds(wait);"),
		regexp.QuoteMeta("holdUntilUnix_ = NowUnixSeconds() + wait;"),
		regexp.QuoteMeta("snapshot_.holdUntilUnix = holdUntilUnix_;"),
		regexp.QuoteMeta("CheckFailed(generation);"),
		regexp.QuoteMeta("return false;"))...)
	apply := applyDefinition(checker, "void UpdateChecker::RunApply(std::uint64_t generation, bool manual) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunApply", apply,
		regexp.QuoteMeta("held = viaHelper && steady_clock::now() < holdUntil_;"),
		regexp.QuoteMeta("if (held) {"),
		regexp.QuoteMeta("fail(Failure::Held);"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta("LaunchUpdateHelper("))...)
	worker := applyDefinition(checker, "void UpdateChecker::WorkerLoop() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::WorkerLoop", worker,
		regexp.QuoteMeta("RunCheck(generation);"),
		regexp.QuoteMeta("nextAuto_ = std::max(steady_clock::now() + kCheckInterval, holdUntil_);"))...)
	return problems
}

func TestUpdateApplyTrayHonoursGitHub(t *testing.T) {
	reportProblems(t, checkTrayHonoursGitHub(stripComments(readAppSource(t, "UpdateChecker.cpp"))))
}

// When no check has worked for 72 hours, the app says so: since the last one
// that did, or, before any has, since the first launch that tried. The connect
// screen's banner offers to try now, or says until when GitHub asked to wait,
// and the developer line says it too. Turning automatic checks off takes the
// warning down, since the app no longer keeps trying.
func checkTraySaysWhenChecksFail(checker, connect, window, developer string) []string {
	var problems []string
	start := applyDefinition(checker, "void UpdateChecker::Start() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::Start", start,
		regexp.QuoteMeta("const auto lastSuccess = prefs.find(kLastSuccessPrefKey);"),
		regexp.QuoteMeta("if (snapshot_.lastSuccessUnix <= 0) {"),
		regexp.QuoteMeta("snapshot_.lastSuccessUnix = NowUnixSeconds();"),
		regexp.QuoteMeta("SaveAppPref(kLastSuccessPrefKey, snapshot_.lastSuccessUnix);"),
		regexp.QuoteMeta("worker_ = std::thread("))...)
	check := applyDefinition(checker, "bool UpdateChecker::RunCheck(std::uint64_t generation) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunCheck", check,
		regexp.QuoteMeta("update::ParseReleaseList(body);"),
		regexp.QuoteMeta("if (!parsed) {"),
		regexp.QuoteMeta("CheckFailed(generation);"),
		regexp.QuoteMeta("SaveAppPref(kLastSuccessPrefKey, succeeded);"),
		regexp.QuoteMeta("snapshot_.lastSuccessUnix = succeeded;"),
		regexp.QuoteMeta("snapshot_.checkStale = false;"),
		regexp.QuoteMeta("snapshot_.holdUntilUnix = 0;"))...)
	failed := applyDefinition(checker, "void UpdateChecker::CheckFailed(std::uint64_t generation) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::CheckFailed", failed,
		regexp.QuoteMeta("s.lastCheck = CheckOutcome::Failed;"),
		regexp.QuoteMeta("s.checkStale = update::CheckIsStale(now, s.lastSuccessUnix, autoCheck_ && version::kCode != 0);"))...)
	automatic := applyDefinition(checker, "void UpdateChecker::SetAutoCheckEnabled(bool on) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::SetAutoCheckEnabled", automatic,
		regexp.QuoteMeta("autoCheck_ = on;"),
		regexp.QuoteMeta("Mutate([now, on](Snapshot& s) {"),
		regexp.QuoteMeta("s.checkStale = update::CheckIsStale(now, s.lastSuccessUnix, on && version::kCode != 0);"))...)
	banner := applyDefinition(connect, offerBannerRenderer)
	problems = append(problems, applyOrderProblems("ConnectPage::RenderUpdateBanner", banner,
		regexp.QuoteMeta("const bool held = snap.holdUntilUnix > nowUnix;"),
		regexp.QuoteMeta("if (snap.phase == Phase::None) {"),
		regexp.QuoteMeta("if (!snap.checkStale) {"),
		regexp.QuoteMeta("bar.IsOpen(false);"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta(`L"Couldn't check for updates since " +`),
		regexp.QuoteMeta("urnw::UpdateChecker::LocalDate(snap.lastSuccessUnix)"),
		regexp.QuoteMeta(`held ? L"GitHub asked this network to wait until " + heldUntil +`),
		regexp.QuoteMeta("button.IsEnabled(snap.lastCheck != CheckOutcome::InFlight && !held);"),
		regexp.QuoteMeta("bar.IsOpen(true);"))...)
	action := applyDefinition(window, "void MainWindow::OnUpdateBannerAction() {")
	problems = append(problems, applyOrderProblems("MainWindow::OnUpdateBannerAction", action,
		regexp.QuoteMeta("case Phase::None:"),
		regexp.QuoteMeta("if (updateSnapshot_.checkStale) urnw::pages::Updates().CheckNow();"))...)
	line := applyDefinition(developer, "void DeveloperPage::ApplyUpdateCheck(UpdateChecker::Snapshot const& snap) {")
	problems = append(problems, applyOrderProblems("DeveloperPage::ApplyUpdateCheck", line,
		regexp.QuoteMeta("if (snap.checkStale) {"),
		regexp.QuoteMeta(`L"Couldn't check for updates since " +`),
		regexp.QuoteMeta("UpdateChecker::LocalDate(snap.lastSuccessUnix);"),
		regexp.QuoteMeta("updateCheckText_.Text(hstring{text});"))...)
	return problems
}

func TestUpdateApplyTraySaysWhenChecksFail(t *testing.T) {
	reportProblems(t, checkTraySaysWhenChecksFail(stripComments(readAppSource(t, "UpdateChecker.cpp")),
		stripComments(readAppSource(t, "ConnectPage.cpp")), stripComments(readAppSource(t, "MainWindow.xaml.cpp")),
		stripComments(readAppSource(t, "DeveloperPage.cpp"))))
}

// Each worker check fails on a source that drops what it pins.
func TestUpdateApplyWiringRejectsWeakerWorkers(t *testing.T) {
	checker := stripComments(readAppSource(t, "UpdateChecker.cpp"))
	connect := stripComments(readAppSource(t, "ConnectPage.cpp"))
	window := stripComments(readAppSource(t, "MainWindow.xaml.cpp"))
	developer := stripComments(readAppSource(t, "DeveloperPage.cpp"))
	replace := func(text, old, replacement string) string {
		if strings.Count(text, old) != 1 {
			t.Fatalf("negative control: %q is not in the source exactly once", old)
		}
		return strings.Replace(text, old, replacement, 1)
	}
	within := func(opener, old, replacement string) string {
		definition := applyDefinition(checker, opener)
		return replace(checker, definition, replace(definition, old, replacement))
	}
	follows := func(source string) []string { return checkTrayFollowsTheFeed(source) }
	honours := func(source string) []string { return checkTrayHonoursGitHub(source) }
	says := func(source string) []string { return checkTraySaysWhenChecksFail(source, connect, window, developer) }
	for _, tc := range []struct {
		name  string
		check func() []string
	}{
		{"a channel change that keeps the generation", func() []string {
			return follows(replace(checker, "    ++feedGeneration_;\n", ""))
		}},
		{"a channel change that keeps the offer", func() []string {
			return follows(within("void UpdateChecker::ChannelChanged() {", "    offer_ = Offer{};\n", ""))
		}},
		{"a channel change that keeps offering the old feed's installer", func() []string {
			return follows(within("void UpdateChecker::ChannelChanged() {", "    snapshot_.offeredCode = 0;\n", ""))
		}},
		{"a channel change that drops the helper's report", func() []string {
			return follows(replace(checker, "if (snapshot_.phase != Phase::Result) {\n      snapshot_ = Snapshot{",
				"if (true) {\n      snapshot_ = Snapshot{"))
		}},
		{"a generation-blind MutateFor", func() []string {
			return follows(within("bool UpdateChecker::MutateFor(std::uint64_t generation,",
				"if (feedGeneration_ != generation) return false;", "if (false) return false;"))
		}},
		{"a check of the old feed published", func() []string {
			return follows(within("bool UpdateChecker::RunCheck(std::uint64_t generation) {",
				"if (feedGeneration_ != generation) {", "if (false) {"))
		}},
		{"a check that changes the snapshot unscoped", func() []string {
			return follows(within("bool UpdateChecker::RunCheck(std::uint64_t generation) {",
				"MutateFor(generation, [](Snapshot& s) { s.lastCheck = CheckOutcome::InFlight; });",
				"Mutate([](Snapshot& s) { s.lastCheck = CheckOutcome::InFlight; });"))
		}},
		{"the generation read after the worker unlocks", func() []string {
			return follows(replace(checker,
				"      manualRequested_ = false;\n      const std::uint64_t generation = feedGeneration_;\n      lock.unlock();\n",
				"      manualRequested_ = false;\n      lock.unlock();\n      const std::uint64_t generation = feedGeneration_;\n"))
		}},
		{"an apply backstop for any feed", func() []string {
			return follows(within("void UpdateChecker::WorkerLoop() {", "MutateFor(generation, [](Snapshot& s) {",
				"Mutate([](Snapshot& s) {"))
		}},
		{"the helper handed a release of the old feed", func() []string {
			return follows(replace(checker,
				"if (!MutateFor(generation, [](Snapshot& s) { s.stage = Stage::Installing; })) {",
				"if (!MutateFor(generation, [](Snapshot& s) { s.stage = Stage::Installing; }) && false) {"))
		}},
		{"a stage changed for any feed", func() []string {
			return follows(replace(checker,
				"if (!MutateFor(generation, [](Snapshot& s) { s.stage = Stage::Verifying; })) {",
				"Mutate([](Snapshot& s) { s.stage = Stage::Verifying; });\n  if (false) {"))
		}},
		{"an apply of the old feed's offer", func() []string {
			return follows(replace(checker,
				"if (!actionable || offer_.code == 0 || feedGeneration_ != generation) return;",
				"if (!actionable || offer_.code == 0) return;"))
		}},
		{"the helper's report dropped with the feed", func() []string {
			return follows(replace(checker, "Mutate([this, &result](Snapshot& s) {", "MutateFor(generation, [this, &result](Snapshot& s) {"))
		}},
		{"an installer shown again for the old feed", func() []string {
			return follows(within("void UpdateChecker::RunReveal(std::uint64_t generation) {",
				"feedGeneration_ != generation || ", ""))
		}},
		{"Retry-After not read", func() []string {
			return honours(replace(checker,
				"    headers.retryAfterSeconds = NumericHeader(request.h, L\"Retry-After\", 0);\n", ""))
		}},
		{"a spent hour not noticed", func() []string {
			return honours(replace(checker,
				"NumericHeader(request.h, L\"X-RateLimit-Remaining\", -1) == 0;",
				"NumericHeader(request.h, L\"X-RateLimit-Remaining\", -1) == -2;"))
		}},
		{"a request during GitHub's hold", func() []string {
			return honours(replace(checker, "held = steady_clock::now() < holdUntil_;", "held = false;"))
		}},
		{"the helper started during GitHub's hold", func() []string {
			return honours(replace(checker, "held = viaHelper && steady_clock::now() < holdUntil_;", "held = false;"))
		}},
		{"the hold never set", func() []string {
			return honours(replace(checker, "holdUntil_ = steady_clock::now() + std::chrono::seconds(wait);", "(void)wait;"))
		}},
		{"the hold's end never said", func() []string {
			return honours(replace(checker, "      snapshot_.holdUntilUnix = holdUntilUnix_;\n", ""))
		}},
		{"the cadence before the hold", func() []string {
			return honours(replace(checker, "nextAuto_ = std::max(steady_clock::now() + kCheckInterval, holdUntil_);",
				"nextAuto_ = steady_clock::now() + kCheckInterval;"))
		}},
		{"no baseline before the first success", func() []string {
			return says(replace(checker, "    snapshot_.lastSuccessUnix = NowUnixSeconds();\n", ""))
		}},
		{"a success never recorded", func() []string {
			return says(replace(checker, "  SaveAppPref(kLastSuccessPrefKey, succeeded);\n", ""))
		}},
		{"a success that leaves the warning up", func() []string {
			return says(replace(checker, "    snapshot_.checkStale = false;\n", ""))
		}},
		{"staleness never worked out", func() []string {
			return says(replace(checker,
				"s.checkStale = update::CheckIsStale(now, s.lastSuccessUnix, autoCheck_ && version::kCode != 0);",
				"s.checkStale = false && now;"))
		}},
		{"checks turned off, and the app still says it keeps trying", func() []string {
			return says(replace(checker,
				"s.checkStale = update::CheckIsStale(now, s.lastSuccessUnix, on && version::kCode != 0);", "(void)now;"))
		}},
		{"the banner silent when checks fail", func() []string {
			return checkTraySaysWhenChecksFail(checker, replace(connect, "if (!snap.checkStale) {", "if (true) {"),
				window, developer)
		}},
		{"the banner promising a check during GitHub's hold", func() []string {
			return checkTraySaysWhenChecksFail(checker, replace(connect,
				`held ? L"GitHub asked this network to wait until " + heldUntil +`,
				`false ? L"GitHub asked this network to wait until " + heldUntil +`), window, developer)
		}},
		{"the banner's button that does nothing", func() []string {
			return checkTraySaysWhenChecksFail(checker, connect,
				replace(window, "if (updateSnapshot_.checkStale) urnw::pages::Updates().CheckNow();", ""), developer)
		}},
		{"the developer line silent when checks fail", func() []string {
			return checkTraySaysWhenChecksFail(checker, connect, window,
				replace(developer, "if (snap.checkStale) {", "if (false) {"))
		}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if problems := tc.check(); len(problems) == 0 {
				t.Fatal("negative control was not detected")
			}
		})
	}
}

// The package closes the running app before it replaces its files, and the
// close can never fail the install:
//   - one util:CloseApplication, for URnetwork.exe, by WM_CLOSE alone, with
//     RebootPrompt="no" said out loud (WiX's default is yes) and no
//     TerminateProcess, ElevatedCloseMessage or ElevatedEndSessionMessage:
//     scheduled before InstallValidate, each of those would schedule a
//     deferred action outside the script and fail every install with 2762;
//   - only for the in-app update (UPDATE_RELAUNCH="1") and a repair or removal
//     of this package, never on a re-run of it, and only when the installed
//     URnetwork.exe is new enough to quit on WM_CLOSE (an AppSearch above
//     2026.10.5.1 where INSTALLFOLDER puts it);
//   - Wix4CloseApplications moved before InstallValidate.
//
// Every other install leaves the app to Windows Installer's Restart Manager,
// which ends an app that does not exit when asked (MSIRMSHUTDOWN=1). After an
// update the helper ran, the package starts the app again through the helper,
// unelevated, once its files are in place, and never on an uninstall or from
// the old product an upgrade removes; the helper ships as its own component.
// Uninstalling removes updates\ beside it, but not the old product's removal
// during an upgrade, whose new version installs from there; the folder's path
// is set on every install, since WixRemoveFoldersEx fails a row whose
// property is empty.
func checkInstallerCloseAndRelaunch(document xmlNode) []string {
	var problems []string
	closes := document.descendants(utilNamespace, "CloseApplication")
	if len(closes) != 1 {
		return append(problems, fmt.Sprintf("Package.wxs has %d util:CloseApplication, want one", len(closes)))
	}
	closer := closes[0]
	for name, want := range map[string]string{
		"Target":       "URnetwork.exe",
		"CloseMessage": "yes",
		"RebootPrompt": "no",
		"Condition":    `URNETWORK_APP_CLOSABLE AND ((NOT Installed AND UPDATE_RELAUNCH = "1") OR REINSTALL OR REMOVE)`,
	} {
		if value, _ := closer.attribute(name); value != want {
			problems = append(problems, fmt.Sprintf("CloseApplication %s = %q, want %q", name, value, want))
		}
	}
	for _, deferred := range []string{"TerminateProcess", "ElevatedCloseMessage", "ElevatedEndSessionMessage",
		"PromptToContinue"} {
		if _, ok := closer.attribute(deferred); ok {
			problems = append(problems, "CloseApplication sets "+deferred+
				": before InstallValidate a deferred action fails every install with 2762")
		}
	}
	if timeout, _ := closer.attribute("Timeout"); timeout == "" || len(timeout) > 2 {
		problems = append(problems, "CloseApplication's Timeout is not a short, explicit number of seconds: "+timeout)
	}
	closable := findByID(document.descendants(wixNamespace, "Property"), "URNETWORK_APP_CLOSABLE")
	if closable == nil {
		problems = append(problems, "URNETWORK_APP_CLOSABLE is not a property")
	} else {
		if secure, _ := closable.attribute("Secure"); secure != "yes" {
			problems = append(problems, "URNETWORK_APP_CLOSABLE is not secure")
		}
		search := closable.child(wixNamespace, "DirectorySearch")
		var file *xmlNode
		if search != nil {
			file = search.child(wixNamespace, "FileSearch")
		}
		path, depth, name, minVersion := "", "", "", ""
		if search != nil {
			path, _ = search.attribute("Path")
			depth, _ = search.attribute("Depth")
		}
		if file != nil {
			name, _ = file.attribute("Name")
			minVersion, _ = file.attribute("MinVersion")
		}
		if path != "[ProgramFiles64Folder]URnetwork" || depth != "0" || name != "URnetwork.exe" ||
			minVersion != "2026.10.5.1" {
			problems = append(problems, fmt.Sprintf("URNETWORK_APP_CLOSABLE searches %q depth %q for %q above %q, "+
				"want the installed URnetwork.exe above 2026.10.5.1", path, depth, name, minVersion))
		}
	}
	if shutdown := findByID(document.descendants(wixNamespace, "Property"), "MSIRMSHUTDOWN"); shutdown == nil {
		problems = append(problems, "MSIRMSHUTDOWN is not set: Windows Installer's Restart Manager leaves an app "+
			"that does not exit running with no tray icon")
	} else if value, _ := shutdown.attribute("Value"); value != "1" {
		problems = append(problems, "MSIRMSHUTDOWN is "+value+", not 1: an app that does not exit when asked "+
			"keeps running with no tray icon after the install")
	}
	sequence := document.descendants(wixNamespace, "InstallExecuteSequence")
	if len(sequence) != 1 {
		// launch_at_startup_wiring_test.go reads the first one as the package's
		problems = append(problems, fmt.Sprintf("Package.wxs has %d InstallExecuteSequence elements, want one", len(sequence)))
	}
	var customs []*xmlNode
	for _, node := range sequence {
		customs = append(customs, node.children(wixNamespace, "Custom")...)
	}
	moved, relaunched := false, false
	for _, custom := range customs {
		action, _ := custom.attribute("Action")
		switch action {
		case "override Wix4CloseApplications_$(sys.BUILDARCHSHORT)":
			before, _ := custom.attribute("Before")
			moved = before == "InstallValidate"
		case "RelaunchAfterUpdate":
			after, _ := custom.attribute("After")
			condition, _ := custom.attribute("Condition")
			relaunched = after == "InstallFinalize" &&
				condition == `UPDATE_RELAUNCH = "1" AND NOT (REMOVE ~= "ALL") AND NOT UPGRADINGPRODUCTCODE`
		}
	}
	if !moved {
		problems = append(problems, "Wix4CloseApplications is not moved before InstallValidate")
	}
	if !relaunched {
		problems = append(problems, "RelaunchAfterUpdate does not run after InstallFinalize on the helper's "+
			`UPDATE_RELAUNCH = "1" only, outside uninstalls and the old product's removal`)
	}
	relaunch := findByID(document.descendants(wixNamespace, "CustomAction"), "RelaunchAfterUpdate")
	if relaunch == nil {
		problems = append(problems, "RelaunchAfterUpdate is not a custom action")
	} else {
		for name, want := range map[string]string{
			"DllEntry":    "WixUnelevatedShellExec",
			"BinaryRef":   "Wix4UtilCA_$(sys.BUILDARCHSHORT)",
			"Execute":     "immediate",
			"Impersonate": "yes",
			"Return":      "ignore",
		} {
			if value, _ := relaunch.attribute(name); value != want {
				problems = append(problems, fmt.Sprintf("RelaunchAfterUpdate %s = %q, want %q", name, value, want))
			}
		}
	}
	properties := document.descendants(wixNamespace, "Property")
	if target := findByID(properties, "WixUnelevatedShellExecTarget"); target == nil {
		problems = append(problems, "WixUnelevatedShellExecTarget is not set")
	} else if value, _ := target.attribute("Value"); value != "[#UpdaterExe]" {
		problems = append(problems, "the relaunch starts "+value+
			", not the helper: URnetwork.exe started without its after-update argument is refused by the update marker")
	}
	if property := findByID(properties, "UPDATE_RELAUNCH"); property == nil {
		problems = append(problems, "UPDATE_RELAUNCH is not declared")
	} else if secure, _ := property.attribute("Secure"); secure != "yes" {
		problems = append(problems, "UPDATE_RELAUNCH is not secure")
	}
	updater := findByID(document.descendants(wixNamespace, "Component"), "UpdaterExe")
	var updaterFile *xmlNode
	if updater != nil {
		updaterFile = updater.child(wixNamespace, "File")
	}
	if updaterFile == nil {
		problems = append(problems, "the helper has no UpdaterExe component")
	} else {
		id, _ := updaterFile.attribute("Id")
		source, _ := updaterFile.attribute("Source")
		if id != "UpdaterExe" || windowsBase(source) != "URnetworkUpdate.exe" {
			problems = append(problems, "the UpdaterExe component does not install URnetworkUpdate.exe as UpdaterExe")
		}
	}
	excluded := false
	for _, node := range document.descendants(wixNamespace, "Exclude") {
		if files, _ := node.attribute("Files"); windowsBase(files) == "URnetworkUpdate.exe" {
			excluded = true
		}
	}
	if !excluded {
		problems = append(problems, "the RuntimeFiles harvest installs URnetworkUpdate.exe a second time")
	}
	referenced := false
	if main := findByID(document.descendants(wixNamespace, "Feature"), "Main"); main != nil {
		for _, reference := range main.children(wixNamespace, "ComponentRef") {
			if id, _ := reference.attribute("Id"); id == "UpdaterExe" {
				referenced = true
			}
		}
	}
	if !referenced {
		problems = append(problems, "the Main feature does not install the helper")
	}
	var removes []*xmlNode
	if updater != nil {
		removes = updater.children(utilNamespace, "RemoveFolderEx")
	}
	if len(removes) != 1 {
		problems = append(problems, fmt.Sprintf("the UpdaterExe component has %d util:RemoveFolderEx, want one for updates\\", len(removes)))
	} else {
		on, _ := removes[0].attribute("On")
		property, _ := removes[0].attribute("Property")
		if on != "uninstall" || property != "URNETWORK_UPDATES_FOLDER" {
			problems = append(problems, fmt.Sprintf("RemoveFolderEx removes %q on %q, want URNETWORK_UPDATES_FOLDER on uninstall",
				property, on))
		}
		if condition, _ := removes[0].attribute("Condition"); condition != `REMOVE~="ALL" AND NOT UPGRADINGPRODUCTCODE` {
			problems = append(problems, fmt.Sprintf("RemoveFolderEx's condition is %q, want %q: updates\\ goes on an "+
				"uninstall of this product, never under the new version an upgrade installs from it",
				condition, `REMOVE~="ALL" AND NOT UPGRADINGPRODUCTCODE`))
		}
	}
	if folder := findByID(document.descendants(wixNamespace, "SetProperty"), "URNETWORK_UPDATES_FOLDER"); folder == nil {
		problems = append(problems, "URNETWORK_UPDATES_FOLDER is not set before RemoveFolderEx reads it")
	} else {
		for name, want := range map[string]string{
			"Value":    `[ProgramFiles64Folder]URnetwork\updates`,
			"Before":   "Wix4RemoveFoldersEx_$(sys.BUILDARCHSHORT)",
			"Sequence": "execute",
		} {
			if value, _ := folder.attribute(name); value != want {
				problems = append(problems, fmt.Sprintf("URNETWORK_UPDATES_FOLDER's %s = %q, want %q", name, value, want))
			}
		}
		if condition, set := folder.attribute("Condition"); set {
			problems = append(problems, fmt.Sprintf("URNETWORK_UPDATES_FOLDER is set only when %q: on every other "+
				"install WixRemoveFoldersEx fails its row for the empty property", condition))
		}
	}
	return problems
}

// Each installer check fails on a package that drops what it pins.
func TestUpdateApplyInstallerRejectsWeakerPackages(t *testing.T) {
	root := repositoryRoot(t)
	data, err := os.ReadFile(filepath.Join(root, "app", "installer", "Package.wxs"))
	if err != nil {
		t.Fatal(err)
	}
	source := string(data)
	for _, tc := range []struct{ name, old, replacement string }{
		{"TerminateProcess", `CloseMessage="yes" RebootPrompt="no" Timeout="15"`,
			`CloseMessage="yes" RebootPrompt="no" TerminateProcess="1" Timeout="15"`},
		{"an elevated close message", `CloseMessage="yes" RebootPrompt="no" Timeout="15"`,
			`CloseMessage="yes" ElevatedCloseMessage="yes" RebootPrompt="no" Timeout="15"`},
		{"WiX's default reboot prompt", `CloseMessage="yes" RebootPrompt="no" Timeout="15"`,
			`CloseMessage="yes" Timeout="15"`},
		{"a re-run of the installed package closes the app",
			`Condition='URNETWORK_APP_CLOSABLE AND ((NOT Installed AND UPDATE_RELAUNCH = "1") OR REINSTALL OR REMOVE)'`,
			`Condition='URNETWORK_APP_CLOSABLE AND (UPDATE_RELAUNCH = "1" OR Installed)'`},
		{"an app without the WM_CLOSE handler closed",
			`Condition='URNETWORK_APP_CLOSABLE AND ((NOT Installed AND UPDATE_RELAUNCH = "1") OR REINSTALL OR REMOVE)'`,
			`Condition='(NOT Installed AND UPDATE_RELAUNCH = "1") OR REINSTALL OR REMOVE'`},
		{"the close for any install, by hand or by a tool",
			`Condition='URNETWORK_APP_CLOSABLE AND ((NOT Installed AND UPDATE_RELAUNCH = "1") OR REINSTALL OR REMOVE)'`,
			`Condition='URNETWORK_APP_CLOSABLE AND (NOT Installed OR REINSTALL OR REMOVE)'`},
		{"an app that does not exit left running", `<Property Id="MSIRMSHUTDOWN" Value="1" />`,
			`<Property Id="MSIRMSHUTDOWN" Value="0" />`},
		{"updates\\ never removed", "          <util:RemoveFolderEx xmlns:util=\"http://wixtoolset.org/schemas/v4/wxs/util\"\n" +
			"                               Id=\"RemoveUpdatesFolder\" On=\"uninstall\" Property=\"URNETWORK_UPDATES_FOLDER\"\n" +
			"                               Condition='REMOVE~=\"ALL\" AND NOT UPGRADINGPRODUCTCODE' />\n", ""},
		{"updates\\ removed on every install", `On="uninstall" Property="URNETWORK_UPDATES_FOLDER"`,
			`On="both" Property="URNETWORK_UPDATES_FOLDER"`},
		{"updates\\ removed under the new version installing from it",
			`Condition='REMOVE~="ALL" AND NOT UPGRADINGPRODUCTCODE' />`, `Condition='REMOVE~="ALL"' />`},
		{"updates\\ removed with the component, upgrades included",
			"Property=\"URNETWORK_UPDATES_FOLDER\"\n                               Condition='REMOVE~=\"ALL\" AND NOT UPGRADINGPRODUCTCODE' />",
			`Property="URNETWORK_UPDATES_FOLDER" />`},
		{"the folder's path set only for the removal",
			"Before=\"Wix4RemoveFoldersEx_$(sys.BUILDARCHSHORT)\" Sequence=\"execute\" />",
			"Before=\"Wix4RemoveFoldersEx_$(sys.BUILDARCHSHORT)\" Sequence=\"execute\"\n" +
				"                 Condition='REMOVE~=\"ALL\" AND NOT UPGRADINGPRODUCTCODE' />"},
		{"the gate on any app", `<FileSearch Name="URnetwork.exe" MinVersion="2026.10.5.1" />`,
			`<FileSearch Name="URnetwork.exe" />`},
		{"the close in WiX's slot", `Before="InstallValidate"`, `Before="InstallFiles"`},
		{"the relaunch elevated", `DllEntry="WixUnelevatedShellExec"`, `DllEntry="WixShellExec"`},
		{"the relaunch refused by the marker", `Value="[#UpdaterExe]"`, `Value="[#URnetworkExe]"`},
		{"a relaunch on a manual install", `Condition='UPDATE_RELAUNCH = "1" AND NOT (REMOVE ~= "ALL") AND NOT UPGRADINGPRODUCTCODE'`,
			`Condition='NOT (REMOVE ~= "ALL") AND NOT UPGRADINGPRODUCTCODE'`},
		{"a relaunch from the removed product", `Condition='UPDATE_RELAUNCH = "1" AND NOT (REMOVE ~= "ALL") AND NOT UPGRADINGPRODUCTCODE'`,
			`Condition='UPDATE_RELAUNCH = "1" AND NOT (REMOVE ~= "ALL")'`},
		{"a failed relaunch failing the install", "Impersonate=\"yes\"\n                  Return=\"ignore\" />",
			"Impersonate=\"yes\"\n                  Return=\"check\" />"},
		{"the helper harvested twice", "        <Exclude Files=\"$(var.BinDir)\\URnetworkUpdate.exe\" />\n", ""},
		{"a second install sequence", "    <!-- Both are scheduled in the one InstallExecuteSequence below. -->\n",
			"    <InstallExecuteSequence />\n"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if strings.Count(source, tc.old) != 1 {
				t.Fatalf("negative control: %q is not in Package.wxs exactly once", tc.old)
			}
			path := filepath.Join(t.TempDir(), "Package.wxs")
			if err := os.WriteFile(path, []byte(strings.Replace(source, tc.old, tc.replacement, 1)), 0600); err != nil {
				t.Fatal(err)
			}
			if problems := checkInstallerCloseAndRelaunch(parseXML(t, path)); len(problems) == 0 {
				t.Fatal("negative control was not detected")
			}
		})
	}
}
