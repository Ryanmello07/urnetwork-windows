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

// buildVlessPresentationTests compiles the VLESS settings spec
// (app/tools/vless-presentation-tests.cpp) with the app's own
// VlessPresentation.cpp. extra adds compiler arguments ahead of the sources.
func buildVlessPresentationTests(t *testing.T, extra ...string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("vless presentation tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	appDir := filepath.Join(root, "app")
	program := filepath.Join(t.TempDir(), "vless-presentation-tests")
	arguments := []string{"-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I" + filepath.Join(appDir, "src", "App")}
	arguments = append(arguments, extra...)
	arguments = append(arguments,
		filepath.Join(appDir, "tools", "vless-presentation-tests.cpp"),
		filepath.Join(appDir, "src", "App", "VlessPresentation.cpp"),
		"-o", program)
	if output, err := exec.Command(compiler, arguments...).CombinedOutput(); err != nil {
		t.Fatalf("build vless presentation tests: %v\n%s", err, output)
	}
	return program
}

func runVlessPresentationTests(t *testing.T, program string) {
	t.Helper()
	appDir := filepath.Join(repositoryRoot(t), "app")
	if output, err := exec.Command(program, appDir).CombinedOutput(); err != nil {
		t.Fatalf("vless presentation: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Compile and execute the VLESS settings spec (App/VlessPresentation.h): the
// new form, the field visibility for every transport and security, the form <->
// VlessSettings round trip (spiderX carried, a hidden flow cleared), the store
// key of all 12 sdk error ids and of an unknown one, the picker options, and the
// space key comparison that reading a space's stored values rests on.
func TestVlessPresentation(t *testing.T) {
	runVlessPresentationTests(t, buildVlessPresentationTests(t))
}

// The same spec built against the generated SDK header, so the mapping and the
// stored-values reading are instantiated with urnet::VlessSettings,
// urnet::NetworkSpaceKey and urnet::NetworkSpaceValues, and a space export's
// stored values are read through the header's json conversions. The header is git-ignored (fetch-deps
// unpacks it into app/third_party/urnetwork-sdk/<arch>; URNETWORK_SDK_INCLUDE
// names another directory) and needs nlohmann/json, so a host without them
// skips this, and so does a stale copy from before the VLESS API.
func TestVlessPresentationAgainstSdkHeader(t *testing.T) {
	root := repositoryRoot(t)
	sdkDir := vlessSdkHeaderDir(t, root)
	if sdkDir == "" {
		t.Skip("no urnetwork_sdk.hpp with the VLESS API (set URNETWORK_SDK_INCLUDE)")
	}
	jsonDir, found := jsonIncludeDir(root)
	if !found {
		t.Skip("no nlohmann/json.hpp (set URNETWORK_JSON_INCLUDE)")
	}
	// System includes: the generated wrapper does not build with -Wextra -Werror.
	extra := []string{"-DURNW_VLESS_TESTS_SDK", "-isystem", sdkDir}
	if jsonDir != "" {
		extra = append(extra, "-isystem", jsonDir)
	}
	t.Logf("against %s", filepath.Join(sdkDir, "urnetwork_sdk.hpp"))
	runVlessPresentationTests(t, buildVlessPresentationTests(t, extra...))
}

// The directory of a urnetwork_sdk.hpp that has the VLESS API, or "".
func vlessSdkHeaderDir(t *testing.T, root string) string {
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
		if !strings.Contains(string(header), "struct VlessSettings {") {
			if dir == explicit {
				t.Fatalf("URNETWORK_SDK_INCLUDE=%s: urnetwork_sdk.hpp has no VLESS API", explicit)
			}
			t.Logf("%s: urnetwork_sdk.hpp predates the VLESS API", dir)
			continue
		}
		return dir
	}
	return ""
}

// The directory to add for nlohmann/json.hpp: "" when the compiler already
// searches it (adding /usr/include as a system directory breaks libc++'s
// include_next), and found=false when there is none.
func jsonIncludeDir(root string) (dir string, found bool) {
	candidates := []string{}
	if explicit := os.Getenv("URNETWORK_JSON_INCLUDE"); explicit != "" {
		candidates = append(candidates, explicit)
	}
	candidates = append(candidates,
		filepath.Join(root, "app", "third_party", "vendor-include"),
		"/opt/homebrew/include",
		"/usr/local/include",
		"/usr/include")
	for _, candidate := range candidates {
		if _, err := os.Stat(filepath.Join(candidate, "nlohmann", "json.hpp")); err != nil {
			continue
		}
		if candidate == "/usr/include" {
			return "", true
		}
		return candidate, true
	}
	return "", false
}

// The sheet, its two doors and SdkHost's half cannot be built off Windows, so
// what they must keep doing is checked on their source: every string the sheet
// shows exists, Settings and the login screen's network sheet open it, every
// sdk call it makes runs off the UI thread through SdkHost, and the space's
// stored values, its VLESS server among them, are read for its own key (the
// whole-values writers start from them: TestNetworkSpaceWritersStartFromTheStoredValues).
func TestVlessSettingsWiring(t *testing.T) {
	root := repositoryRoot(t)
	document := parseXML(t, filepath.Join(root, "app", "src", "App", "Strings", "en", "Resources.resw"))
	names := map[string]bool{}
	for _, node := range document.descendants("", "data") {
		if name, ok := node.attribute("name"); ok {
			names[name] = true
		}
	}

	sheet := stripLineComments(readAppSource(t, "VlessSheet.cpp"))
	keyPattern := regexp.MustCompile(`\bLoc(?:Box)?\("([a-z0-9_]+)"\)|Make(?:TextField|Picker)\(form, "([a-z0-9_]+)"\)`)
	used := map[string]bool{}
	for _, match := range keyPattern.FindAllStringSubmatch(sheet, -1) {
		key := match[1] + match[2]
		used[key] = true
		if !names[key] {
			t.Errorf("VlessSheet.cpp looks up %q, which en/Resources.resw does not define", key)
		}
	}
	for _, key := range []string{
		"vless", "vless_settings_description", "vless_enabled", "vless_link", "vless_link_hint",
		"vless_paste_link", "vless_copy_link", "vless_link_copied", "name_label",
		"vless_server_address", "vless_port", "vless_user_id", "transport", "vless_security",
		"vless_flow", "vless_server_name", "vless_fingerprint", "vless_alpn",
		"vless_allow_insecure", "vless_public_key", "vless_short_id", "vless_path",
		"vless_host_header", "save", "vless_settings_saved", "vless_settings_next_connect",
	} {
		if !used[key] {
			t.Errorf("the VLESS sheet does not show %q", key)
		}
	}

	// The decisions are VlessPresentation.h's, not the sheet's.
	for _, want := range []string{
		"vless::FormFrom(",
		"vless::SettingsFrom<urnet::VlessSettings>(ReadForm())",
		"vless::VisibilityFor(",
		"vless::ErrorKey(",
		"vless::OptionsWith(",
		"vless::LinkToParse(",
		"vless::IsPortInput(",
	} {
		if !strings.Contains(sheet, want) {
			t.Errorf("VlessSheet.cpp: missing %s", want)
		}
	}
	// Every sdk call goes through SdkHost after the hop off the UI thread; the
	// host is reached through a pointer taken before it, never through the
	// sheet's member after it.
	if strings.Contains(sheet, "sdk_.") {
		t.Error("VlessSheet.cpp calls the host through its member; take the pointer before the hop")
	}
	for name, call := range map[string]string{
		"winrt::fire_and_forget VlessSheet::Load(":      "sdk->CurrentVlessSettings()",
		"winrt::fire_and_forget VlessSheet::PasteLink(": "sdk->ParseVlessLink(link)",
		"winrt::fire_and_forget VlessSheet::CopyLink(":  "sdk->VlessSettingsLink(settings)",
		"winrt::fire_and_forget VlessSheet::Save(":      "sdk->SetVlessSettings(settings)",
	} {
		body := functionBody(sheet, name)
		hop := strings.Index(body, "co_await winrt::resume_background();")
		at := strings.Index(body, call)
		if hop < 0 || at < 0 || at < hop {
			t.Errorf("%s: %s must run after co_await winrt::resume_background()", name, call)
		}
	}
	save := functionBody(sheet, "winrt::fire_and_forget VlessSheet::Save(")
	for _, want := range []string{
		`Loc("vless_settings_saved")`,
		"nextConnectText_.Visibility(Visibility::Visible)",
		"self->Load();",
	} {
		if !strings.Contains(save, want) {
			t.Errorf("VlessSheet::Save: a save must read back and say %s", want)
		}
	}

	// Door one: Settings > Connections > VLESS.
	settings := stripLineComments(readAppSource(t, "SettingsPage.cpp"))
	connections := functionBody(settings, "void SettingsPage::BuildConnectionsSection(")
	if !strings.Contains(connections, `NavRow(card, Loc("vless"),`) || !strings.Contains(connections, "ShowVlessSheet();") {
		t.Error("Settings' Connections section has no VLESS row opening the VLESS sheet")
	}
	if !strings.Contains(functionBody(settings, "winrt::fire_and_forget SettingsPage::ShowVlessSheet("), "urnw::VlessSheet::Create(") {
		t.Error("SettingsPage::ShowVlessSheet does not open the VLESS sheet")
	}

	// Door two: the login screen's network sheet, which closes for it.
	auth := stripLineComments(readAppSource(t, "AuthSheets.cpp"))
	network := functionBody(auth, "void NetworkServerSheet::Build(")
	for _, want := range []string{`LocBox("vless")`, "self->vlessRequested_ = true;", "self->dialog_.Hide();"} {
		if !strings.Contains(network, want) {
			t.Errorf("NetworkServerSheet::Build: the VLESS button is missing %s", want)
		}
	}
	login := stripLineComments(readAppSource(t, "LoginPage.cpp"))
	change := functionBody(login, "winrt::fire_and_forget LoginPage::OnChangeNetworkServer(")
	asked := strings.Index(change, "networkServerSheet_->VlessRequested()")
	opened := strings.Index(change, "urnw::VlessSheet::Create(")
	if asked < 0 || opened < 0 || opened < asked {
		t.Error("LoginPage::OnChangeNetworkServer does not open the VLESS sheet after the network sheet asks for it")
	}

	// SdkHost: the wrappers, and the stored values the whole-values writers
	// start from.
	host := stripLineComments(readAppSource(t, "SdkHost.cpp"))
	for _, want := range []string{
		"networkSpace_->getVlessSettings()",
		"networkSpace_->setVlessSettings(settings)",
		"urnet::parseVlessLink(link)",
		"urnet::vlessSettingsLink(settings)",
		"urnet::validateVlessSettings(settings)",
	} {
		if !strings.Contains(host, want) {
			t.Errorf("SdkHost.cpp: missing %s", want)
		}
	}
	stored := functionBody(host, "std::optional<urnet::NetworkSpaceValues> SdkHost::StoredSpaceValuesLocked(")
	if !strings.Contains(stored, "spaceManager_->getNetworkSpace(key)") ||
		!strings.Contains(stored, "vless::StoredValuesFor<urnet::NetworkSpaceKey, urnet::NetworkSpaceValues>(") {
		t.Error("SdkHost::StoredSpaceValuesLocked does not read the space's stored values for its key")
	}

	// The project builds both units, the pure one without the pch.
	project := parseXML(t, filepath.Join(root, "app", "src", "App", "App.vcxproj"))
	compiles := map[string]*xmlNode{}
	for _, node := range project.descendants(msbuildNamespace, "ClCompile") {
		if include, ok := node.attribute("Include"); ok {
			compiles[include] = node
		}
	}
	if node := compiles["VlessPresentation.cpp"]; node == nil {
		t.Error("App.vcxproj does not compile VlessPresentation.cpp")
	} else if pch := node.child(msbuildNamespace, "PrecompiledHeader"); pch == nil || strings.TrimSpace(pch.Text) != "NotUsing" {
		t.Error("App.vcxproj compiles VlessPresentation.cpp with the pch; it has none, so the host tests can build it")
	}
	if compiles["VlessSheet.cpp"] == nil {
		t.Error("App.vcxproj does not compile VlessSheet.cpp")
	}
	headers := map[string]bool{}
	for _, node := range project.descendants(msbuildNamespace, "ClInclude") {
		if include, ok := node.attribute("Include"); ok {
			headers[include] = true
		}
	}
	for _, header := range []string{"VlessPresentation.h", "VlessSheet.h"} {
		if !headers[header] {
			t.Errorf("App.vcxproj does not list %s", header)
		}
	}
}
