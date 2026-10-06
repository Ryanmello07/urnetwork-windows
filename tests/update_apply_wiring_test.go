// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"fmt"
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

// Before anything else the helper limits where libraries load from and drops
// the app's URNETWORK_* overrides; nothing in it reads the environment or the
// user's storage.
func checkUpdaterEntry(main string, sources map[string]string) []string {
	entry := applyDefinition(main, "int WINAPI wWinMain(")
	problems := applyOrderProblems("wWinMain", entry,
		regexp.QuoteMeta("::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);"),
		regexp.QuoteMeta("DropAppOverrides();"),
		regexp.QuoteMeta("::CommandLineToArgvW("),
		regexp.QuoteMeta(`if (args.size() == 3 && args[1] == L"--apply-update") return urnw::updater::ApplyUpdate(args[2]);`),
		regexp.QuoteMeta("if (args.size() == 1) return RelaunchApp();"),
		regexp.QuoteMeta("return static_cast<int>(urnw::update::Refusal::BadArguments);"))
	if strings.Index(entry, "SetDefaultDllDirectories") > strings.Index(entry, "CommandLineToArgvW") {
		problems = append(problems, "wWinMain loads shell32 before it limits the library search")
	}
	drop := applyDefinition(main, "void DropAppOverrides() {")
	problems = append(problems, applyOrderProblems("DropAppOverrides", drop,
		regexp.QuoteMeta("::GetEnvironmentStringsW();"),
		regexp.QuoteMeta(`if (upper.rfind(L"URNETWORK_", 0) == 0) names.push_back(std::move(name));`),
		regexp.QuoteMeta("::SetEnvironmentVariableW(name.c_str(), nullptr);"))...)
	relaunch := applyDefinition(main, "int RelaunchApp() {")
	problems = append(problems, applyOrderProblems("RelaunchApp", relaunch,
		regexp.QuoteMeta("if (IsFullyElevated()) return 0;"),
		regexp.QuoteMeta(`folder / L"URnetwork.exe";`),
		regexp.QuoteMeta(`L"\" --after-update";`),
		regexp.QuoteMeta("::CreateProcessW(app.c_str(), command.data(),"))...)
	for name, source := range sources {
		for _, forbidden := range []string{"GetEnvironmentVariable", "getenv", "StorageRoot(", "Paths.h",
			"LoadAppPrefs", "SHGetKnownFolderPath(FOLDERID_LocalAppData"} {
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
	for _, name := range []string{"main.cpp", "ApplyUpdate.cpp", "Http.cpp", "HelperLog.cpp"} {
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
// the package's identity; the image moved out of the way; msiexec; the result.
func checkApplyUpdate(apply string) []string {
	body := applyDefinition(apply, "int ApplyUpdate(std::wstring_view tagArgument) {")
	return applyOrderProblems("ApplyUpdate", body,
		regexp.QuoteMeta("if (!IsElevated()) {"),
		regexp.QuoteMeta("return static_cast<int>(Refusal::NotElevated);"),
		regexp.QuoteMeta("const fs::path executable = install::OwnExecutablePath();"),
		regexp.QuoteMeta("!install::AdminOnlyLocation(executable, why)"),
		regexp.QuoteMeta("return static_cast<int>(Refusal::NotInstalled);"),
		regexp.QuoteMeta("if (version::kCode == 0) {"),
		regexp.QuoteMeta("const update::Feed& feed = ChannelFeed();"),
		regexp.QuoteMeta("if (!update::IsTagArgument(feed, tag)) {"),
		regexp.QuoteMeta(`::CreateMutexW(nullptr, FALSE, L"Global\\URnetworkUpdateHelper")`),
		regexp.QuoteMeta(`const fs::path updates = installFolder / L"updates";`),
		regexp.QuoteMeta("if (!PrepareFolder(updates, error) || !PrepareFolder(tagFolder, error)) {"),
		regexp.QuoteMeta(`L"https://api.github.com/repositories/{}/releases?per_page=15", feed.numericRepoId);`),
		regexp.QuoteMeta("HttpGet("),
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
		regexp.QuoteMeta("nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));"),
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
		regexp.QuoteMeta("::MoveFileExW(executable.c_str(), running.c_str(), MOVEFILE_WRITE_THROUGH)"),
		regexp.QuoteMeta(`const fs::path msiexec = systemFolder / L"msiexec.exe";`),
		regexp.QuoteMeta("update::MsiexecCommandLine(msiexec.native(), package.native(),"),
		regexp.QuoteMeta("::CreateProcessW(msiexec.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,"),
		regexp.QuoteMeta("::WaitForSingleObject(installer.get(), INFINITE);"),
		regexp.QuoteMeta("::GetExitCodeProcess(installer.get(), &exitCode);"),
		regexp.QuoteMeta("held.Close();"),
		regexp.QuoteMeta("if (update::KeepsPackage(exitCode)) {"),
		regexp.QuoteMeta("::DeleteFileW(package.c_str());"),
		regexp.QuoteMeta("return finish(exitCode);"))
}

func TestUpdateApplyHelperChecksInOrder(t *testing.T) {
	reportProblems(t, checkApplyUpdate(updaterSources(t)["ApplyUpdate.cpp"]))
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
// /p:UrnUpdateRunnerTest* properties, never from the environment, and nothing
// this repository builds official releases with passes them: build.ps1 and
// every workflow here are checked for any feed setting at all.
func checkRunnerFeedIsTestOnly(buildScript string, workflows map[string]string, project, apply string,
	sources map[string]string) []string {
	var problems []string
	official := map[string]string{"app/build.ps1": buildScript}
	for name, text := range workflows {
		official[name] = text
	}
	feedDefault := regexp.MustCompile(`UrnUpdateFeedDefault=["']?([A-Za-z0-9_-]*)`)
	for name, text := range official {
		for _, setting := range []string{"UrnUpdateRunnerTest", "URN_UPDATE_RUNNER_TEST", "URN_UPDATE_FEED_DEFAULT_BETA"} {
			if strings.Contains(text, setting) {
				problems = append(problems, name+" sets "+setting+": official builds must poll the official feed")
			}
		}
		for _, match := range feedDefault.FindAllStringSubmatch(text, -1) {
			if match[1] != "official" {
				problems = append(problems, name+" passes UrnUpdateFeedDefault="+match[1]+
					": official builds must poll the official feed")
			}
		}
	}
	if !strings.Contains(project, `<ItemDefinitionGroup Condition="'$(UrnUpdateRunnerTestRepoId)'!=''">`) {
		problems = append(problems, "Updater.vcxproj defines a runner feed without its property")
	}
	if !strings.Contains(project, "$([System.Environment]::GetEnvironmentVariable(`UrnUpdateRunnerTestRepoId`))") {
		problems = append(problems, "Updater.vcxproj takes a runner feed from the environment")
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

func officialWorkflows(t *testing.T) map[string]string {
	t.Helper()
	root := repositoryRoot(t)
	paths, err := filepath.Glob(filepath.Join(root, ".github", "workflows", "*.y*ml"))
	if err != nil {
		t.Fatal(err)
	}
	if len(paths) == 0 {
		t.Fatal("no workflows under .github/workflows")
	}
	workflows := map[string]string{}
	for _, path := range paths {
		data, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		workflows[filepath.ToSlash(path[len(root)+1:])] = string(data)
	}
	return workflows
}

func TestUpdateApplyRunnerFeedIsTestOnly(t *testing.T) {
	root := repositoryRoot(t)
	build, err := os.ReadFile(filepath.Join(root, "app", "build.ps1"))
	if err != nil {
		t.Fatal(err)
	}
	sources := updaterSources(t)
	appSources := appSourceFiles(t, ".cpp", ".h")
	for name, source := range appSources {
		sources["App/"+name] = stripComments(source)
	}
	reportProblems(t, checkRunnerFeedIsTestOnly(string(build), officialWorkflows(t),
		readUpdaterSource(t, "Updater.vcxproj"), sources["ApplyUpdate.cpp"], sources))
}

// Each check above fails on a source that drops the defence it pins.
func TestUpdateApplyWiringRejectsWeakerHelpers(t *testing.T) {
	root := repositoryRoot(t)
	solutionData, err := os.ReadFile(filepath.Join(root, "app", "URnetwork.sln"))
	if err != nil {
		t.Fatal(err)
	}
	buildData, err := os.ReadFile(filepath.Join(root, "app", "build.ps1"))
	if err != nil {
		t.Fatal(err)
	}
	project := readUpdaterSource(t, "Updater.vcxproj")
	solution := string(solutionData)
	common := readCommonSource(t, "Common.vcxproj")
	sources := updaterSources(t)
	workflows := officialWorkflows(t)
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
	for _, tc := range []struct {
		name  string
		check func() []string
	}{
		{"the UpgradeCode check skipped", func() []string {
			return checkApplyUpdate(replace(sources["ApplyUpdate.cpp"],
				"!update::PackageMatches(*upgradeCode, *productVersion, selection.code)", "false"))
		}},
		{"the digest compared to nothing", func() []string {
			return checkApplyUpdate(replace(sources["ApplyUpdate.cpp"],
				"!update::EqualsAsciiCaseless(actual, selection.digestHex)", "false"))
		}},
		{"any offered release installed", func() []string {
			return checkApplyUpdate(replace(sources["ApplyUpdate.cpp"],
				"if (!update::SelectionOffers(selection, tag, version::kCode)) {", "if (selection.code == 0) {"))
		}},
		{"a redirect anywhere", func() []string {
			return checkApplyUpdate(replace(sources["ApplyUpdate.cpp"],
				"if (redirect.status != 302 || !update::IsAllowedAssetRedirect(redirect.location) ||",
				"if (redirect.status != 302 ||"))
		}},
		{"hashed after the handle is gone", func() []string {
			return checkApplyUpdate(replace(sources["ApplyUpdate.cpp"],
				"const std::string actual = Sha256(held.get());", "const std::string actual = HashPath(package);"))
		}},
		{"the image left in the installer's way", func() []string {
			return checkApplyUpdate(replace(sources["ApplyUpdate.cpp"],
				"::MoveFileExW(executable.c_str(), running.c_str(), MOVEFILE_WRITE_THROUGH)", "false"))
		}},
		{"a user-writable location", func() []string {
			return checkApplyUpdate(replace(sources["ApplyUpdate.cpp"],
				"!install::AdminOnlyLocation(executable, why)", "false"))
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
			apply := replace(sources["ApplyUpdate.cpp"], "  const fs::path installFolder = executable.parent_path();",
				"  const fs::path installFolder = _wgetenv(L\"URNETWORK_APP_ROOT\");")
			return checkUpdaterEntry(sources["main.cpp"], with("ApplyUpdate.cpp", apply))
		}},
		{"the relaunch started elevated", func() []string {
			main := replace(sources["main.cpp"], "  if (IsFullyElevated()) return 0;\n", "")
			return checkUpdaterEntry(main, with("main.cpp", main))
		}},
		{"build.ps1 passes a runner feed", func() []string {
			return checkRunnerFeedIsTestOnly(string(buildData)+"\n/p:UrnUpdateRunnerTestRepoId=1\n", workflows,
				project, sources["ApplyUpdate.cpp"], sources)
		}},
		{"build.ps1 passes another feed", func() []string {
			return checkRunnerFeedIsTestOnly(string(buildData)+"\n/p:UrnUpdateFeedDefault=beta\n", workflows,
				project, sources["ApplyUpdate.cpp"], sources)
		}},
		{"a workflow passes a runner feed", func() []string {
			changed := map[string]string{".github/workflows/extra.yml": "run: msbuild /p:UrnUpdateRunnerTestRepoId=1"}
			for name, text := range workflows {
				changed[name] = text
			}
			return checkRunnerFeedIsTestOnly(string(buildData), changed, project, sources["ApplyUpdate.cpp"], sources)
		}},
		{"the runner feed from the environment", func() []string {
			return checkRunnerFeedIsTestOnly(string(buildData), workflows,
				strings.ReplaceAll(project, "$([System.Environment]::GetEnvironmentVariable(`UrnUpdateRunnerTestRepoId`))", ""),
				sources["ApplyUpdate.cpp"], sources)
		}},
		{"the runner feed as the default", func() []string {
			apply := replace(sources["ApplyUpdate.cpp"], "  return update::kOfficialFeed;\n", "  return kRunnerTestFeed;\n")
			return checkRunnerFeedIsTestOnly(string(buildData), workflows, project, apply, sources)
		}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if problems := tc.check(); len(problems) == 0 {
				t.Fatal("negative control was not detected")
			}
		})
	}
}

// The tray app hands an elevated process nothing but a tag: on an installed
// copy it starts the helper beside it and waits; on any other copy it shows
// the checked installer and elevates nothing. It never builds msiexec's
// arguments, and it does not quit for the installer.
func checkTrayRunsTheHelper(checker string) []string {
	var problems []string
	start := applyDefinition(checker, "void UpdateChecker::Start() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::Start", start,
		regexp.QuoteMeta("const fs::path exe = install::OwnExecutablePath();"),
		regexp.QuoteMeta("installed_ = !exe.empty() && install::AdminOnlyLocation(installFolder_ / kHelperName, why);"),
		regexp.QuoteMeta("snapshot_.installed = installed_;"),
		regexp.QuoteMeta("worker_ = std::thread("))...)
	apply := applyDefinition(checker, "void UpdateChecker::RunApply(std::uint64_t generation) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunApply", apply,
		regexp.QuoteMeta("const fs::path dir = UpdatesDir() / offer.tag;"),
		regexp.QuoteMeta("const std::string actual = Sha256File(msiPath);"),
		regexp.QuoteMeta("fail(Failure::Checksum);"),
		regexp.QuoteMeta("if (!installed_) {"),
		regexp.QuoteMeta("s.phase = Phase::ManualInstall;"),
		regexp.QuoteMeta("RevealInExplorer(msiW);"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta("LaunchUpdateHelper(installFolder_ / kHelperName, offer.tag, &helper, launchError)"),
		regexp.QuoteMeta("fail(Failure::Elevation);"),
		regexp.QuoteMeta("::WaitForSingleObject(helper, 250)"),
		regexp.QuoteMeta("if (cancelled()) break;"),
		regexp.QuoteMeta(`ReadResult(installFolder_ / L"updates" / L"last-result.json");`),
		regexp.QuoteMeta("s.phase = Phase::Result;"))...)
	if portable := strings.Index(apply, "if (!installed_) {"); portable >= 0 {
		branch := apply[portable:]
		if end := strings.Index(branch, "\n  }\n"); end >= 0 {
			branch = branch[:end]
		}
		for _, forbidden := range []string{"LaunchUpdateHelper", "runas", "ShellExecute"} {
			if strings.Contains(branch, forbidden) {
				problems = append(problems, "the portable branch of RunApply runs "+forbidden+
					": a copy outside an admin-only install elevates nothing")
			}
		}
	}
	launch := applyDefinition(checker,
		"bool LaunchUpdateHelper(fs::path const& helperPath, std::wstring const& tag, HANDLE* helper,")
	problems = append(problems, applyOrderProblems("LaunchUpdateHelper", launch,
		regexp.QuoteMeta(`const std::wstring params = L"--apply-update " + tag;`),
		regexp.QuoteMeta(`sei.lpVerb = L"runas";`),
		regexp.QuoteMeta("sei.lpFile = helperPath.c_str();"),
		regexp.QuoteMeta("sei.lpParameters = params.c_str();"))...)
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
// shown until the user dismisses it.
func checkTrayReadsTheReport(checker string) []string {
	var problems []string
	show := applyDefinition(checker, "void UpdateChecker::ShowLastResult() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::ShowLastResult", show,
		regexp.QuoteMeta("if (!installed_) return;"),
		regexp.QuoteMeta(`ReadResult(installFolder_ / L"updates" / L"last-result.json");`),
		regexp.QuoteMeta("if (SeenResult() == result->finishedUtc) return;"),
		regexp.QuoteMeta("s.phase = Phase::Result;"))...)
	read := applyDefinition(checker, "std::optional<update::UpdateResult> ReadResult(fs::path const& file) {")
	problems = append(problems, applyOrderProblems("ReadResult", read,
		regexp.QuoteMeta("return update::ParseUpdateResult(text);"))...)
	worker := applyDefinition(checker, "void UpdateChecker::WorkerLoop() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::WorkerLoop", worker,
		regexp.QuoteMeta("ShowLastResult();"), regexp.QuoteMeta("CleanupStaleFiles();"))...)
	dismiss := applyDefinition(checker, "void UpdateChecker::DismissResult() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::DismissResult", dismiss,
		regexp.QuoteMeta("SaveAppPref(kResultSeenPrefKey, Narrow(finished));"))...)
	check := applyDefinition(checker, "void UpdateChecker::RunCheck(std::uint64_t generation) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunCheck", check,
		regexp.QuoteMeta("if (snapshot_.phase != Phase::None && snapshot_.phase != Phase::Result) {"))...)
	return problems
}

func TestUpdateApplyTrayReadsTheReport(t *testing.T) {
	reportProblems(t, checkTrayReadsTheReport(stripComments(readAppSource(t, "UpdateChecker.cpp"))))
}

// The installer's relaunch after an update is the one launch the update
// marker must not turn away: it carries --after-update and waits, bounded,
// for the update to end before it asks like every launch.
func checkRelaunchWaits(main, glue, handover string) []string {
	var problems []string
	if !strings.Contains(handover, `inline constexpr std::wstring_view kAfterUpdateArgument = L"--after-update";`) {
		problems = append(problems, "InstanceHandover.h no longer names the relaunch's argument --after-update")
	}
	if !regexp.MustCompile(`inline constexpr std::chrono::milliseconds kAfterUpdateBudget\{\d{5,6}\};`).MatchString(handover) {
		problems = append(problems, "InstanceHandover.h does not bound the relaunch's wait")
	}
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
		regexp.QuoteMeta("std::chrono::steady_clock::now() + instance::kAfterUpdateBudget;"),
		regexp.QuoteMeta("while (UpdateInProgress()) {"),
		regexp.QuoteMeta("if (std::chrono::steady_clock::now() >= deadline) {"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta("::Sleep("))...)
	return problems
}

func TestUpdateApplyTheRelaunchWaitsForTheUpdate(t *testing.T) {
	reportProblems(t, checkRelaunchWaits(appMainSource(t), stripComments(readAppSource(t, "SingleInstance.cpp")),
		stripComments(readCommonSource(t, "InstanceHandover.h"))))
}

// Each tray check fails on a source that drops what it pins.
func TestUpdateApplyWiringRejectsWeakerTrays(t *testing.T) {
	checker := stripComments(readAppSource(t, "UpdateChecker.cpp"))
	main := appMainSource(t)
	glue := stripComments(readAppSource(t, "SingleInstance.cpp"))
	handover := stripComments(readCommonSource(t, "InstanceHandover.h"))
	replace := func(text, old, replacement string) string {
		if strings.Count(text, old) != 1 {
			t.Fatalf("negative control: %q is not in the source exactly once", old)
		}
		return strings.Replace(text, old, replacement, 1)
	}
	for _, tc := range []struct {
		name  string
		check func() []string
	}{
		{"a portable copy runs the helper", func() []string {
			apply := applyDefinition(checker, "void UpdateChecker::RunApply(std::uint64_t generation) {")
			return checkTrayRunsTheHelper(replace(checker, apply, replace(apply, "if (!installed_) {", "if (false) {")))
		}},
		{"the helper started from the portable branch", func() []string {
			return checkTrayRunsTheHelper(replace(checker, "    RevealInExplorer(msiW);\n    return;\n",
				"    LaunchUpdateHelper(installFolder_ / kHelperName, offer.tag, &helper, launchError);\n    return;\n"))
		}},
		{"any copy treated as installed", func() []string {
			return checkTrayRunsTheHelper(replace(checker,
				"installed_ = !exe.empty() && install::AdminOnlyLocation(installFolder_ / kHelperName, why);",
				"installed_ = !exe.empty();"))
		}},
		{"the tray builds msiexec's arguments", func() []string {
			return checkTrayRunsTheHelper(replace(checker, `const std::wstring params = L"--apply-update " + tag;`,
				`const std::wstring params = L"/i msiexec --apply-update " + tag;`))
		}},
		{"the wait the app's teardown cannot end", func() []string {
			return checkTrayRunsTheHelper(replace(checker, "    if (cancelled()) break;\n", ""))
		}},
		{"a report read on a portable copy", func() []string {
			return checkTrayReadsTheReport(replace(checker,
				"void UpdateChecker::ShowLastResult() {\n  if (!installed_) return;\n",
				"void UpdateChecker::ShowLastResult() {\n"))
		}},
		{"a report dropped by the next check", func() []string {
			return checkTrayReadsTheReport(replace(checker,
				"if (snapshot_.phase != Phase::None && snapshot_.phase != Phase::Result) {",
				"if (snapshot_.phase != Phase::None) {"))
		}},
		{"the relaunch refused by its own update", func() []string {
			return checkRelaunchWaits(replace(main, "if (urnw::LaunchedAfterUpdate()) urnw::AwaitUpdateEnd();", ""),
				glue, handover)
		}},
		{"the relaunch waiting without a bound", func() []string {
			return checkRelaunchWaits(main, replace(glue,
				"    if (std::chrono::steady_clock::now() >= deadline) {", "    if (false) {"), handover)
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
		regexp.QuoteMeta("checkRequested_ = true;"),
		regexp.QuoteMeta("cv_.notify_all();"))...)
	scoped := applyDefinition(checker, "bool UpdateChecker::MutateFor(std::uint64_t generation,")
	problems = append(problems, applyOrderProblems("UpdateChecker::MutateFor", scoped,
		regexp.QuoteMeta("std::lock_guard lock(mutex_);"),
		regexp.QuoteMeta("if (feedGeneration_ != generation) return false;"),
		regexp.QuoteMeta("fn(snapshot_);"))...)
	worker := applyDefinition(checker, "void UpdateChecker::WorkerLoop() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::WorkerLoop", worker,
		regexp.QuoteMeta("if (applyRequested_) {"),
		regexp.QuoteMeta("const std::uint64_t generation = feedGeneration_;"),
		regexp.QuoteMeta("lock.unlock();"),
		regexp.QuoteMeta("RunApply(generation);"),
		regexp.QuoteMeta("const std::uint64_t generation = feedGeneration_;"),
		regexp.QuoteMeta("lock.unlock();"),
		regexp.QuoteMeta("RunCheck(generation);"))...)
	if unscoped.MatchString(worker) {
		problems = append(problems, "UpdateChecker::WorkerLoop changes the snapshot without a generation")
	}
	check := applyDefinition(checker, "void UpdateChecker::RunCheck(std::uint64_t generation) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunCheck", check,
		regexp.QuoteMeta("if (feedGeneration_ != generation) {"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta("snapshot_.newestCode = newestCode;"),
		regexp.QuoteMeta("offer_ = offer;"))...)
	if unscoped.MatchString(check) {
		problems = append(problems, "UpdateChecker::RunCheck changes the snapshot without its generation")
	}
	failed := applyDefinition(checker, "void UpdateChecker::CheckFailed(std::uint64_t generation) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::CheckFailed", failed,
		regexp.QuoteMeta("MutateFor(generation,"))...)
	apply := applyDefinition(checker, "void UpdateChecker::RunApply(std::uint64_t generation) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunApply", apply,
		regexp.QuoteMeta("if (!actionable || offer_.code == 0 || feedGeneration_ != generation) return;"),
		regexp.QuoteMeta("if (!MutateFor(generation, [](Snapshot& s) { s.stage = Stage::Installing; })) {"),
		regexp.QuoteMeta("abandoned(dir);"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta("LaunchUpdateHelper(installFolder_ / kHelperName, offer.tag, &helper, launchError)"),
		regexp.QuoteMeta("Mutate([&result](Snapshot& s) {"))...)
	if handOff := strings.Index(apply, "LaunchUpdateHelper("); handOff >= 0 && unscoped.MatchString(apply[:handOff]) {
		problems = append(problems,
			"UpdateChecker::RunApply changes the snapshot without its generation before the hand-off to the helper")
	}
	return problems
}

