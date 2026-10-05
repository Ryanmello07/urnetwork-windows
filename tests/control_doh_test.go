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

// Compile and execute the extender UI spec (app/tools/extender-tests.cpp) with
// the app's own ExtenderPresentation.cpp and the vendored QR encoder: the ring
// geometry, the status mapping, the settings form, the bootstrap
// DNS-over-HTTPS servers box (its lines and its error keys), the share code,
// the import decisions with their servers line, the provider extender row and
// the statistics sections. Built with the documented line; it reads the en
// store relative to app/tools, so it runs there.
func TestExtenderPresentation(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("extender tests require a C++20 compiler: ", err)
	}
	appDir := filepath.Join(repositoryRoot(t), "app")
	toolsDir := filepath.Join(appDir, "tools")
	program := filepath.Join(t.TempDir(), "extender-tests")
	build := exec.Command(compiler, "-std=c++20",
		"-I"+filepath.Join(appDir, "src", "App"),
		"-I"+filepath.Join(appDir, "third_party", "qrcodegen"),
		filepath.Join(toolsDir, "extender-tests.cpp"),
		filepath.Join(appDir, "src", "App", "ExtenderPresentation.cpp"),
		filepath.Join(appDir, "third_party", "qrcodegen", "qrcodegen.cpp"),
		"-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build extender tests: %v\n%s", err, output)
	}
	run := exec.Command(program)
	run.Dir = toolsDir
	if output, err := run.CombinedOutput(); err != nil {
		t.Fatalf("extender presentation: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The block, its two doors and SdkHost's half cannot be built off Windows, so
// what they must keep doing is checked on their source, as the VLESS sheet's
// are: every string the block shows exists, Account > Extenders hosts it
// whether or not there is a session and the login screen's network sheet opens
// it, every sdk call runs off the UI thread through SdkHost, a save goes
// through the space's own setter, and the import sheet names the servers a
// code would set.
func TestControlDohSettingsWiring(t *testing.T) {
	root := repositoryRoot(t)
	document := parseXML(t, filepath.Join(root, "app", "src", "App", "Strings", "en", "Resources.resw"))
	names := map[string]bool{}
	for _, node := range document.descendants("", "data") {
		if name, ok := node.attribute("name"); ok {
			names[name] = true
		}
	}

	block := stripLineComments(readAppSource(t, "ControlDohSettings.cpp"))
	keyPattern := regexp.MustCompile(`\bLoc\("([a-z0-9_]+)"\)|MakeAction\(root_, "([a-z0-9_]+)"`)
	used := map[string]bool{}
	for _, match := range keyPattern.FindAllStringSubmatch(block, -1) {
		key := match[1] + match[2]
		used[key] = true
		if !names[key] {
			t.Errorf("ControlDohSettings.cpp looks up %q, which en/Resources.resw does not define", key)
		}
	}
	for _, key := range []string{
		"control_doh_urls", "control_doh_urls_description", "control_doh_urls_hint",
		"control_doh_use_china", "control_doh_use_china_hint", "control_doh_urls_reset", "save",
		"control_doh_urls_next_connect", "loading", "something_went_wrong",
	} {
		if !used[key] {
			t.Errorf("the bootstrap DNS-over-HTTPS block does not show %q", key)
		}
	}

	// The decisions are ExtenderPresentation.h's, not the block's.
	for _, want := range []string{
		"ParseControlDohLines(winrt::to_string(self->urlsBox_.Text()))",
		"ControlDohText(",
		"ControlDohSaveOutcomeFor(*errorId)",
		"kControlDohChinaCountryCode",
	} {
		if !strings.Contains(block, want) {
			t.Errorf("ControlDohSettings.cpp: missing %s", want)
		}
	}
	// Every sdk call goes through SdkHost after the hop off the UI thread; the
	// host is reached through a pointer taken before it, never through the
	// block's member after it.
	if strings.Contains(block, "sdk_.") {
		t.Error("ControlDohSettings.cpp calls the host through its member; take the pointer before the hop")
	}
	for name, call := range map[string]string{
		"winrt::fire_and_forget ControlDohBlock::Load(":              "sdk->CurrentControlDohUrls()",
		"winrt::fire_and_forget ControlDohBlock::UseChinaResolvers(": "sdk->RegionalControlDohUrls(kControlDohChinaCountryCode)",
		"winrt::fire_and_forget ControlDohBlock::Save(":              "sdk->SetControlDohUrls(urls)",
	} {
		body := functionBody(block, name)
		hop := strings.Index(body, "co_await winrt::resume_background();")
		at := strings.Index(body, call)
		if hop < 0 || at < 0 || at < hop {
			t.Errorf("%s: %s must run after co_await winrt::resume_background()", name, call)
		}
	}
	// Only "" is a save (ControlDohSaveOutcomeFor): any id, the sdk's
	// internal_error included, saved nothing and says its message.
	save := functionBody(block, "winrt::fire_and_forget ControlDohBlock::Save(")
	for _, want := range []string{
		"const ControlDohSaveOutcome outcome = ControlDohSaveOutcomeFor(*errorId);",
		"if (!outcome.saved) {",
		"self->ShowStatus(Loc(outcome.messageKey), kit::ValidationState::Invalid);",
		"self->ShowStatus(Loc(outcome.messageKey), kit::ValidationState::Valid);",
		"self->nextConnectText_.Visibility(Visibility::Visible)",
		"self->Load();",
	} {
		if !strings.Contains(save, want) {
			t.Errorf("ControlDohBlock::Save: a save must read back and say %s", want)
		}
	}
	if strings.Contains(save, "errorId->empty()") {
		t.Error("ControlDohBlock::Save decides a save on the id itself; only ControlDohSaveOutcomeFor says what is saved")
	}
	// "Use China resolvers" fills the box and saves nothing; "Use built-in
	// servers only" clears it and saves the empty list at once.
	china := functionBody(block, "winrt::fire_and_forget ControlDohBlock::UseChinaResolvers(")
	if strings.Contains(china, "SetControlDohUrls") || strings.Contains(china, "Save(") ||
		!strings.Contains(china, "self->urlsBox_.Text(H(ControlDohText(preset)));") {
		t.Error("ControlDohBlock::UseChinaResolvers must fill the box with the sdk's preset and leave the save to the user")
	}
	build := functionBody(block, "void ControlDohBlock::Build(")
	reset := strings.Index(build, `MakeAction(root_, "control_doh_urls_reset"`)
	if reset < 0 || !strings.Contains(build[reset:], "self->urlsBox_.Text(hstring{});") ||
		!strings.Contains(build[reset:], "self->Save({});") {
		t.Error("the reset button must clear the box and save an empty list")
	}

	// Door one: Account > Extenders, under the extender settings, read whether
	// or not the view controller is there.
	account := stripLineComments(readAppSource(t, "AccountPage.cpp"))
	pane := functionBody(account, "void AccountPage::BuildExtenderPane(")
	for _, want := range []string{
		`header("control_doh_urls");`,
		"controlDoh_ = ControlDohBlock::Create(Sdk());",
		"host.Children().Append(controlDoh_->Root());",
	} {
		if !strings.Contains(pane, want) {
			t.Errorf("AccountPage::BuildExtenderPane: missing %s", want)
		}
	}
	load := functionBody(account, "winrt::fire_and_forget AccountPage::LoadExtenderSettings(")
	loaded := strings.Index(load, "controlDoh_->Load();")
	gate := strings.Index(load, "if (!controller)")
	if loaded < 0 || gate < 0 || gate < loaded {
		t.Error("AccountPage::LoadExtenderSettings must load the servers ahead of the view controller's gate: they need no session")
	}
	if !strings.Contains(functionBody(account, "void AccountPage::ApplyExtenderStrings("), "controlDoh_->ApplyStrings();") {
		t.Error("AccountPage::ApplyExtenderStrings does not re-text the servers block")
	}
	if strings.Contains(functionBody(account, "void AccountPage::ApplyExtenderForm("), "controlDoh_") {
		t.Error("AccountPage::ApplyExtenderForm gates the servers block on the view controller")
	}

	// Door two: the login screen's network sheet, which closes for it.
	auth := stripLineComments(readAppSource(t, "AuthSheets.cpp"))
	network := functionBody(auth, "void NetworkServerSheet::Build(")
	for _, want := range []string{`LocBox("control_doh_urls")`, "self->controlDohRequested_ = true;", "self->dialog_.Hide();"} {
		if !strings.Contains(network, want) {
			t.Errorf("NetworkServerSheet::Build: the bootstrap DNS-over-HTTPS button is missing %s", want)
		}
	}
	login := stripLineComments(readAppSource(t, "LoginPage.cpp"))
	change := functionBody(login, "winrt::fire_and_forget LoginPage::OnChangeNetworkServer(")
	asked := strings.Index(change, "networkServerSheet_->ControlDohRequested()")
	opened := strings.Index(change, "urnw::ControlDohSheet::Create(")
	if asked < 0 || opened < 0 || opened < asked {
		t.Error("LoginPage::OnChangeNetworkServer does not open the servers sheet after the network sheet asks for it")
	}

	// SdkHost: the wrappers and the in-place setter. Both whole-values writers
	// start from what the space stores, so they keep the servers
	// (TestNetworkSpaceWritersStartFromTheStoredValues, TestNetworkSpaceStartup).
	host := stripLineComments(readAppSource(t, "SdkHost.cpp"))
	for _, want := range []string{
		"networkSpace_->getControlDohUrls()",
		"networkSpace_->setControlDohUrls(urnet::StringList(urls))",
		"urnet::regionalControlDohUrls(countryCode)",
	} {
		if !strings.Contains(host, want) {
			t.Errorf("SdkHost.cpp: missing %s", want)
		}
	}
	if strings.Contains(functionBody(host, "std::optional<std::string> SdkHost::SetControlDohUrls("), "updateNetworkSpaceValues") {
		t.Error("SdkHost::SetControlDohUrls writes the values whole; the space's setter validates the list and applies it in place")
	}

	// The import sheet: the decode carries the servers, and the line beside the
	// settings toggle names them.
	sheets := stripLineComments(readAppSource(t, "ExtenderSheets.cpp"))
	if !strings.Contains(functionBody(sheets, "void ExtenderImportSheet::Decode("),
		"decoded_.controlDohUrls = *result->ControlDohUrls;") {
		t.Error("ExtenderImportSheet::Decode drops the servers of the code's settings")
	}
	apply := functionBody(sheets, "void ExtenderImportSheet::ApplyDecision(")
	if !strings.Contains(apply, `urnw::Format("import_extenders_control_doh_urls"`) ||
		!strings.Contains(apply, "urnw::Widen(decision.controlDohUrlsArg)") {
		t.Error("ExtenderImportSheet::ApplyDecision does not name the servers a code's settings would set")
	}

	// The project builds the block.
	project := parseXML(t, filepath.Join(root, "app", "src", "App", "App.vcxproj"))
	compiled := false
	for _, node := range project.descendants(msbuildNamespace, "ClCompile") {
		if include, ok := node.attribute("Include"); ok && include == "ControlDohSettings.cpp" {
			compiled = true
		}
	}
	listed := false
	for _, node := range project.descendants(msbuildNamespace, "ClInclude") {
		if include, ok := node.attribute("Include"); ok && include == "ControlDohSettings.h" {
			listed = true
		}
	}
	if !compiled || !listed {
		t.Errorf("App.vcxproj does not build the servers block (compiles %t, lists the header %t)", compiled, listed)
	}
}

// The mirrored error ids are the SDK's own, and the generated header carries
// everything SdkHost calls and the server lists a space's values keep. The
// header is git-ignored (fetch-deps unpacks it into
// app/third_party/urnetwork-sdk/<arch>; URNETWORK_SDK_INCLUDE names another
// directory), so a host without one skips this, and so does a stale copy from
// before the bootstrap DoH servers.
func TestControlDohMatchesTheSdkHeader(t *testing.T) {
	root := repositoryRoot(t)
	dir := controlDohSdkHeaderDir(t, root)
	if dir == "" {
		t.Skip("no urnetwork_sdk.hpp with the bootstrap DoH servers (set URNETWORK_SDK_INCLUDE)")
	}
	data, err := os.ReadFile(filepath.Join(dir, "urnetwork_sdk.hpp"))
	if err != nil {
		t.Fatal(err)
	}
	header := string(data)
	presentation := readAppSource(t, "ExtenderPresentation.h")
	for name, id := range map[string]string{
		"ControlDohErrorUrlInvalid":    "control_doh_error_url_invalid",
		"ControlDohErrorHttpsRequired": "control_doh_error_https_required",
		"ControlDohErrorIpRequired":    "control_doh_error_ip_required",
		"ControlDohErrorTooMany":       "control_doh_error_too_many",
	} {
		if !strings.Contains(header, `inline constexpr const char* `+name+` = "`+id+`";`) {
			t.Errorf("urnetwork_sdk.hpp: urnet::%s is not %q", name, id)
		}
		if !strings.Contains(presentation, `inline constexpr const char* k`+name+` = "`+id+`";`) {
			t.Errorf("ExtenderPresentation.h: k%s does not mirror the sdk's %q", name, id)
		}
	}
	for _, want := range []string{
		"std::optional<StringList> getControlDohUrls() const;",
		"std::string setControlDohUrls(const std::optional<StringList>& doh_urls) const;",
		"inline std::optional<StringList> regionalControlDohUrls(const std::string& country_code)",
		"std::optional<std::vector<std::string>> control_doh_urls_ipv4;",
		"std::optional<std::vector<std::string>> control_doh_urls_ipv6;",
		"std::optional<StringList> ControlDohUrls;",
	} {
		if !strings.Contains(header, want) {
			t.Errorf("urnetwork_sdk.hpp: missing %s", want)
		}
	}
	t.Logf("against %s", filepath.Join(dir, "urnetwork_sdk.hpp"))
}

// The directory of a urnetwork_sdk.hpp that has the bootstrap DoH servers, or "".
func controlDohSdkHeaderDir(t *testing.T, root string) string {
	t.Helper()
	explicit := os.Getenv("URNETWORK_SDK_INCLUDE")
	candidates := []string{}
	if explicit != "" {
		candidates = append(candidates, explicit)
	}
	for _, arch := range []string{"amd64", "arm64"} {
		candidates = append(candidates, filepath.Join(root, "app", "third_party", "urnetwork-sdk", arch))
	}
	for _, dir := range candidates {
		header, err := os.ReadFile(filepath.Join(dir, "urnetwork_sdk.hpp"))
		if err != nil {
			if dir == explicit {
				t.Fatalf("URNETWORK_SDK_INCLUDE=%s: %v", explicit, err)
			}
			continue
		}
		if !strings.Contains(string(header), "setControlDohUrls(") {
			if dir == explicit {
				t.Fatalf("URNETWORK_SDK_INCLUDE=%s: urnetwork_sdk.hpp has no bootstrap DoH servers", explicit)
			}
			t.Logf("%s: urnetwork_sdk.hpp predates the bootstrap DoH servers", dir)
			continue
		}
		return dir
	}
	return ""
}
