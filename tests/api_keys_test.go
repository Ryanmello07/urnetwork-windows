// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"html"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The Account > API keys page (owner decision 2026-10-29): the C++ spec of its
// presentation (App/ApiKeysPresentation.h), alone and through the generated
// sdk header's own result types and Api calls; negative controls on it; and,
// for what needs Windows to build, the wiring read from the sources: the
// page's requests and the fence on their answers, the create sheet's one copy
// of the key (shown, copied out of Clipboard History and the cloud clipboard,
// wiped when the sheet closes, never logged or stored), the delete
// confirmation, the Account row and the window's open, close and sign-out.

// Compile the API keys spec (app/tools/api-keys-tests.cpp, header-only).
// extra adds compiler arguments ahead of the source.
func buildApiKeysTests(t *testing.T, extra ...string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("api keys tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "api-keys-tests")
	arguments := []string{"-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I" + filepath.Join(root, "app", "src", "App")}
	arguments = append(arguments, extra...)
	arguments = append(arguments, filepath.Join(root, "app", "tools", "api-keys-tests.cpp"), "-o", program)
	if output, err := exec.Command(compiler, arguments...).CombinedOutput(); err != nil {
		t.Fatalf("build api keys tests: %v\n%s", err, output)
	}
	return program
}

func runApiKeysTests(t *testing.T, program string) {
	t.Helper()
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("api keys: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Compile and execute the API keys spec: the sdk's answers read as plain
// values (failures, server errors, keys with no id, every create_time, the
// list's order), the name a key is created with, the rows and "Unnamed key",
// progress, failed, empty and the rows with a reload over them, the delete
// states, the confirmation, the model's fence and latest-load rule, and the
// create sheet's steps with the one copy of the key.
func TestApiKeysPresentation(t *testing.T) {
	runApiKeysTests(t, buildApiKeysTests(t))
}

// The same spec with the readers run on the generated header's result types,
// and the page's three calls made through the generated Api over a fake of
// the C ABI functions it calls: the JSON the wrapper sends (the trimmed name,
// the key's id) and the JSON the sdk answers with, as Go writes it, read the
// way the page reads it. The header is git-ignored (fetch-deps unpacks it into
// app/third_party/urnetwork-sdk/<arch>; URNETWORK_SDK_INCLUDE names another
// directory) and needs nlohmann/json, so a host without them skips this, and
// so does a header from before the API key calls.
func TestApiKeysPresentationAgainstSdkHeader(t *testing.T) {
	root := repositoryRoot(t)
	sdkDir := apiKeysSdkHeaderDir(t, root)
	if sdkDir == "" {
		t.Skip("no urnetwork_sdk.hpp with the API key calls (set URNETWORK_SDK_INCLUDE)")
	}
	jsonDir, found := jsonIncludeDir(root)
	if !found {
		t.Skip("no nlohmann/json.hpp (set URNETWORK_JSON_INCLUDE)")
	}
	// System includes: the generated wrapper does not build with -Wextra -Werror.
	extra := []string{"-DURNW_API_KEYS_TESTS_SDK", "-isystem", sdkDir}
	if jsonDir != "" {
		extra = append(extra, "-isystem", jsonDir)
	}
	t.Logf("against %s", filepath.Join(sdkDir, "urnetwork_sdk.hpp"))
	runApiKeysTests(t, buildApiKeysTests(t, extra...))

	// a key the reader leaves in the sdk's result on a failed answer
	requireApiKeysFailureWith(t, extra, func(source string) string {
		return strings.Replace(source, "    if (result && result->api_key) Wipe(*result->api_key);\n", "", 1)
	}, "sdk header: a key on a failed answer is wiped, never shown")
}

// The directory of a urnetwork_sdk.hpp that has the API key calls, or "".
func apiKeysSdkHeaderDir(t *testing.T, root string) string {
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
		for _, want := range []string{"struct PublicAccountApiKey {", "void listApiKeys(ListApiKeysCallback callback) const;",
			"void createApiKey(const std::optional<CreateApiKeyArgs>& args, CreateApiKeyCallback callback) const;",
			"void deleteApiKey(const std::optional<DeleteApiKeyArgs>& args, DeleteApiKeyCallback callback) const;"} {
			if !strings.Contains(string(header), want) {
				if dir == explicit {
					t.Fatalf("URNETWORK_SDK_INCLUDE=%s: urnetwork_sdk.hpp has no %s", explicit, want)
				}
				t.Logf("%s: urnetwork_sdk.hpp predates the API key calls", dir)
				dir = ""
				break
			}
		}
		if dir != "" {
			return dir
		}
	}
	return ""
}

// Run the spec against a rewritten copy of ApiKeysPresentation.h and require
// that it fails, naming want.
func requireApiKeysFailure(t *testing.T, mutate func(string) string, want string) {
	t.Helper()
	requireApiKeysFailureWith(t, nil, mutate, want)
}

// requireApiKeysFailure, with the spec built with extra arguments ahead of the
// rewritten header's directory (the sdk header build's).
func requireApiKeysFailureWith(t *testing.T, extra []string, mutate func(string) string, want string) {
	t.Helper()
	const header = "ApiKeysPresentation.h"
	root := repositoryRoot(t)
	source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", header))
	if err != nil {
		t.Fatal(err)
	}
	changed := mutate(string(source))
	if changed == string(source) {
		t.Fatalf("negative control did not change the production %s", header)
	}
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, header), []byte(changed), 0600); err != nil {
		t.Fatal(err)
	}
	// -iquote: searched for the spec's quoted include ahead of the app's -I
	program := buildApiKeysTests(t, append(append([]string{}, extra...), "-iquote", dir)...)
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control on %s was not detected (want %q): %v\n%s", header, want, err, output)
	}
}

