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

// Earnings › Top 200 › Sign fleet binding (support inbox 7934; factury
// support/plans/top200-in-app-signing.md). A provider could not produce the
// device signature the ur.io Top 200 page asks for: the SDK's
// SignSnFleetBinding had no caller in the app. The decisions are
// App/Top200Binding.h, run as a C++ spec (app/tools/top200-sign-tests.cpp)
// against sn's fleet_sign_test.go vector; the sheet, the card's button and the
// urnetwork://top200/sign link cannot be built off Windows, so what they must
// keep doing is checked on their source. Skips without nlohmann/json, as the
// other readers of JSON do.

// Compile the spec against App/Top200Binding.h. `mutate`, when set, rewrites a
// copy of the header for a negative control.
func top200SignTestProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("top 200 sign tests require a C++20 compiler: ", err)
	}
	jsonFlags := provideJsonFlags(t)
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "Top200Binding.h"))
	if err != nil {
		t.Fatal(err)
	}
	content := lf(string(source))
	if mutate != nil {
		changed := mutate(content)
		if changed == content {
			t.Fatal("negative control did not change the production Top200Binding.h")
		}
		content = changed
	}
	if err := os.WriteFile(filepath.Join(includeDir, "Top200Binding.h"), []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	program := filepath.Join(includeDir, "top200-sign-tests")
	args := []string{"-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + includeDir}
	args = append(args, jsonFlags...)
	args = append(args, filepath.Join(root, "app", "tools", "top200-sign-tests.cpp"), "-o", program)
	if output, err := exec.Command(compiler, args...).CombinedOutput(); err != nil {
		t.Fatalf("build top 200 sign tests: %v\n%s", err, output)
	}
	return program
}

func TestTop200SignSpec(t *testing.T) {
	project := readAppSource(t, "App.vcxproj")
	for _, want := range []string{
		`<ClInclude Include="Top200Binding.h" />`,
		`<ClInclude Include="Top200SignSheet.h" />`,
		`<ClCompile Include="Top200SignSheet.cpp" />`,
	} {
		if !strings.Contains(project, want) {
			t.Errorf("App.vcxproj does not list %s", want)
		}
	}
	program := top200SignTestProgram(t, nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("top 200 sign: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Each defect below is a looser check or a different payload. Put it back, and
// the spec must fail naming the case it breaks.
func TestTop200SignSpecDetectsEachDefectPutBack(t *testing.T) {
	cases := []struct {
		name string
		old  string
		new  string
		want string
	}{
		{
			// the SDK compares only the client id: without this the app signs a
			// binding naming another key, into a signature that never verifies
			name: "the client key is not compared",
			old:  "      detail::Lower(binding.clientKey) != detail::Lower(deviceClientKeyHex)) {",
			new:  "      false) {",
			want: "another client key",
		},
		{
			name: "any object with the 11 fields is a binding",
			old:  "  if (document.size() != 11) return check;\n",
			new:  "",
			want: "a twelfth field",
		},
		{
			name: "a field named twice is read as its last value",
			old:  "|| duplicate) return check;",
			new:  ") return check;",
			want: "a field named twice",
		},
		{
			name: "fields are found whatever their case",
			old:  "    const auto it = document.find(name);\n    return it == document.end() ? nullptr : &*it;\n",
			new:  "    for (auto it = document.begin(); it != document.end(); ++it) {\n      if (detail::Lower(it.key()) == name) return &*it;\n    }\n    return nullptr;\n",
			want: "a field spelled in another case",
		},
		{
			name: "text before the object is skipped",
			old:  "  if (check.trimmed.front() != '{') return check;\n",
			new:  "",
			want: "a UTF-8 byte order mark before the object",
		},
		{
			name: "the epochs are not ordered",
			old:  " ||\n        *toValue < *fromValue) {",
			new:  ") {",
			want: "valid_to_epoch before valid_from_epoch",
		},
		{
			name: "the payload re-serializes the binding",
			old:  "  payload += trimmedBinding;\n",
			new:  "  payload += nlohmann::json::parse(trimmedBinding).dump();\n",
			want: "keeps the binding's own spacing",
		},
		{
			name: "the signer runs for any binding",
			old:  "  if (check.verdict != Verdict::Valid) return result;\n",
			new:  "",
			want: "never called",
		},
		{
			name: "a link's binding has no length limit",
			old:  "encoded->size() > kMaxLinkBindingLength",
			new:  "false",
			want: "over 4096 characters",
		},
		{
			name: "a link's binding need not be UTF-8",
			old:  "!decoded || !detail::IsUtf8(*decoded)",
			new:  "!decoded",
			want: "not UTF-8",
		},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			t.Parallel()
			program := top200SignTestProgram(t, func(source string) string {
				if strings.Count(source, c.old) != 1 {
					t.Fatalf("negative control expects exactly one %q in Top200Binding.h", c.old)
				}
				return strings.Replace(source, c.old, c.new, 1)
			})
			output, err := exec.Command(program).CombinedOutput()
			if err == nil || !strings.Contains(string(output), c.want) {
				t.Fatalf("negative control was not detected (want %q): %v\n%s", c.want, err, output)
			}
		})
	}
}

// The button, the link and the sheet, on their source.
func TestTop200SignWiring(t *testing.T) {
	// the button sits in the Top 200 card, which shows for a network projected
	// inside the top 200 and a claimed one alike. Owner decision 2026-10-29
	// (LEADERTOP Q16): it shows in both while fleet binding is open and stays
	// hidden while it is closed, so ApplyTop200 writes it from the tile, and
	// the tile (ProtocolBoardPresentation.h TileFor, run by
	// tools/protocol-board-tests.cpp) sets it only with binding open
	xaml := readAppSource(t, "MainWindow.xaml")
	card := xaml[strings.Index(xaml, `x:Name="Top200Card"`):]
	card = card[:strings.Index(card, `x:Name="UpgradeButton"`)]
	if !regexp.MustCompile(`<Button x:Name="Top200SignButton"[^>]*Click="OnSignTop200"`).MatchString(card) {
		t.Error("the Top 200 card has no Top200SignButton wired to OnSignTop200")
	}
	wallet := stripLineComments(readAppSource(t, "WalletPage.cpp"))
	if !strings.Contains(definitionBody(t, "WalletPage.cpp", wallet, "void WalletPage::ApplyTop200("),
		"w_.Top200SignButton().Visibility(tile.signButton ? Visibility::Visible : Visibility::Collapsed);") {
		t.Error("ApplyTop200 does not show the sign button from the tile: it shows in the claimable tile and the claimed card alike while binding is open")
	}
	tile := definitionBody(t, "ProtocolBoardPresentation.h",
		stripLineComments(readAppSource(t, "ProtocolBoardPresentation.h")), "inline Tile TileFor(")
	openAt, signAt := strings.Index(tile, "if (snapshot->bindingOpen) {"), strings.Index(tile, "tile.signButton = true;")
	if openAt < 0 || signAt < openAt {
		t.Error("TileFor offers Sign fleet binding outside the binding-open branch: nothing invites registering early")
	}
	if !strings.Contains(wallet, `w_.Top200SignButton().Content(LocBox("top200_sign_binding"));`) {
		t.Error("the sign button is not labelled top200_sign_binding")
	}
	show := definitionBody(t, "WalletPage.cpp", wallet, "winrt::fire_and_forget WalletPage::ShowTop200Sign(")
	for _, want := range []string{"top200SignSheet_->Prefill(prefill, linkInvalid);", "w_.sheetOpen()",
		"urnw::Top200SignSheet::Create(", "SetSheetOpen(false);"} {
		if !strings.Contains(show, want) {
			t.Errorf("WalletPage::ShowTop200Sign is missing %s", want)
		}
	}

	// the link: after the auth gate, before the sdk's handler; dropped while
	// signed out; it only pre-fills
	controller := stripLineComments(readAppSource(t, "AppController.cpp"))
	handle := definitionBody(t, "AppController.cpp", controller, "void AppController::HandleDeepLink(")
	auth := strings.Index(handle, "authlink::IsAuthHost(url)")
	route := strings.Index(handle, "top200sign::ParseSignLink(url).sign")
	sdk := strings.Index(handle, "sdk_.HandleDeepLink(url)")
	if auth < 0 || route < 0 || sdk < 0 || !(auth < route && route < sdk) {
		t.Error("AppController::HandleDeepLink must route urnetwork://top200/sign after the auth gate and before the sdk's handler")
	}
	if !strings.Contains(handle, "self->HandleTop200SignLink(url);") {
		t.Error("AppController::HandleDeepLink does not hand the sign link to the window")
	}
	window := stripLineComments(readAppSource(t, "MainWindow.xaml.cpp"))
	link := definitionBody(t, "MainWindow.xaml.cpp", window, "void MainWindow::HandleTop200SignLink(")
	if !strings.HasPrefix(strings.TrimSpace(link[strings.Index(link, "{")+1:]), "if (!Sdk().IsLoggedIn()) return;") {
		t.Error("MainWindow::HandleTop200SignLink must drop a link that arrives while signed out, first")
	}
	if !strings.Contains(link, "wallet_->ShowTop200Sign(") {
		t.Error("MainWindow::HandleTop200SignLink does not open the sheet")
	}
	header := readAppSource(t, "MainWindow.xaml.h")
	public := header[strings.Index(header, "struct MainWindow : MainWindowT<MainWindow>"):]
	if private := strings.Index(public, "\n private:"); private < 0 ||
		!strings.Contains(public[:private], "void HandleTop200SignLink(std::string const& url);") {
		t.Error("MainWindow::HandleTop200SignLink must be public because AppController invokes it")
	}

	// the sheet: the one place that signs or reads the client key, off the UI
	// thread, through the checked path, with nothing sent and nothing logged
	files := appSourceFiles(t, ".cpp", ".h")
	for _, name := range sortedNames(files) {
		code := stripLineComments(files[name])
		for _, call := range []string{"signSnFleetBinding(", "getSnClientKey("} {
			if strings.Contains(code, call) && name != "Top200SignSheet.cpp" {
				t.Errorf("%s calls %s: only the Top 200 sign sheet signs or reads the client key", name, call)
			}
		}
	}
	sheet := stripLineComments(readAppSource(t, "Top200SignSheet.cpp"))
	sign := definitionBody(t, "Top200SignSheet.cpp", sheet, "winrt::fire_and_forget Top200SignSheet::Sign() {")
	background := strings.Index(sign, "co_await winrt::resume_background();")
	checked := strings.Index(sign, "top200sign::SignChecked(check,")
	call := strings.Index(sign, "sdk.device().signSnFleetBinding(binding)")
	validated := strings.Index(sign, "if (check.verdict != top200sign::Verdict::Valid) co_return;")
	if validated < 0 || background < 0 || checked < 0 || call < 0 || !(validated < background && background < checked && checked < call) {
		t.Error("Top200SignSheet::Sign must refuse a binding that is not Valid, then sign off the UI thread through top200sign::SignChecked")
	}
	apply := definitionBody(t, "Top200SignSheet.cpp", sheet, "void Top200SignSheet::ApplyState() {")
	if !strings.Contains(apply, "hotkeyValue_.Text(H(check.binding->hotkey));") || strings.Contains(apply, "ShowMono(hotkeyValue_") {
		t.Error("the review must show the hotkey in full: the user compares all 64 hex digits with the page")
	}
	load := definitionBody(t, "Top200SignSheet.cpp", sheet, "winrt::fire_and_forget Top200SignSheet::LoadDevice() {")
	if b, k := strings.Index(load, "co_await winrt::resume_background();"), strings.Index(load, "getSnClientKey()"); b < 0 || k < 0 || b > k {
		t.Error("Top200SignSheet::LoadDevice must read the client key (an rpc into the service) off the UI thread")
	}
	for _, network := range []string{"Sdk().api()", "sdk.api()", "http", "OpenUrl("} {
		if strings.Contains(sheet, network) {
			t.Errorf("Top200SignSheet.cpp names %s: the sheet sends nothing over the network", network)
		}
	}
	logCall := regexp.MustCompile(`Log(?:Info|Warn|Error)\("[^"]*"([^;]*)\);`)
	for _, match := range logCall.FindAllStringSubmatch(sheet, -1) {
		for _, secret := range []string{"payload", "binding", "Text()", "trimmed", "clientKey", "detail", "text"} {
			if strings.Contains(match[1], secret) {
				t.Errorf("Top200SignSheet.cpp logs %s (%s): never the binding, the payload or the key", secret, match[0])
			}
		}
	}
}
