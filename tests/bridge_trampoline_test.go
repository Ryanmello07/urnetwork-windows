// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"encoding/base64"
	"encoding/json"
	"fmt"
	"maps"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
)

// The wallet bridge's trampoline (App/BridgeTrampoline.h, v2/CONSENT.md §10):
// a bridge url longer than ShellExecuteW passes on is opened through a page of
// our own. The C++ spec with negative controls that rewrite a copy of the
// header, the payout consent's own bridge url measured as the SDK builds it,
// and WalletConnect's wiring with a negative control for each assertion.

// Builds the trampoline spec against a copy of BridgeTrampoline.h, rewritten
// by `mutation` when it is set.
func buildBridgeTrampolineTests(t *testing.T, mutation func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("bridge trampoline tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	header, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "BridgeTrampoline.h"))
	if err != nil {
		t.Fatal(err)
	}
	content := string(header)
	if mutation != nil {
		content = mutation(content)
		if content == string(header) {
			t.Fatal("negative control did not change BridgeTrampoline.h")
		}
	}
	if err := os.WriteFile(filepath.Join(includeDir, "BridgeTrampoline.h"), []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	program := filepath.Join(includeDir, "bridge-trampoline-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I"+includeDir,
		filepath.Join(root, "app", "tools", "bridge-trampoline-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build bridge trampoline tests: %v\n%s", err, output)
	}
	return program
}

// Compile and execute the trampoline spec: a url within the limit opens as it
// is, a longer one through the page, the page holds the url verbatim and
// nothing else, and only an https url of url characters gets one.
func TestBridgeTrampoline(t *testing.T) {
	if output, err := exec.Command(buildBridgeTrampolineTests(t, nil)).CombinedOutput(); err != nil {
		t.Fatalf("bridge trampoline: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Each decision the spec pins, put back wrong in a copy of the header, fails
// the spec in the case that names it.
func TestBridgeTrampolineCatchesEachRegression(t *testing.T) {
	for _, control := range []struct {
		name, old, new, want string
	}{
		{"the limit is ShellExecuteW's own", "kDirectUrlLimit = 2000;", "kDirectUrlLimit = 2083;",
			"a url within the limit opens as it is"},
		{"every url opens as it is", "{ return url.size() > kDirectUrlLimit; }", "{ return url.empty() && url.size() > kDirectUrlLimit; }",
			"a longer url goes through the page"},
		{"the page sends a referrer", `      "<meta name=\"referrer\" content=\"no-referrer\">\n"` + "\n", "",
			"the page replaces itself with the url, and holds nothing else"},
		{"a url of any scheme gets a page", `  if (!url.starts_with("https://")) return std::string();` + "\n", "",
			"only an https url is opened through the page"},
		{"any character gets into the page", "    if (!IsUrlCharacter(c)) return std::string();\n", "    if (c == '\\0') return std::string();\n",
			"nothing in the url can end the page's script"},
		{"a url character is refused", `"-._~:/?#[]@!$&'()*+,;=%"`, `"-._~:/?#%"`,
			"every url character passes, the fragment's and the query's"},
	} {
		t.Run(control.name, func(t *testing.T) {
			program := buildBridgeTrampolineTests(t, func(source string) string {
				if strings.Count(source, control.old) != 1 {
					t.Fatalf("BridgeTrampoline.h no longer has exactly one %q; update this control", control.old)
				}
				return strings.Replace(source, control.old, control.new, 1)
			})
			output, err := exec.Command(program).CombinedOutput()
			if err == nil || !strings.Contains(string(output), control.want) {
				t.Fatalf("negative control was not detected (want %q): %v\n%s", control.want, err, output)
			}
		})
	}
}

// The bridge url for `message`, as the SDK's BittensorWalletSession.BridgeUrl
// builds it: the query's keys sorted (net/url is the SDK's own encoder), and
// for the consent purpose the message in the fragment as base64url of its exact
// bytes (CONSENT.md §10), else in the query.
func sdkBridgeUrl(walletId, message, purpose, address string) string {
	values := url.Values{}
	values.Set("provider", "bittensor")
	values.Set("method", "signMessage")
	values.Set("wallet", walletId)
	values.Set("purpose", purpose)
	values.Set("redirect_link", "urnetwork://bittensor-sign-message")
	if address != "" {
		values.Set("address", address)
	}
	if walletId == "walletconnect" {
		values.Set("wc_project_id", "0123456789abcdef0123456789abcdef")
	}
	if purpose != "consent" {
		values.Set("message", message)
		return "https://ur.io/wallet-connect?" + values.Encode()
	}
	return "https://ur.io/wallet-connect?" + values.Encode() + "#message=" +
		base64.RawURLEncoding.EncodeToString([]byte(message))
}

// The review's W-2 (CONSENT.md §10): the payout consent's bridge url, for the
// sn protocol's own consent challenge (tests/testdata, synthetic identities),
// is longer than ShellExecuteW passes on (INTERNET_MAX_URL_LENGTH, 2,083) and
// than Chromium opens (2,048): cut there it would lose the parameters the page
// needs, and Talisman and WalletConnect could never answer. Every such url goes
// through the trampoline, and what reaches ShellExecuteW is the page's path; a
// login challenge's url still opens as it is.
func TestConsentBridgeUrlOpensThroughTheTrampoline(t *testing.T) {
	root := repositoryRoot(t)
	data, err := os.ReadFile(filepath.Join(root, "tests", "testdata", "network-consent-challenge-v1.json"))
	if err != nil {
		t.Fatal(err)
	}
	fixture := struct {
		ColdkeySs58 string `json:"coldkey_ss58"`
		Message     string `json:"message"`
	}{}
	if err := json.Unmarshal(data, &fixture); err != nil || fixture.Message == "" || fixture.ColdkeySs58 == "" {
		t.Fatalf("the consent fixture: %v", err)
	}
	match := regexp.MustCompile(`kDirectUrlLimit = (\d+);`).FindStringSubmatch(readAppSource(t, "BridgeTrampoline.h"))
	if match == nil {
		t.Fatal("BridgeTrampoline.h: no kDirectUrlLimit")
	}
	limit, _ := strconv.Atoi(match[1])
	if limit >= 2048 {
		t.Errorf("kDirectUrlLimit %d: not under Chromium's 2,048 external-open limit", limit)
	}

	login := sdkBridgeUrl("talisman", "Sign in to URnetwork\nChallenge: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\nTimestamp: 1791849600", "login", "")
	t.Logf("login bridge url: %d characters", len(login))
	if len(login) > limit {
		t.Errorf("a login bridge url (%d characters) no longer opens as it is", len(login))
	}
	t.Logf("consent message: %d bytes", len(fixture.Message))
	consent := []string{}
	for _, wallet := range []string{"talisman", "walletconnect"} {
		for _, address := range []string{fixture.ColdkeySs58, ""} {
			bridge := sdkBridgeUrl(wallet, fixture.Message, "consent", address)
			t.Logf("%s consent bridge url: %d characters", wallet, len(bridge))
			if len(bridge) <= 2083 {
				t.Errorf("%s: the consent bridge url is %d characters; the fixture no longer shows the limit", wallet, len(bridge))
			}
			consent = append(consent, bridge)
		}
	}
	// each one, with the message in the query as before the fragment, too
	consent = append(consent, sdkBridgeUrl("talisman", fixture.Message, "connect", fixture.ColdkeySs58))
	if output, err := exec.Command(buildBridgeTrampolineTests(t, nil), consent...).CombinedOutput(); err != nil {
		t.Fatalf("the consent's bridge urls: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
	for _, miss := range bridgeTrampolineWiringMisses(readAppSource(t, "WalletConnect.cpp"), readAppSource(t, "App.vcxproj")) {
		t.Error(miss)
	}
}

// Every way WalletConnect.cpp and App.vcxproj miss the trampoline's wiring, one
// line each. WalletConnect.cpp is Win32 and does not build here, so these read
// its source.
func bridgeTrampolineWiringMisses(walletConnect, project string) []string {
	misses := []string{}
	source := stripComments(walletConnect)
	require := func(where, body string, wants ...string) {
		for _, want := range wants {
			if !strings.Contains(body, want) {
				misses = append(misses, fmt.Sprintf("%s: missing %s", where, want))
			}
		}
	}
	requirePattern := func(where, body, pattern, what string) {
		if !regexp.MustCompile(pattern).MatchString(body) {
			misses = append(misses, fmt.Sprintf("%s: %s", where, what))
		}
	}
	// every url opens through OpenUrl, and a long one never reaches ShellExecuteW
	open := functionBody(source, "void WalletConnect::OpenUrl(")
	requirePattern("WalletConnect::OpenUrl", open,
		`^\{\s*if \(bridge::NeedsTrampoline\(url\)\) \{\s*OpenThroughTrampoline\(url\);\s*return;\s*\}\s*`+
			`std::wstring w = Widen\(url\);\s*HINSTANCE h = ShellExecuteW\(nullptr, L"open", w\.c_str\(\)`,
		"a url longer than ShellExecuteW passes on is opened as it is")
	require("WalletConnect::OpenBittensorBridge", functionBody(source, "void WalletConnect::OpenBittensorBridge("),
		"OpenUrl(bridgeUrl);")
	trampoline := functionBody(source, "void WalletConnect::OpenThroughTrampoline(")
	if trampoline == "" {
		return append(misses, "WalletConnect.cpp: no OpenThroughTrampoline")
	}
	// the page: only a plain https url's, in the user's own temp directory,
	// after the pages earlier flows left, named at random and created new,
	// for its owner alone
	requirePattern("WalletConnect::OpenThroughTrampoline", trampoline,
		`const std::string page = bridge::TrampolinePage\(url\);\s*if \(page\.empty\(\)\) return fail\(`,
		"a url that is not a plain https url gets a page")
	require("WalletConnect::OpenThroughTrampoline", trampoline,
		"::GetTempPathW(",
		"::FindFirstFileW((directory / bridge::kTrampolinePattern).c_str(), &found);",
		"::DeleteFileW((directory / found.cFileName).c_str());",
		"std::random_device random;",
		"directory / (std::wstring(bridge::kTrampolinePrefix) + name + L\".html\");",
		"kOwnerOnlyFileSddl, SDDL_REVISION_1, &security.lpSecurityDescriptor, nullptr)",
		"::CreateFileW(path.c_str(), GENERIC_WRITE, 0, &security, CREATE_NEW,")
	require("WalletConnect.cpp", source, `constexpr wchar_t kOwnerOnlyFileSddl[] = L"D:P(A;;FA;;;OW)";`)
	// what ShellExecuteW opens is the page's short path, never the url, and the
	// page goes once the browser has had time to read it
	requirePattern("WalletConnect::OpenThroughTrampoline", trampoline,
		`HINSTANCE h = ShellExecuteW\(nullptr, L"open", path\.c_str\(\), nullptr, nullptr, SW_SHOWNORMAL\);`,
		"ShellExecuteW does not open the page")
	if strings.Count(trampoline, "ShellExecuteW(") != 1 || strings.Contains(trampoline, "Widen(url)") {
		misses = append(misses, "WalletConnect::OpenThroughTrampoline: the url itself reaches ShellExecuteW")
	}
	requirePattern("WalletConnect::OpenThroughTrampoline", trampoline,
		`std::thread\(\[path\] \{\s*std::this_thread::sleep_for\(bridge::kTrampolineLifetime\);\s*::DeleteFileW\(path\.c_str\(\)\);\s*\}\)\.detach\(\);`,
		"the page is never deleted")
	requirePattern("WalletConnect::OpenThroughTrampoline", trampoline,
		`if \(!wrote\) \{\s*::DeleteFileW\(path\.c_str\(\)\);`, "a page that was not written stays")
	require("App.vcxproj", project, `<ClInclude Include="BridgeTrampoline.h" />`)
	return misses
}

// Each wiring assertion, with the change it pins taken out of a copy of the
// sources, reports exactly that.
func TestBridgeTrampolineWiringCatchesEachRegression(t *testing.T) {
	sources := map[string]string{
		"WalletConnect.cpp": readAppSource(t, "WalletConnect.cpp"),
		"App.vcxproj":       readAppSource(t, "App.vcxproj"),
	}
	for _, control := range []struct {
		name, file, old, new, want string
	}{
		{"a long url opens as it is", "WalletConnect.cpp", "  if (bridge::NeedsTrampoline(url)) {\n    OpenThroughTrampoline(url);\n    return;\n  }\n", "",
			"a url longer than ShellExecuteW passes on is opened as it is"},
		{"any url gets a page", "WalletConnect.cpp", "  if (page.empty()) return fail(\"not opening a long bridge url that is not a plain https url\");\n", "",
			"a url that is not a plain https url gets a page"},
		{"earlier pages stay", "WalletConnect.cpp", "      ::DeleteFileW((directory / found.cFileName).c_str());\n", "",
			"missing ::DeleteFileW((directory / found.cFileName).c_str());"},
		{"a page anyone may open", "WalletConnect.cpp", `L"D:P(A;;FA;;;OW)"`, `L"D:(A;;FA;;;WD)"`,
			`missing constexpr wchar_t kOwnerOnlyFileSddl[] = L"D:P(A;;FA;;;OW)";`},
		{"an existing file is opened", "WalletConnect.cpp", "&security, CREATE_NEW,", "&security, CREATE_ALWAYS,",
			"CREATE_NEW,"},
		{"a guessable name", "WalletConnect.cpp", "  std::random_device random;\n", "  auto random = [] { return 0u; };\n",
			"missing std::random_device random;"},
		{"the url reaches ShellExecuteW", "WalletConnect.cpp", "ShellExecuteW(nullptr, L\"open\", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);",
			"ShellExecuteW(nullptr, L\"open\", Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);", "the url itself reaches ShellExecuteW"},
		{"the page stays", "WalletConnect.cpp", "    ::DeleteFileW(path.c_str());\n  }).detach();", "  }).detach();",
			"the page is never deleted"},
		{"a page not written stays", "WalletConnect.cpp", "  if (!wrote) {\n    ::DeleteFileW(path.c_str());\n", "  if (!wrote) {\n",
			"a page that was not written stays"},
		{"the header is not in the project", "App.vcxproj", `<ClInclude Include="BridgeTrampoline.h" />`, "",
			`<ClInclude Include="BridgeTrampoline.h" />`},
	} {
		t.Run(control.name, func(t *testing.T) {
			mutated := maps.Clone(sources)
			if strings.Count(mutated[control.file], control.old) != 1 {
				t.Fatalf("%s no longer has exactly one %q; update this control", control.file, control.old)
			}
			mutated[control.file] = strings.Replace(mutated[control.file], control.old, control.new, 1)
			for _, miss := range bridgeTrampolineWiringMisses(mutated["WalletConnect.cpp"], mutated["App.vcxproj"]) {
				if strings.Contains(miss, control.want) {
					return
				}
			}
			t.Fatalf("negative control was not detected (want %q)", control.want)
		})
	}
}