// The spec catches each way the page could mishandle the key or misread the
// list: a key left in a failed answer, kept after the sheet closed or shown
// by a sheet that is gone, a close that does not wait for the request; a key
// with no id listed, the list oldest first, Go's zero time read as year 1, a
// stale load repainting the list, one from before a reopen applying, a failed
// reload dropping the list, a delete sent twice; a name of spaces shown, an
// over-long name sent, and a confirmation that does not name the key.
func TestApiKeysPresentationRejectsBrokenReadings(t *testing.T) {
	controls := []struct {
		old, new, want string
	}{
		{"    if (result && result->api_key) Wipe(*result->api_key);\n", "",
			"fakes: a key on a failed answer is wiped, never shown"},
		{"    closed_ = true;\n    Wipe(secret_);\n", "    closed_ = true;\n",
			"Done, Esc, or any close wipes the key"},
		{"    if (closed_ || step_ != CreateStep::Creating) {", "    if (step_ != CreateStep::Creating) {",
			"and never shown"},
		{"  bool HoldsClose() const { return step_ == CreateStep::Creating && !closed_; }",
			"  bool HoldsClose() const { return false; }",
			"Esc is refused while the request is out"},
		{"      if (!key.id || key.id->empty()) continue;", "      if (!key.id) continue;",
			"fakes: the keys with no id are left out, three remain"},
		{"    if (a.createTimeMillis != b.createTimeMillis) return b.createTimeMillis < a.createTimeMillis;",
			"    if (a.createTimeMillis != b.createTimeMillis) return a.createTimeMillis < b.createTimeMillis;",
			"newest first, the same second by name then id"},
		{"  if (!seconds || *seconds <= 0) return 0;", "  if (!seconds) return 0;",
			"Go's zero time is unknown, not year 1"},
		{"    if (load != lastLoad_) return false;\n", "    if (load == 0) return false;\n",
			"an older load, answering after it, is dropped"},
		{"    ++lastLoad_;  // no load from before is the latest\n", "",
			"a load from before the reopen is not the latest"},
		{"    if (!outcome.ok) {\n      state_.failed = true;\n      return true;",
			"    if (!outcome.ok) {\n      state_.failed = true;\n      state_.loaded = false;\n      return true;",
			"a failed reload keeps the list, with its notice"},
		{"    if (found != state_.deletes.end() && found->second == DeleteState::Pending) return false;",
			"    if (found != state_.deletes.end() && found->second == DeleteState::Failed) return false;",
			"a second delete of the same key does not, while one is out"},
		{"  return TrimSpace(key.name).empty() ? text.localized(\"api_keys_unnamed\") : key.name;",
			"  return key.name.empty() ? text.localized(\"api_keys_unnamed\") : key.name;",
			"a name of spaces is no name"},
		{"  if (name.empty() || kMaxNameLength < CharacterCount(name)) return std::nullopt;",
			"  if (name.empty()) return std::nullopt;",
			"129 are refused here, not by the server"},
		{"                      .body = text.format(\"api_keys_delete_confirm_body\", row.name)};",
			"                      .body = text.format(\"api_keys_delete_confirm_body\", row.id)};",
			"the body names the key"},
	}
	for _, control := range controls {
		requireApiKeysFailure(t, func(source string) string {
			return strings.Replace(source, control.old, control.new, 1)
		}, control.want)
	}
}

