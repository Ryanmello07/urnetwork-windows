// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// "Reset extenders" on Windows (connect EXTENDER.md E7): the Account page's
// extenders pane resets the app's own network space and hands the reset's id
// to the service (reset_extenders), which resets the space its session's
// device and its provider-only device run in. The pure parts run in
// app/tools/reset-extenders-tests.cpp against the production sources; the
// pane, SdkHost, ServiceClient and the service need Windows (and the app
// WinRT), so what they must keep doing is read off their sources, with
// comments stripped so prose cannot satisfy a contract.

// The sources the harness compiles, by their directory under app/src. They are
// copied into a fixture tree, so `mutate`, keyed by file name, can rewrite one
// for a negative control.
var resetExtendersSourceDirs = map[string]string{
	"Protocol.h":               "Common",
	"ProvideLifecycle.h":       "Common",
	"ExtenderPresentation.h":   "App",
	"ExtenderPresentation.cpp": "App",
	"ExtenderRingGeometry.h":   "App",
}

// Builds the harness against a fixture copy of the sources, `mutate` applied,
// and returns the program's path. `extra` adds compiler flags.
func resetExtendersTestProgram(t *testing.T, mutate map[string]func(string) string,
	extra ...string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("reset extenders tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	fixture := t.TempDir()
	for name, dir := range resetExtendersSourceDirs {
		source, err := os.ReadFile(filepath.Join(root, "app", "src", dir, name))
		if err != nil {
			t.Fatal(err)
		}
		content := string(source)
		if change := mutate[name]; change != nil {
			content = change(content)
			if content == string(source) {
				t.Fatalf("negative control did not change the production %s", name)
			}
		}
		if err := os.MkdirAll(filepath.Join(fixture, dir), 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(fixture, dir, name), []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(fixture, "reset-extenders-tests")
	args := []string{"-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror",
		"-I" + filepath.Join(fixture, "Common"), "-I" + filepath.Join(fixture, "App")}
	args = append(args, provideJsonFlags(t)...)
	args = append(args, extra...)
	args = append(args, filepath.Join(root, "app", "tools", "reset-extenders-tests.cpp"),
		filepath.Join(fixture, "App", "ExtenderPresentation.cpp"), "-o", program)
	if output, err := exec.Command(compiler, args...).CombinedOutput(); err != nil {
		t.Fatalf("build reset-extenders-tests.cpp: %v\n%s", err, output)
	}
	return program
}

// Execute the spec: the verb, its strict request, the reply and an older
// service's silence, the key from the app's space to the service's lookup, the
// form a reset leaves and when the pane's button is live.
func TestResetExtenders(t *testing.T) {
	program := resetExtendersTestProgram(t, nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("reset extenders: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The same spec on the generated header's own urnet::NetworkSpaceKey and
// urnet::ExtenderSettings, with the reset id read through its space values;
// and the header declares every call SdkHost and the service make for the
// reset. The header is git-ignored (fetch-deps unpacks it into
// app/third_party/urnetwork-sdk/<arch>; URNETWORK_SDK_INCLUDE names another
// directory), so a host without one skips this, and so does a copy from
// before the reset.
func TestResetExtendersAgainstSdkHeader(t *testing.T) {
	root := repositoryRoot(t)
	sdkDir := sdkHeaderDirWith(t, root, "applyExtenderReset(")
	if sdkDir == "" {
		t.Skip("no urnetwork_sdk.hpp with the extender reset (set URNETWORK_SDK_INCLUDE)")
	}
	data, err := os.ReadFile(filepath.Join(sdkDir, "urnetwork_sdk.hpp"))
	if err != nil {
		t.Fatal(err)
	}
	header := string(data)
	for _, want := range []string{
		"std::string resetExtenders() const;",
		"bool applyExtenderReset(const std::string& reset_id) const;",
		"std::optional<NetworkSpaceKey> getKey() const;",
		"NetworkSpace getNetworkSpace(const std::optional<NetworkSpaceKey>& key) const;",
		"std::optional<ExtenderSettings> getSettings() const;",
		"std::optional<std::string> extender_reset_id;",
	} {
		if !strings.Contains(header, want) {
			t.Errorf("urnetwork_sdk.hpp: missing %s", want)
		}
	}
	// System includes: the generated wrapper does not build with -Wextra -Werror.
	program := resetExtendersTestProgram(t, nil, "-DURNW_RESET_EXTENDERS_TESTS_SDK", "-isystem", sdkDir)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("reset extenders against the sdk header: %v\n%s", err, output)
	} else {
		t.Logf("against %s\n%s", filepath.Join(sdkDir, "urnetwork_sdk.hpp"), output)
	}
}

// One negative control: a rewrite of the production sources, and the failure
// line the harness must print for it.
type resetExtendersControl struct {
	name   string
	mutate map[string]func(string) string
	want   string
}

// Runs the harness against each control's rewritten sources and reports, by
// name, every control it does not fail on with the wanted line.
func requireResetExtendersFailures(t *testing.T, controls []resetExtendersControl) {
	t.Helper()
	for _, control := range controls {
		program := resetExtendersTestProgram(t, control.mutate)
		output, err := exec.Command(program).CombinedOutput()
		if err == nil || !strings.Contains(string(output), control.want) {
			t.Errorf("negative control %q was not detected (want %q): %v\n%s",
				control.name, control.want, err, output)
		}
	}
}

// A lenient request resets a space it does not name, a reply that loses
// `reset` hides what the service did, and a key or request rule that sends
// what the service must refuse, or names another space, resets the wrong one.
func TestResetExtendersRejectsAWrongWire(t *testing.T) {
	requireResetExtendersFailures(t, []resetExtendersControl{
		{
			name: "an empty field accepted",
			mutate: map[string]func(string) string{"Protocol.h": replaceOnce(
				"if (it == j.end() || !it->is_string() || it->get_ref<const std::string&>().empty()) {",
				"if (it == j.end() || !it->is_string()) {")},
			want: "wire: a request with host_name empty is refused",
		},
		{
			name: "a request without its env",
			mutate: map[string]func(string) string{"Protocol.h": replaceOnce(
				"       {\"env_name\", v.env_name},\n", "")},
			want: "wire: the request round-trips its key and its reset id",
		},
		{
			name: "the reply's reset not read",
			mutate: map[string]func(string) string{"Protocol.h": replaceOnce(
				"  get(\"reset\", v.reset);\n", "")},
			want: "reply: reset round-trips",
		},
		{
			name: "the reply's reset not written",
			mutate: map[string]func(string) string{"Protocol.h": replaceOnce(
				"  if (v.reset) j[\"reset\"] = true;\n", "")},
			want: "reply: reset round-trips",
		},
		{
			name: "a request sent without a reset id",
			mutate: map[string]func(string) string{"Protocol.h": replaceOnce(
				"      request.extender_reset_id.empty()) {", "      false) {")},
			want: "keys: no reset id sends nothing",
		},
		{
			name: "the env looked up as the host",
			mutate: map[string]func(string) string{"Protocol.h": replaceOnce(
				"  key.env_name = request.env_name;", "  key.env_name = request.host_name;")},
			want: "keys: the host and the env each go to their own field",
		},
	})
}

// A button live during a save lets a reset cross it, one live while signed out
// resets an account's pane nobody is in, and a settings reading that drops the
// hosts or a default flag reloads a form the reset did not leave.
func TestResetExtendersRejectsAWrongPane(t *testing.T) {
	requireResetExtendersFailures(t, []resetExtendersControl{
		{
			name: "live during a save",
			mutate: map[string]func(string) string{"ExtenderPresentation.cpp": replaceOnce(
				"return signedIn && !writing;", "(void)writing;\n  return signedIn;")},
			want: "button: off while a save or a reset of the pane runs",
		},
		{
			name: "live while signed out",
			mutate: map[string]func(string) string{"ExtenderPresentation.cpp": replaceOnce(
				"return signedIn && !writing;", "(void)signedIn;\n  return !writing;")},
			want: "button: off while signed out",
		},
		{
			name: "the hosts not read",
			mutate: map[string]func(string) string{"ExtenderPresentation.h": replaceOnce(
				"  if (settings.Hosts) view.hosts = *settings.Hosts;\n", "")},
			want: "settings: the manual hosts are read, in order",
		},
		{
			name: "the dns name's default flag not read",
			mutate: map[string]func(string) string{"ExtenderPresentation.h": replaceOnce(
				"  view.dnsNameDefault = settings.DnsNameDefault;\n", "")},
			want: "reset form: the three boxes are empty, which is the default",
		},
	})
}

// The innermost brace block of code around `at`, from its `{` through its
// matching `}`. String literals are blanked first, so a brace in a log format
// is not read as code; the sources read here put no brace in a character
// literal.
func enclosingBlock(t *testing.T, where, code string, at int) string {
	t.Helper()
	blanked := []byte(code)
	for i := 0; i < len(blanked); i++ {
		if blanked[i] != '"' {
			continue
		}
		for i++; i < len(blanked) && blanked[i] != '"' && blanked[i] != '\n'; i++ {
			if blanked[i] == '\\' && i+1 < len(blanked) {
				blanked[i] = ' '
				i++
			}
			blanked[i] = ' '
		}
	}
	start := -1
	for i, depth := at, 0; i >= 0 && start < 0; i-- {
		switch blanked[i] {
		case '}':
			depth++
		case '{':
			if depth == 0 {
				start = i
			} else {
				depth--
			}
		}
	}
	if start < 0 {
		t.Fatalf("%s: no block encloses offset %d; update this contract", where, at)
	}
	for i, depth := start, 0; i < len(blanked); i++ {
		switch blanked[i] {
		case '{':
			depth++
		case '}':
			depth--
			if depth == 0 {
				return code[start : i+1]
			}
		}
	}
	t.Fatalf("%s: the block at offset %d does not close; update this contract", where, start)
	return ""
}

// The block of `text` that holds the lock `lock` declares; fatal when the
// lock is missing.
func lockBlock(t *testing.T, where, text, lock string) string {
	t.Helper()
	at := strings.Index(text, lock)
	if at < 0 {
		t.Fatalf("%s no longer takes %q; update this contract", where, lock)
	}
	return enclosingBlock(t, where, text, at)
}

// The service answers the verb on the one control pipe and resets the space
// the manager holds for the request's key -- the one object the session's
// device and the provider-only device run in -- looking it up under the
// session lock in budget and resetting it with the lock released.
func TestResetExtendersServiceWiring(t *testing.T) {
	controller := stripComments(readServiceSource(t, "TunnelController.h"))
	provideRequire(t, "TunnelController.h", controller,
		"bool ResetExtenders(const proto::ResetExtenders& request, bool& reset, std::string& error);")

	server := stripComments(readServiceSource(t, "ControlServer.cpp"))
	// the one pipe's handler, so the pipe's access rule (kPipeSddl) is the verb's
	start := definitionBody(t, "ControlServer.cpp", server, "bool ControlServer::Start()")
	provideRequire(t, "ControlServer::Start", start, "return Handle(req);")
	handle := definitionBody(t, "ControlServer.cpp", server, "nlohmann::json ControlServer::Handle(")
	branch := sourceBetween(t, "ControlServer::Handle", handle,
		"type == proto::msg::kResetExtenders", "} else {")
	provideRequireOrder(t, "the reset_extenders branch", branch,
		"request.get<proto::ResetExtenders>()",
		"reply.ok = tunnel_.ResetExtenders(req, reset, error);")
	provideRequire(t, "the reset_extenders branch", branch, "reply.reset = reset;",
		"reply.error = error;")
	// the tunnel's status does not move
	requireNone(t, "the reset_extenders branch", branch, "PushState()")

	source := tunnelControllerSource(t)
	reset := definitionBody(t, "TunnelController.cpp", source,
		"bool TunnelController::ResetExtenders(const proto::ResetExtenders& request, bool& reset,")
	provideRequire(t, "TunnelController::ResetExtenders", reset,
		"proto::SpaceKeyOf<urnet::NetworkSpaceKey>(request)",
		"request.host_name", "request.env_name")
	locked := lockBlock(t, "TunnelController::ResetExtenders", reset,
		"std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);")
	// a wedged bring-up refuses it in budget rather than holding the pipe
	provideRequireOrder(t, "the session lock's block", locked,
		"lock.try_lock_for(kStopLockBudget)", "spaceManager_->getNetworkSpace(key)")
	refused := sourceBetween(t, "the session lock's block", locked,
		"lock.try_lock_for(kStopLockBudget)", "LogWarn(")
	provideRequire(t, "the refusal", refused, "error = \"a tunnel operation is in progress\";")
	provideRequire(t, "the session lock's block", locked,
		"if (spaceManager_) space = spaceManager_->getNetworkSpace(key);")
	// the reset joins the space's extender network client: never under the lock
	requireNone(t, "the session lock's block", locked, "applyExtenderReset")
	provideRequireOrder(t, "TunnelController::ResetExtenders", reset, locked,
		"reset = space.applyExtenderReset(request.extender_reset_id);")
	provideRequireOrder(t, "TunnelController::ResetExtenders", reset, "if (space) {",
		"reset = space.applyExtenderReset(request.extender_reset_id);")
	// one call through the manager's space covers both devices; a reset of one
	// device's slot would miss the other
	requireNone(t, "TunnelController::ResetExtenders", reset, "networkSpace_", "providerSpace_")
}

// The app resets its own space under the host's lock, then hands the id to
// the service outside it, and the pane confirms, resets off the UI thread and
// reloads the form before it says so.
func TestResetExtendersAppWiring(t *testing.T) {
	clientHeader := stripComments(readAppSource(t, "ServiceClient.h"))
	provideRequire(t, "ServiceClient.h", clientHeader,
		"bool ResetExtenders(const proto::ResetExtenders& request, bool* reset = nullptr,")
	client := stripComments(readAppSource(t, "ServiceClient.cpp"))
	send := definitionBody(t, "ServiceClient.cpp", client,
		"bool ServiceClient::ResetExtenders(const proto::ResetExtenders& request, bool* reset,")
	provideRequire(t, "ServiceClient::ResetExtenders", send,
		"nlohmann::json body = request;", "proto::msg::kResetExtenders", "*reset = r.ok && r.reset;",
		"return r.ok;")

	hostHeader := stripComments(readAppSource(t, "SdkHost.h"))
	provideRequire(t, "SdkHost.h", hostHeader, "bool ResetExtenders();")
	host := definitionBody(t, "SdkHost.cpp", sdkHostSource(t), "bool SdkHost::ResetExtenders()")
	locked := lockBlock(t, "SdkHost::ResetExtenders", host, "std::scoped_lock lock(mutex_);")
	provideRequireOrder(t, "SdkHost::ResetExtenders under mutex_", locked,
		"const std::string resetId = networkSpace_->resetExtenders();",
		"request = proto::ResetExtendersRequestFor(networkSpace_->getKey(), resetId);")
	// the pipe serializes calls, and this one can wait behind a start_tunnel
	requireNone(t, "SdkHost::ResetExtenders under mutex_", locked, "service_.")
	provideRequireOrder(t, "SdkHost::ResetExtenders", host, locked, "service_.IsConnected()")
	provideRequireOrder(t, "SdkHost::ResetExtenders", host, "service_.IsConnected()",
		"service_.ResetExtenders(*request, &reset, &error)")
	// the verb covers the provider-only device in place, and the device rpc is
	// not the path: no rebuild, no detour through the session's device
	requireNone(t, "SdkHost::ResetExtenders", host, "RequestProviderReconcile", "extenderVc_",
		"device_")

	pageHeader := stripComments(readAppSource(t, "AccountPage.h"))
	provideRequire(t, "AccountPage.h", pageHeader, "winrt::fire_and_forget ConfirmResetExtenders();",
		"winrt::fire_and_forget ResetExtenders();", "resetExtendersButton_{nullptr};")
	page := stripComments(readAppSource(t, "AccountPage.cpp"))
	build := definitionBody(t, "AccountPage.cpp", page, "void AccountPage::BuildExtenderPane()")
	provideRequireOrder(t, "BuildExtenderPane", build, "action(host, \"import_extenders\",",
		"resetExtendersButton_ = action(host, \"reset_extenders\",")
	provideRequire(t, "BuildExtenderPane", build,
		"resetExtendersButton_.Click([this](auto const&, auto const&) { ConfirmResetExtenders(); });")

	form := definitionBody(t, "AccountPage.cpp", page, "void AccountPage::ApplyExtenderForm(")
	provideRequire(t, "ApplyExtenderForm", form, "resetExtendersButton_.IsEnabled(",
		"ExtenderResetEnabled(state != FieldState::NoSession, savingExtender_)")

	confirm := definitionBody(t, "AccountPage.cpp", page,
		"winrt::fire_and_forget AccountPage::ConfirmResetExtenders()")
	provideRequire(t, "ConfirmResetExtenders", confirm,
		"if (savingExtender_ || w_.sheetOpen()) co_return;",
		"rows::MakeSheet(self->Content().XamlRoot(), Loc(\"reset_extenders\"))",
		"dialog.PrimaryButtonText(Loc(\"reset_extenders\"));",
		"dialog.CloseButtonText(Loc(\"cancel\"));",
		"body.Text(Loc(\"reset_extenders_confirm\"));")
	// Enter must not reset
	provideRequire(t, "ConfirmResetExtenders", confirm,
		"dialog.DefaultButton(ContentDialogButton::Close);")
	provideRequireOrder(t, "ConfirmResetExtenders", confirm,
		"confirmed = co_await dialog.ShowAsync() == ContentDialogResult::Primary;",
		"if (confirmed) ResetExtenders();")
	provideRequireOrder(t, "ConfirmResetExtenders", confirm, "w_.SetSheetOpen(false);",
		"if (confirmed) ResetExtenders();")

	reset := definitionBody(t, "AccountPage.cpp", page,
		"winrt::fire_and_forget AccountPage::ResetExtenders()")
	provideRequireOrder(t, "AccountPage::ResetExtenders", reset, "if (savingExtender_) co_return;",
		"savingExtender_ = true;")
	provideRequireOrder(t, "AccountPage::ResetExtenders", reset, "savingExtender_ = true;",
		"co_await winrt::resume_background();")
	provideRequireOrder(t, "AccountPage::ResetExtenders", reset,
		"co_await winrt::resume_background();", "const bool reset = sdk->ResetExtenders();")
	provideRequireOrder(t, "AccountPage::ResetExtenders", reset,
		"const bool reset = sdk->ResetExtenders();", "reading = ReadExtenderForm(*controller, *sdk);")
	done := sourceBetween(t, "AccountPage::ResetExtenders", reset, "queue.TryEnqueue(", "")
	provideRequireOrder(t, "the reset's completion", done, "page.savingExtender_ = false;",
		"page.ApplyExtenderForm(")
	// the form writes the status line itself, so the verdict goes after it
	provideRequireOrder(t, "the reset's completion", done, "page.ApplyExtenderForm(",
		"Loc(\"extenders_reset_done\")")

	// a load and the reload after a reset read the form one way
	load := definitionBody(t, "AccountPage.cpp", page,
		"winrt::fire_and_forget AccountPage::LoadExtenderSettings()")
	provideRequire(t, "LoadExtenderSettings", load, "ReadExtenderForm(*controller, *sdk)")
	read := definitionBody(t, "AccountPage.cpp", page, "ExtenderFormReading ReadExtenderForm(")
	provideRequire(t, "ReadExtenderForm", read, "ExtenderSettingsViewOf(*settings)",
		"ExtenderSettingsFormFor(view)", "sdk.CurrentNetExtender()")
}