func TestUpdateApplyTrayFollowsTheFeed(t *testing.T) {
	reportProblems(t, checkTrayFollowsTheFeed(stripComments(readAppSource(t, "UpdateChecker.cpp"))))
}

// GitHub's answer to too many requests is honoured: a refused request's
// Retry-After or X-RateLimit-Reset holds every later request, manual or
// automatic, and the cadence never schedules one before it.
func checkTrayHonoursGitHub(checker string) []string {
	var problems []string
	fetch := applyDefinition(checker, "bool FetchUrl(std::wstring const& url, const wchar_t* accept,")
	problems = append(problems, applyOrderProblems("FetchUrl", fetch,
		regexp.QuoteMeta("if (status != 200) {"),
		regexp.QuoteMeta(`headers.retryAfterSeconds = NumericHeader(request.h, L"Retry-After", 0);`),
		regexp.QuoteMeta(`headers.rateLimitResetUnixSeconds = NumericHeader(request.h, L"X-RateLimit-Reset", 0);`),
		regexp.QuoteMeta(`headers.rateLimitExhausted = NumericHeader(request.h, L"X-RateLimit-Remaining", -1) == 0;`),
		regexp.QuoteMeta("return false;"))...)
	check := applyDefinition(checker, "void UpdateChecker::RunCheck(std::uint64_t generation) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunCheck", check,
		regexp.QuoteMeta("held = steady_clock::now() < holdUntil_;"),
		regexp.QuoteMeta("if (held) {"),
		regexp.QuoteMeta("CheckFailed(generation);"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta("FetchUrl("),
		regexp.QuoteMeta("if (!fetched) {"),
		regexp.QuoteMeta(".retryAfterSeconds = headers.retryAfterSeconds,"),
		regexp.QuoteMeta(".resetUnixSeconds = headers.rateLimitResetUnixSeconds,"),
		regexp.QuoteMeta(".exhausted = headers.rateLimitExhausted,"),
		regexp.QuoteMeta(".serverUnixSeconds = headers.serverUnixSeconds};"),
		regexp.QuoteMeta("const std::int64_t wait = update::NextCheckDelaySeconds(0, limit);"),
		regexp.QuoteMeta("holdUntil_ = steady_clock::now() + std::chrono::seconds(wait);"),
		regexp.QuoteMeta("CheckFailed(generation);"),
		regexp.QuoteMeta("return;"))...)
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
// screen's banner offers to try now, and the developer line says it too.
func checkTraySaysWhenChecksFail(checker, connect, window, developer string) []string {
	var problems []string
	start := applyDefinition(checker, "void UpdateChecker::Start() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::Start", start,
		regexp.QuoteMeta("const auto lastSuccess = prefs.find(kLastSuccessPrefKey);"),
		regexp.QuoteMeta("if (snapshot_.lastSuccessUnix <= 0) {"),
		regexp.QuoteMeta("snapshot_.lastSuccessUnix = NowUnixSeconds();"),
		regexp.QuoteMeta("SaveAppPref(kLastSuccessPrefKey, snapshot_.lastSuccessUnix);"),
		regexp.QuoteMeta("worker_ = std::thread("))...)
	check := applyDefinition(checker, "void UpdateChecker::RunCheck(std::uint64_t generation) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunCheck", check,
		regexp.QuoteMeta("update::ParseReleaseList(body);"),
		regexp.QuoteMeta("if (!parsed) {"),
		regexp.QuoteMeta("CheckFailed(generation);"),
		regexp.QuoteMeta("SaveAppPref(kLastSuccessPrefKey, succeeded);"),
		regexp.QuoteMeta("snapshot_.lastSuccessUnix = succeeded;"),
		regexp.QuoteMeta("snapshot_.checkStale = false;"))...)
	failed := applyDefinition(checker, "void UpdateChecker::CheckFailed(std::uint64_t generation) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::CheckFailed", failed,
		regexp.QuoteMeta("s.lastCheck = CheckOutcome::Failed;"),
		regexp.QuoteMeta("s.checkStale = update::CheckIsStale(now, s.lastSuccessUnix, autoCheck_ && version::kCode != 0);"))...)
	banner := applyDefinition(connect, "void ConnectPage::ApplyUpdateChecker(urnw::UpdateChecker::Snapshot const& snap) {")
	problems = append(problems, applyOrderProblems("ConnectPage::ApplyUpdateChecker", banner,
		regexp.QuoteMeta("if (snap.phase == Phase::None) {"),
		regexp.QuoteMeta("if (!snap.checkStale) {"),
		regexp.QuoteMeta("bar.IsOpen(false);"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta(`L"Couldn't check for updates since " +`),
		regexp.QuoteMeta("urnw::UpdateChecker::LocalDate(snap.lastSuccessUnix)"),
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
		{"a channel change that drops the helper's report", func() []string {
			return follows(replace(checker, "if (snapshot_.phase != Phase::Result) {\n      snapshot_ = Snapshot{",
				"if (true) {\n      snapshot_ = Snapshot{"))
		}},
		{"a generation-blind MutateFor", func() []string {
			return follows(replace(checker, "if (feedGeneration_ != generation) return false;", "if (false) return false;"))
		}},
		{"a check of the old feed published", func() []string {
			return follows(within("void UpdateChecker::RunCheck(std::uint64_t generation) {",
				"if (feedGeneration_ != generation) {", "if (false) {"))
		}},
		{"a check that changes the snapshot unscoped", func() []string {
			return follows(within("void UpdateChecker::RunCheck(std::uint64_t generation) {",
				"MutateFor(generation, [](Snapshot& s) { s.lastCheck = CheckOutcome::InFlight; });",
				"Mutate([](Snapshot& s) { s.lastCheck = CheckOutcome::InFlight; });"))
		}},
		{"the generation read after the worker unlocks", func() []string {
			return follows(replace(checker,
				"      applyRequested_ = false;\n      const std::uint64_t generation = feedGeneration_;\n      lock.unlock();\n",
				"      applyRequested_ = false;\n      lock.unlock();\n      const std::uint64_t generation = feedGeneration_;\n"))
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
			return follows(replace(checker, "Mutate([&result](Snapshot& s) {", "MutateFor(generation, [&result](Snapshot& s) {"))
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
		{"the hold never set", func() []string {
			return honours(replace(checker, "holdUntil_ = steady_clock::now() + std::chrono::seconds(wait);", "(void)wait;"))
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
		{"the banner silent when checks fail", func() []string {
			return checkTraySaysWhenChecksFail(checker, replace(connect, "if (!snap.checkStale) {", "if (true) {"),
				window, developer)
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
//   - not on a re-run of the installed package, and only when the installed
//     URnetwork.exe is new enough to quit on WM_CLOSE (an AppSearch above
//     2026.10.5.1 where INSTALLFOLDER puts it);
//   - Wix4CloseApplications moved before InstallValidate.
//
// After an update the helper ran, it starts the app again through the helper,
// unelevated, once its files are in place, and never on an uninstall or from
// the old product an upgrade removes; the helper ships as its own component.
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
		"Condition":    "URNETWORK_APP_CLOSABLE AND (NOT Installed OR REINSTALL OR REMOVE)",
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
	sequence := document.descendants(wixNamespace, "InstallExecuteSequence")
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
			`Condition="URNETWORK_APP_CLOSABLE AND (NOT Installed OR REINSTALL OR REMOVE)"`,
			`Condition="URNETWORK_APP_CLOSABLE"`},
		{"an app without the WM_CLOSE handler closed",
			`Condition="URNETWORK_APP_CLOSABLE AND (NOT Installed OR REINSTALL OR REMOVE)"`,
			`Condition="NOT Installed OR REINSTALL OR REMOVE"`},
		{"the gate on any app", `<FileSearch Name="URnetwork.exe" MinVersion="2026.10.5.1" />`,
			`<FileSearch Name="URnetwork.exe" />`},
		{"the close in WiX's slot", `Before="InstallValidate"`, `Before="InstallFiles"`},
		{"the relaunch elevated", `DllEntry="WixUnelevatedShellExec"`, `DllEntry="WixShellExec"`},
		{"the relaunch refused by the marker", `Value="[#UpdaterExe]"`, `Value="[#URnetworkExe]"`},
		{"a relaunch on a manual install", `Condition='UPDATE_RELAUNCH = "1" AND NOT (REMOVE ~= "ALL") AND NOT UPGRADINGPRODUCTCODE'`,
			`Condition='NOT (REMOVE ~= "ALL") AND NOT UPGRADINGPRODUCTCODE'`},
		{"a relaunch from the removed product", `Condition='UPDATE_RELAUNCH = "1" AND NOT (REMOVE ~= "ALL") AND NOT UPGRADINGPRODUCTCODE'`,
			`Condition='UPDATE_RELAUNCH = "1" AND NOT (REMOVE ~= "ALL")'`},
		{"a failed relaunch failing the install", `Return="ignore" />`, `Return="check" />`},
		{"the helper harvested twice", "        <Exclude Files=\"$(var.BinDir)\\URnetworkUpdate.exe\" />\n", ""},
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