// The spec's English is the catalog's: every key it reads, word for word, so
// its cases read what the app shows.
func TestApiKeysSpecEnglishIsTheCatalogs(t *testing.T) {
	root := repositoryRoot(t)
	spec, err := os.ReadFile(filepath.Join(root, "app", "tools", "api-keys-tests.cpp"))
	if err != nil {
		t.Fatal(err)
	}
	table := regexp.MustCompile(`(?s)kEnglish = \{(.*?)\n\};`).FindSubmatch(spec)
	if table == nil {
		t.Fatal("api-keys-tests.cpp no longer has its kEnglish table; update this contract")
	}
	entry := regexp.MustCompile(`(?s)\{"([a-z0-9_]+)",\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\}`)
	literal := regexp.MustCompile(`"((?:[^"\\]|\\.)*)"`)
	entries := entry.FindAllSubmatch(table[1], -1)
	if len(entries) < 5 {
		t.Fatalf("read only %d English entries from api-keys-tests.cpp; update this contract", len(entries))
	}
	for _, match := range entries {
		key := string(match[1])
		english := ""
		for _, part := range literal.FindAllSubmatch(match[2], -1) {
			english += strings.ReplaceAll(string(part[1]), `\"`, `"`)
		}
		if got := html.UnescapeString(reswValue(t, root, "en", key)); got != english {
			t.Errorf("api-keys-tests.cpp's English for %s is %q; Strings/en/Resources.resw has %q", key, english, got)
		}
	}
}

// The API keys the store carries for the screen, every one of which the page
// shows somewhere.
var apiKeysStoreKeys = []string{
	"api_keys_title", "api_keys_intro", "api_keys_docs_link", "api_keys_empty", "api_keys_create",
	"api_keys_name_label", "api_keys_name_placeholder", "api_keys_created_title", "api_keys_created_warning",
	"api_keys_copy", "api_keys_copied", "api_keys_created_at", "api_keys_delete", "api_keys_delete_confirm_title",
	"api_keys_delete_confirm_body", "api_keys_load_failed", "api_keys_create_failed", "api_keys_delete_failed",
	"api_keys_unnamed", "api_keys_id",
}

// Every key the API keys sources read is in the catalog, in English and in
// every locale the store translates the page into, and the page reads every
// one of the screen's keys.
func TestApiKeysStringsExist(t *testing.T) {
	root := repositoryRoot(t)
	keyPattern := regexp.MustCompile(`"(api_keys_[a-z0-9_]*)"`)
	keys := map[string]bool{}
	for _, name := range []string{"ApiKeysPresentation.h", "ApiKeysPage.cpp", "AccountPage.cpp"} {
		for _, match := range keyPattern.FindAllStringSubmatch(stripComments(readAppSource(t, name)), -1) {
			keys[match[1]] = true
		}
	}
	for _, key := range apiKeysStoreKeys {
		if !keys[key] {
			t.Errorf("the API keys sources no longer read %s", key)
		}
	}
	for key := range keys {
		for _, locale := range []string{"en", "de", "ja", "ar", "zh-Hans"} {
			if reswValue(t, root, locale, key) == "" {
				t.Errorf("the API keys page reads %q, which %s/Resources.resw does not define", key, locale)
			}
		}
	}
	// the reused keys the page reads by name
	page := stripComments(readAppSource(t, "ApiKeysPage.cpp"))
	for _, key := range []string{"refresh", "retry", "cancel", "done", "create", "loading", "account",
		"please_login_to_urnetwork"} {
		if !strings.Contains(page, `Loc("`+key+`")`) {
			t.Errorf("ApiKeysPage.cpp no longer reads %s", key)
		}
	}
}

// The page's requests: the list on the in-process Api when the page opens
// with a session, its answer read on the sdk's thread and marshalled through
// the DispatcherQueue behind the window's weak reference and the page's alive_
// guard, and applied only to the open it went out under and only when it is
// the latest load; a delete only after a confirmation that names the key and
// defaults to Cancel, for the list the dialog opened over; and the window's
// open, close, rail navigation and sign-out.
func TestApiKeysPageWiring(t *testing.T) {
	page := stripComments(readAppSource(t, "ApiKeysPage.cpp"))
	body := func(signature string) string {
		return definitionBody(t, "ApiKeysPage.cpp", page, signature)
	}
	requireAll := func(name, source string, wants ...string) {
		t.Helper()
		for _, want := range wants {
			if !strings.Contains(source, want) {
				t.Errorf("%s: missing %s", name, want)
			}
		}
	}

	requireAll("ApiKeysPage::HasSession", body("bool ApiKeysPage::HasSession() const {"),
		"return !w_.previewUi() && Sdk().apiReady() && Sdk().IsLoggedIn();")
	requireSessionsOrder(t, "ApiKeysPage::Open", sessionsCode(body("void ApiKeysPage::Open() {")),
		"model_.Open();", "session_=HasSession();", "if(session_)Load();", "Render(true);")
	requireSessionsOrder(t, "ApiKeysPage::Close", body("void ApiKeysPage::Close() {"),
		"open_ = false;", "session_ = false;", "model_.Close();", "if (createSheet_) createSheet_->Dismiss();",
		"w_.ApiKeysHost().Children().Clear();")
	requireSessionsOrder(t, "ApiKeysPage::~ApiKeysPage", body("ApiKeysPage::~ApiKeysPage() {"),
		"*alive_ = false;", "model_.Close();", "createSheet_->Dismiss();")
	requireAll("ApiKeysPage::ResetForSignOut", body("void ApiKeysPage::ResetForSignOut() {"), "Close();")

	requireSessionsOrder(t, "ApiKeysPage::Load", sessionsCode(body("void ApiKeysPage::Load() {")),
		"if(!open_||!session_)return;",
		"constuint64_tgeneration=model_.Current();",
		"constuint64_tload=model_.BeginLoad();",
		"Sdk().api().listApiKeys([queue,weak,alive,generation,load](",
		"apikeys::ListOutcomeoutcome=apikeys::ListOutcomeFrom(result,err);",
		"queue.TryEnqueue([weak,alive,generation,load,outcome=std::move(outcome)]()mutable{",
		"if(!*alive)return;",
		"if(autoself=weak.get())self->apiKeys().ApplyList(generation,load,std::move(outcome));")
	requireSessionsOrder(t, "ApiKeysPage::ApplyList",
		sessionsCode(body("void ApiKeysPage::ApplyList(uint64_t generation, uint64_t load, apikeys::ListOutcome outcome) {")),
		"if(!model_.Admits(generation))return;", "if(!model_.ApplyLoad(load,std::move(outcome)))return;",
		"Render(false);")
	requireAll("ApiKeysPage::Refresh", body("void ApiKeysPage::Refresh() {"), "Load();")
	requireAll("ApiKeysPage::OnCreated", body("void ApiKeysPage::OnCreated(uint64_t generation) {"),
		"if (!open_ || !model_.Admits(generation)) return;", "Load();")
	requireAll("ApiKeysPage::Build", body("void ApiKeysPage::Build() {"),
		"w_.ApiKeysBackButton().Click(", "w_.CloseApiKeys();", "w_.ApiKeysRefreshButton().Click(", "Refresh();",
		"docsLink_.NavigateUri(winrt::Windows::Foundation::Uri(apikeys::kDocsUrl));",
		"createButton_.Click(", "ShowCreateSheet();")
	if !strings.Contains(readAppSource(t, "ApiKeysPresentation.h"),
		`inline constexpr const wchar_t* kDocsUrl = L"https://ur.io/docs/api";`) {
		t.Error("the intro's link must be https://ur.io/docs/api")
	}
	requireAll("ApiKeysPage::ApplyStrings", body("void ApiKeysPage::ApplyStrings() {"),
		`Loc("api_keys_title")`, `Loc("api_keys_intro")`, `Loc("api_keys_docs_link")`, `Loc("api_keys_create")`)

	// every delete confirms first, Cancel the default, names the key, and
	// deletes only on the explicit button, for the list the dialog opened over
	requireSessionsOrder(t, "ApiKeysPage::ConfirmDelete",
		body("winrt::fire_and_forget ApiKeysPage::ConfirmDelete(std::string keyId) {"),
		"w_.sheetOpen()", "apikeys::DeleteConfirmationFor(row, ApiKeysText())",
		"const uint64_t generation = model_.Current();", "w_.SetSheetOpen(true);",
		`dialog.PrimaryButtonText(Loc("api_keys_delete"));`, `dialog.CloseButtonText(Loc("cancel"));`,
		"dialog.DefaultButton(ContentDialogButton::Close);",
		"confirmed = co_await dialog.ShowAsync() == ContentDialogResult::Primary;",
		"w_.SetSheetOpen(false);", "if (confirmed) Delete(keyId, generation);", "RestoreFocus(")
	requireSessionsOrder(t, "ApiKeysPage::Delete",
		body("void ApiKeysPage::Delete(std::string const& keyId, uint64_t generation) {"),
		"if (!open_ || !session_ || !model_.Admits(generation)) return;",
		"if (!model_.BeginDelete(keyId)) return;", "args.id = keyId;", "Sdk().api().deleteApiKey(args,",
		"apikeys::DeleteOutcomeFrom(result, err)", "if (!*alive) return;",
		"self->apiKeys().ApplyDelete(generation, keyId, outcome);")
	requireSessionsOrder(t, "ApiKeysPage::ApplyDelete",
		sessionsCode(body("void ApiKeysPage::ApplyDelete(uint64_t generation, std::string const& keyId,")),
		"if(!model_.Admits(generation))return;", "model_.ApplyDelete(keyId,outcome.ok);",
		"Render(false);", "if(outcome.ok)Load();")
	for call, want := range map[string]int{"->listApiKeys(": 0, ".listApiKeys(": 1, ".createApiKey(": 1,
		".deleteApiKey(": 1, "ConfirmDelete(keyId);": 1} {
		if got := strings.Count(page, call); got != want {
			t.Errorf("ApiKeysPage.cpp makes %d %s calls, want %d: each request goes out from its one place", got, call, want)
		}
	}

	// the row: Delete disabled while it runs, named, its key's name described;
	// the id selectable; a failed delete keeps its words
	row := body("void ApiKeysPage::AppendRow(Panel const& host, apikeys::Row const& row) {")
	requireAll("ApiKeysPage::AppendRow", row,
		"kit::MakeRowGlyph(kKeyGlyph)", "del.IsEnabled(!pending);", `Loc("api_keys_delete")`,
		"Automation::AutomationProperties::SetFullDescription(del, H(row.name));",
		"ConfirmDelete(keyId);", "idLine.IsTextSelectionEnabled(true);", "row.createdSpoken")
	requireSessionsOrder(t, "ApiKeysPage::AppendRow", sessionsCode(row),
		"if(row.del==apikeys::DeleteState::Failed){", `failed.Text(Loc("api_keys_delete_failed"));`)
	render := body("void ApiKeysPage::RenderBody(apikeys::View const& view) {")
	requireAll("ApiKeysPage::RenderBody", render, `Loc("api_keys_load_failed")`, `Loc("retry")`,
		`Loc("api_keys_empty")`, `Loc("loading")`, "RestoreFocus(focused);")
	requireSessionsOrder(t, "ApiKeysPage::Render", sessionsCode(body("void ApiKeysPage::Render(bool force) {")),
		"if(!session_){", `AppendProse(host,Loc("please_login_to_urnetwork"),colors::FaintBrush());`, "return;",
		"apikeys::Viewview=apikeys::ViewFor(model_.state(),ApiKeysText());",
		"w_.ApiKeysRefreshRing().IsActive(view.refreshing);")

	// the window: open from the Account row, closed by its back button, by any
	// rail navigation and by a sign-out
	window := stripComments(readAppSource(t, "MainWindow.xaml.cpp"))
	windowBody := func(signature string) string {
		return definitionBody(t, "MainWindow.xaml.cpp", window, signature)
	}
	requireSessionsOrder(t, "MainWindow::OpenApiKeys", windowBody("void MainWindow::OpenApiKeys() {"),
		"apiKeysOpen_ = true;", "AccountView().Visibility(Visibility::Collapsed);",
		"ApiKeysView().Visibility(Visibility::Visible);", "apiKeys_->Open();")
	requireSessionsOrder(t, "MainWindow::CloseApiKeys", windowBody("void MainWindow::CloseApiKeys() {"),
		"apiKeysOpen_ = false;", "apiKeys_->Close();", "ApiKeysView().Visibility(Visibility::Collapsed);",
		"AccountView().Visibility(Visibility::Visible);", "account_->FocusApiKeysNav();")
	requireAll("MainWindow::OnNavSelectionChanged", windowBody("void MainWindow::OnNavSelectionChanged("),
		"apiKeys_->Close();", "ApiKeysView().Visibility(Visibility::Collapsed);")
	requireAll("MainWindow::ApplyAuthState", windowBody("void MainWindow::ApplyAuthState("),
		"apiKeys_->ResetForSignOut();", "if (apiKeysOpen_) CloseApiKeys();")
	requireAll("MainWindow::~MainWindow", windowBody("MainWindow::~MainWindow() {"), "apiKeys_.reset();")
	requireAll("MainWindow::ApplyStrings", window, "apiKeys_->ApplyStrings();")
	requireAll("MainWindow::MainWindow", windowBody("MainWindow::MainWindow() {"),
		"apiKeys_ = std::make_unique<urnw::ApiKeysPage>(*this);")

	// the Account row, right after Sessions
	account := stripComments(readAppSource(t, "AccountPage.cpp"))
	requireAll("AccountPage::BuildApiKeysNav",
		definitionBody(t, "AccountPage.cpp", account, "void AccountPage::BuildApiKeysNav() {"),
		"w_.AccountApiKeysNavHost()", "kit::MakeRowGlyph(kApiKeysGlyph)", `Loc("api_keys_title")`,
		`L"acceptance.account.api-keys"`, "w_.OpenApiKeys();")
	requireSessionsOrder(t, "AccountPage::ApplyStrings",
		definitionBody(t, "AccountPage.cpp", account, "void AccountPage::ApplyStrings() {"),
		"BuildSessionsNav();", "BuildApiKeysNav();")
	requireAll("AccountPage::FocusApiKeysNav",
		definitionBody(t, "AccountPage.cpp", account, "void AccountPage::FocusApiKeysNav() {"),
		"apiKeysNavButton_.Focus(FocusState::Programmatic);")

	// the markup: the page in Account's place, and the row's host right after
	// the Sessions row's, ahead of the security rows
	markup := readAppSource(t, "MainWindow.xaml")
	for _, name := range []string{"ApiKeysView", "ApiKeysPane", "ApiKeysBackButton", "ApiKeysPaneTitle",
		"ApiKeysRefreshRing", "ApiKeysRefreshButton", "ApiKeysRefreshText", "ApiKeysIntroHost", "ApiKeysHost"} {
		if !strings.Contains(markup, `x:Name="`+name+`"`) {
			t.Errorf("MainWindow.xaml: missing %s", name)
		}
	}
	sessions := strings.Index(markup, `<StackPanel x:Name="AccountSessionsNavHost" />`)
	apiKeys := strings.Index(markup, `<StackPanel x:Name="AccountApiKeysNavHost" />`)
	security := strings.Index(markup, `<StackPanel x:Name="AccountSecurityHost" />`)
	if sessions < 0 || apiKeys < 0 || security < 0 || !(sessions < apiKeys && apiKeys < security) {
		t.Error("MainWindow.xaml: the API keys row's host must sit right after the Sessions row's, ahead of the security rows")
	} else if strings.Contains(markup[sessions+1:apiKeys], "<StackPanel") || strings.Contains(markup[sessions+1:apiKeys], "<Border") {
		t.Error("MainWindow.xaml: nothing may sit between the Sessions row and the API keys row")
	}

	project := readAppSource(t, "App.vcxproj")
	for _, item := range []string{`<ClCompile Include="ApiKeysPage.cpp" />`, `<ClInclude Include="ApiKeysPage.h" />`,
		`<ClInclude Include="ApiKeysPresentation.h" />`} {
		if !strings.Contains(project, item) {
			t.Errorf("App.vcxproj does not list %s", item)
		}
	}
}

// The create sheet's one copy of the key: the name step requires a name the
// server takes; while the request is out the sheet has no close button and
// refuses Esc, but not a sign-out's close; the key moves out of the sdk's
// result on the sdk's thread into a value that wipes it unless the sheet takes
// it; it is shown in a text block nothing can select, copied only out of
// Clipboard History and off the cloud clipboard (never a plain SetContent),
// wiped and taken off the screen when the sheet closes by any route, and never
// logged, stored or sent anywhere else.
func TestApiKeysCreateSheetKeepsTheKeyOnce(t *testing.T) {
	page := stripComments(readAppSource(t, "ApiKeysPage.cpp"))
	body := func(signature string) string {
		return definitionBody(t, "ApiKeysPage.cpp", page, signature)
	}
	requireAll := func(name, source string, wants ...string) {
		t.Helper()
		for _, want := range wants {
			if !strings.Contains(source, want) {
				t.Errorf("%s: missing %s", name, want)
			}
		}
	}

	build := body("void ApiKeyCreateSheet::Build(XamlRoot const& root) {")
	requireAll("ApiKeyCreateSheet::Build", build,
		`nameBox_.Header(winrt::box_value(Loc("api_keys_name_label")));`,
		`nameBox_.PlaceholderText(Loc("api_keys_name_placeholder"));`,
		"nameBox_.MaxLength(static_cast<int32_t>(apikeys::kMaxNameLength));",
		`Loc("api_keys_create_failed")`, `Loc("api_keys_created_warning")`,
		"keyText_.IsTextSelectionEnabled(false);",
		"args.Cancel(true);",
		"if (!self->dismissed_ && self->flow_.HoldsClose()) args.Cancel(true);",
		"dialog_.Closed(", "self->Clear();")
	render := body("void ApiKeyCreateSheet::Render() {")
	requireAll("ApiKeyCreateSheet::Render", render,
		"keyText_.Text(H(flow_.secret()));", `dialog_.CloseButtonText(Loc("done"));`,
		"dialog_.DefaultButton(ContentDialogButton::Close);", `dialog_.PrimaryButtonText(Loc("create"));`,
		"dialog_.IsPrimaryButtonEnabled(flow_.CanCreate(", `dialog_.CloseButtonText(creating ? hstring{} : Loc("cancel"));`,
		`Loc(created ? "api_keys_created_title" : "api_keys_create")`)
	requireSessionsOrder(t, "ApiKeyCreateSheet::Submit", sessionsCode(body("void ApiKeyCreateSheet::Submit() {")),
		"if(!Sdk().apiReady()||!Sdk().IsLoggedIn())return;",
		"conststd::optional<std::string>name=flow_.Begin(Narrow(nameBox_.Text().c_str()));",
		"if(!name)return;", "args.name=*name;", "Sdk().api().createApiKey(args,",
		"std::make_shared<apikeys::CreateOutcome>(apikeys::CreateOutcomeFrom(result,err));",
		"queue.TryEnqueue([weak,outcome]{", "if(autoself=weak.lock())self->Apply(*outcome);")
	requireSessionsOrder(t, "ApiKeyCreateSheet::Apply",
		body("void ApiKeyCreateSheet::Apply(apikeys::CreateOutcome& outcome) {"),
		"const bool created = outcome.ok;", "flow_.Apply(outcome);", "if (created && onCreated_) onCreated_();")
	requireSessionsOrder(t, "ApiKeyCreateSheet::Copy", body("void ApiKeyCreateSheet::Copy() {"),
		"package.SetText(H(flow_.secret()));", "options.IsAllowedInHistory(false);", "options.IsRoamable(false);",
		"if (!dt::Clipboard::SetContentWithOptions(package, options)) {", "return;", "ShowCopied(true);")
	requireSessionsOrder(t, "ApiKeyCreateSheet::Clear", body("void ApiKeyCreateSheet::Clear() {"),
		"flow_.Close();", "keyText_.Text(hstring{});")
	requireSessionsOrder(t, "ApiKeyCreateSheet::Dismiss", body("void ApiKeyCreateSheet::Dismiss() {"),
		"dismissed_ = true;", "Clear();", "dialog_.Hide();")
	requireAll("ApiKeyCreateSheet::~ApiKeyCreateSheet", body("ApiKeyCreateSheet::~ApiKeyCreateSheet() {"),
		"flow_.Close();")
	requireAll("ApiKeyCreateSheet::ShowCopied", body("void ApiKeyCreateSheet::ShowCopied(bool copied) {"),
		`Loc(copied ? "api_keys_copied" : "api_keys_copy")`)

	// the key is read in three places: shown (Render), and checked and copied
	// (Copy)
	if count := strings.Count(page, "flow_.secret()"); count != 3 {
		t.Errorf("ApiKeysPage.cpp reads the key %d times; it is shown (Render), checked and copied (Copy), "+
			"and nothing else may read it", count)
	}
	// only the history-excluded copy reaches the clipboard: no plain copy, and
	// no selection whose Ctrl+C would make one
	for _, call := range []string{"Clipboard::SetContent(", "CopyToClipboard(", "keyText_.IsTextSelectionEnabled(true)"} {
		if strings.Contains(page, call) {
			t.Errorf("ApiKeysPage.cpp: %s would put the key into Clipboard History or the cloud clipboard", call)
		}
	}
	// no log line names the key or its holders
	logCall := regexp.MustCompile(`Log(?:Info|Warn|Error|Debug)\(((?:[^;]|"[^"]*")*)\);`)
	logs := logCall.FindAllStringSubmatch(page, -1)
	if len(logs) < 5 {
		t.Errorf("found only %d log calls in ApiKeysPage.cpp; update this contract", len(logs))
	}
	for _, match := range logs {
		for _, secret := range []string{"secret", "api_key", "flow_", "keyText_", "*outcome"} {
			if strings.Contains(match[1], secret) {
				t.Errorf("ApiKeysPage.cpp logs %s: %s", secret, match[0])
			}
		}
	}
	// nothing of the page is stored or reported
	for _, call := range []string{"ApplicationData", "LocalSettings", "RegSetValue", "RegSetKeyValue", "ofstream",
		"fopen", "WriteFile", "events_", "ClientEvent"} {
		if strings.Contains(page, call) {
			t.Errorf("ApiKeysPage.cpp uses %s; nothing of the API keys page is stored or reported", call)
		}
	}
	header := stripComments(readAppSource(t, "ApiKeysPage.h"))
	for _, member := range []string{"std::string secret", "std::string key_", "std::string apiKey"} {
		if strings.Contains(header, member) {
			t.Errorf("ApiKeysPage.h holds the key outside the sheet's CreateFlow (%s)", member)
		}
	}
}
