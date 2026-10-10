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

// The Account > Sessions page (server session/REVOKE-UI-FINAL.md): the C++
// spec of its presentation (App/SessionsPresentation.h) and of the relative
// time it shares (App/RelativeTimeSpan.h), alone and through the generated sdk
// header's own classes; negative controls on both; and, for what needs
// Windows to build, the wiring read from the sources: the page's controller
// lifecycle, the client info and the app's version, and the Account rows'
// leading marks.

// Compile the sessions spec (app/tools/sessions-tests.cpp, header-only).
// extra adds compiler arguments ahead of the source.
func buildSessionsTests(t *testing.T, extra ...string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("sessions tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "sessions-tests")
	arguments := []string{"-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I" + filepath.Join(root, "app", "src", "App")}
	arguments = append(arguments, extra...)
	arguments = append(arguments, filepath.Join(root, "app", "tools", "sessions-tests.cpp"), "-o", program)
	if output, err := exec.Command(compiler, arguments...).CombinedOutput(); err != nil {
		t.Fatalf("build sessions tests: %v\n%s", err, output)
	}
	return program
}

func runSessionsTests(t *testing.T, program string) {
	t.Helper()
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("sessions: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Compile and execute the sessions spec: relative time across seconds,
// minutes, hours, days and the 7-day date cut-over; every device label and
// logo, every sign-in method and the omitted legacy kinds; the country
// circle's colour and its empty-code fallback; the rows' three lines, their
// spoken forms and the 8-character id; never loaded, loading, empty, failed,
// refresh-failed, unsupported and sign-in required; the pending, failed and
// bulk states; the confirmations; and the fence that drops an old
// controller's snapshots.
func TestSessionsPresentation(t *testing.T) {
	runSessionsTests(t, buildSessionsTests(t))
}

// The same spec with the snapshot read through the generated header's
// urnet::ClientSessionSnapshot family, over a fake of the C ABI functions
// those classes call: every field arrives, nil handles read as nothing, and
// every handle the reader takes is released once. The header is git-ignored
// (fetch-deps unpacks it into app/third_party/urnetwork-sdk/<arch>;
// URNETWORK_SDK_INCLUDE names another directory) and needs nlohmann/json, so a
// host without them skips this, and so does a header from before the session
// controller.
func TestSessionsPresentationAgainstSdkHeader(t *testing.T) {
	root := repositoryRoot(t)
	sdkDir := sessionsSdkHeaderDir(t, root)
	if sdkDir == "" {
		t.Skip("no urnetwork_sdk.hpp with the session view controller (set URNETWORK_SDK_INCLUDE)")
	}
	jsonDir, found := jsonIncludeDir(root)
	if !found {
		t.Skip("no nlohmann/json.hpp (set URNETWORK_JSON_INCLUDE)")
	}
	// System includes: the generated wrapper does not build with -Wextra -Werror.
	extra := []string{"-DURNW_SESSIONS_TESTS_SDK", "-isystem", sdkDir}
	if jsonDir != "" {
		extra = append(extra, "-isystem", jsonDir)
	}
	t.Logf("against %s", filepath.Join(sdkDir, "urnetwork_sdk.hpp"))
	runSessionsTests(t, buildSessionsTests(t, extra...))
}

// The directory of a urnetwork_sdk.hpp that has the session view controller, or "".
func sessionsSdkHeaderDir(t *testing.T, root string) string {
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
		if !strings.Contains(string(header), "class ClientSessionViewController final") {
			if dir == explicit {
				t.Fatalf("URNETWORK_SDK_INCLUDE=%s: urnetwork_sdk.hpp has no session view controller", explicit)
			}
			t.Logf("%s: urnetwork_sdk.hpp predates the session view controller", dir)
			continue
		}
		return dir
	}
	return ""
}

// Run the spec against a rewritten copy of one of its headers and require
// that it fails, naming want. The rest of the includes still come from the app.
func requireSessionsFailure(t *testing.T, header string, mutate func(string) string, want string) {
	t.Helper()
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
	program := buildSessionsTests(t, "-iquote", dir)
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control on %s was not detected (want %q): %v\n%s", header, want, err, output)
	}
}

// The spec catches each way the page could misread a session: a relative time
// that stops at hours (what RelativeTime did before days) or turns into a date
// a day late, a last use read as milliseconds, a legacy kind given a method, a
// bulk sign-out offered without a current session, an empty country coloured
// as a real one, and a pending revoke that offers Sign out again.
func TestSessionsPresentationRejectsBrokenReadings(t *testing.T) {
	requireSessionsFailure(t, "RelativeTimeSpan.h", func(source string) string {
		return strings.Replace(source,
			"  if (seconds < kDateDays * kDay) return {Unit::Days, seconds / kDay};\n", "", 1)
	}, "24h is 1d")
	requireSessionsFailure(t, "RelativeTimeSpan.h", func(source string) string {
		return strings.Replace(source, "inline constexpr int64_t kDateDays = 7;",
			"inline constexpr int64_t kDateDays = 8;", 1)
	}, "a week is a date")
	requireSessionsFailure(t, "SessionsPresentation.h", func(source string) string {
		return strings.Replace(source, "session.lastUsed->unixTime * 1000;", "session.lastUsed->unixTime;", 1)
	}, "in milliseconds")
	requireSessionsFailure(t, "SessionsPresentation.h", func(source string) string {
		return strings.Replace(source,
			"  if (kind == \"api_key_client\") return \"sessions_kind_api_key_client\";\n  return \"\";",
			"  if (kind == \"api_key_client\") return \"sessions_kind_api_key_client\";\n  return \"sessions_kind_password\";", 1)
	}, "kind \"legacy\"")
	requireSessionsFailure(t, "SessionsPresentation.h", func(source string) string {
		return strings.Replace(source, "view.bulkShown = current && other;", "view.bulkShown = other;", 1)
	}, "no current session: no bulk")
	requireSessionsFailure(t, "SessionsPresentation.h", func(source string) string {
		return strings.Replace(source, "  if (countryCode.empty()) return kUnknownCountryRgb;\n", "", 1)
	}, "an empty code is the unknown-country colour")
	requireSessionsFailure(t, "SessionsPresentation.h", func(source string) string {
		return strings.Replace(source, "if (action->loading || action->pending) return ActionState::Pending;",
			"if (action->loading) return ActionState::Pending;", 1)
	}, "a 202 stays pending")
}

// The spec's English is the catalog's: every key it reads, word for word, so
// its cases read what the app shows.
func TestSessionsSpecEnglishIsTheCatalogs(t *testing.T) {
	root := repositoryRoot(t)
	spec, err := os.ReadFile(filepath.Join(root, "app", "tools", "sessions-tests.cpp"))
	if err != nil {
		t.Fatal(err)
	}
	table := regexp.MustCompile(`(?s)kEnglish = \{(.*?)\n\};`).FindSubmatch(spec)
	if table == nil {
		t.Fatal("sessions-tests.cpp no longer has its kEnglish table; update this contract")
	}
	entry := regexp.MustCompile(`(?s)\{"([a-z0-9_]+)",\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\}`)
	literal := regexp.MustCompile(`"((?:[^"\\]|\\.)*)"`)
	entries := entry.FindAllSubmatch(table[1], -1)
	if len(entries) < 38 {
		t.Fatalf("read only %d English entries from sessions-tests.cpp; update this contract", len(entries))
	}
	for _, match := range entries {
		key := string(match[1])
		english := ""
		for _, part := range literal.FindAllSubmatch(match[2], -1) {
			english += strings.ReplaceAll(string(part[1]), `\"`, `"`)
		}
		if got := html.UnescapeString(reswValue(t, root, "en", key)); got != english {
			t.Errorf("sessions-tests.cpp's English for %s is %q; Strings/en/Resources.resw has %q", key, english, got)
		}
	}
}

// Every key the sessions sources read is in the catalog, in English and in
// every locale the store translates the page into.
func TestSessionsStringsExist(t *testing.T) {
	root := repositoryRoot(t)
	keyPattern := regexp.MustCompile(`"((?:sessions|days_ago|hours_ago|minutes_ago|seconds_ago)[a-z0-9_]*)"`)
	keys := map[string]bool{}
	for _, name := range []string{"SessionsPresentation.h", "SessionsPage.cpp", "RelativeTimeSpan.h", "AccountPage.cpp"} {
		for _, match := range keyPattern.FindAllStringSubmatch(stripComments(readAppSource(t, name)), -1) {
			keys[match[1]] = true
		}
	}
	if len(keys) < 48 {
		t.Errorf("found only %d sessions keys in the sources; update this contract", len(keys))
	}
	for key := range keys {
		for _, locale := range []string{"en", "de", "ja", "ar", "zh-Hans"} {
			if reswValue(t, root, locale, key) == "" {
				t.Errorf("the sessions page reads %q, which %s/Resources.resw does not define", key, locale)
			}
		}
	}
	// the reused keys the page reads by name
	page := stripComments(readAppSource(t, "SessionsPage.cpp"))
	for _, key := range []string{"refresh", "try_again", "sign_out", "cancel", "copied", "loading",
		"account", "please_login_to_urnetwork", "something_went_wrong"} {
		if !strings.Contains(page, `Loc("`+key+`")`) {
			t.Errorf("SessionsPage.cpp no longer reads %s", key)
		}
	}
}

// The page's controller: opened on the in-process Api when the page opens,
// its listener reading the snapshot on the sdk's thread and marshalling the
// value through the DispatcherQueue behind the window's weak reference, the
// page's alive_ guard and the open fence; visible while the page shows and the
// window presents, foreground from the presentation; and closed with the page:
// the listener dropped, then the controller.
func TestSessionsPageControllerLifecycle(t *testing.T) {
	page := stripComments(readAppSource(t, "SessionsPage.cpp"))
	body := func(signature string) string {
		return definitionBody(t, "SessionsPage.cpp", page, signature)
	}
	requireAll := func(name, source string, wants ...string) {
		t.Helper()
		for _, want := range wants {
			if !strings.Contains(source, want) {
				t.Errorf("%s: missing %s", name, want)
			}
		}
	}
	requireOrder := func(name, source string, ordered ...string) {
		t.Helper()
		from := 0
		for _, want := range ordered {
			at := strings.Index(source[from:], want)
			if at < 0 {
				t.Errorf("%s: %s is missing or out of order", name, want)
				return
			}
			from += at + len(want)
		}
	}

	open := body("void SessionsPage::OpenController() {")
	requireOrder("SessionsPage::OpenController", open,
		"controller_.emplace(Sdk().api().openClientSessionViewController());",
		"if (!*controller_) {",
		"const uint64_t generation = fence_.Open();",
		"sub_.emplace(controller_->addClientSessionListener(",
		"read = sessions::SnapshotFrom(snapshot);",
		"queue.TryEnqueue([weak, alive, generation, read = std::move(read)]",
		"if (!*alive) return;",
		"self->sessions().ApplySnapshot(generation, std::move(read));",
		"ApplyVisibility();",
		"controller_->start();")
	if strings.Contains(open, "device()") {
		t.Error("SessionsPage::OpenController opens the controller on the device; it belongs to the in-process Api")
	}

	closer := body("void SessionsPage::CloseController() {")
	requireOrder("SessionsPage::CloseController", closer,
		"fence_.Close();", "sub_.reset();", "controller_->close();", "controller_.reset();")
	requireOrder("SessionsPage::~SessionsPage", body("SessionsPage::~SessionsPage() {"),
		"*alive_ = false;", "CloseController();")
	requireAll("SessionsPage::Close", body("void SessionsPage::Close() {"), "open_ = false;", "CloseController();")
	requireAll("SessionsPage::Open", body("void SessionsPage::Open() {"),
		"CloseController();", "open_ = true;",
		"!w_.previewUi() && Sdk().apiReady() && Sdk().IsLoggedIn()", "OpenController();")

	requireAll("SessionsPage::ApplyVisibility", body("void SessionsPage::ApplyVisibility() {"),
		"const bool visible = open_ && presentationActive_;", "controller_->setVisible(visible);")
	requireOrder("SessionsPage::SetPresentationActive", body("void SessionsPage::SetPresentationActive(bool active) {"),
		"presentationActive_ = active;", "ApplyVisibility();", "controller_->setForeground(active);")
	requireAll("SessionsPage::ApplySnapshot", body("void SessionsPage::ApplySnapshot(uint64_t generation, sessions::Snapshot snapshot) {"),
		"if (!fence_.Admits(generation)) return;")
	requireAll("SessionsPage::Refresh", body("void SessionsPage::Refresh() {"), "controller_->refresh();")
	requireAll("SessionsPage::Build", body("void SessionsPage::Build() {"),
		"w_.SessionsBackButton().Click(", "w_.CloseSessions();",
		"w_.SessionsRefreshButton().Click(", "Refresh();")

	// every sign-out confirms first, Cancel the default, and revokes only on the
	// explicit button, through the controller the dialog opened over
	for _, signature := range []string{
		"winrt::fire_and_forget SessionsPage::ConfirmSignOut(std::string sessionId) {",
		"winrt::fire_and_forget SessionsPage::ConfirmSignOutOthers() {",
	} {
		confirm := body(signature)
		requireOrder(signature, confirm,
			"w_.sheetOpen()", "const uint64_t generation = fence_.Current();", "w_.SetSheetOpen(true);",
			`dialog.PrimaryButtonText(Loc("sign_out"));`, `dialog.CloseButtonText(Loc("cancel"));`,
			"dialog.DefaultButton(ContentDialogButton::Close);",
			"confirmed = co_await dialog.ShowAsync() == ContentDialogResult::Primary;",
			"w_.SetSheetOpen(false);", "if (confirmed)", "RestoreFocus(")
	}
	requireOrder("SessionsPage::SignOut", body("void SessionsPage::SignOut(std::string const& sessionId, uint64_t generation) {"),
		"if (!controller_ || !fence_.Admits(generation)) return;", "controller_->revokeSession(sessionId);")
	requireOrder("SessionsPage::SignOutOthers", body("void SessionsPage::SignOutOthers(uint64_t generation) {"),
		"if (!controller_ || !fence_.Admits(generation)) return;", "controller_->revokeOtherSessions();")
	if strings.Count(page, "->revokeSession(") != 1 || strings.Count(page, "->revokeOtherSessions(") != 1 {
		t.Error("SessionsPage.cpp revokes outside SignOut and SignOutOthers, which only a confirmation reaches")
	}

	// the row: Sign out disabled while the revoke runs, named for a screen
	// reader by its device, and the copy of the whole id
	row := body("void SessionsPage::AppendRow(Panel const& host, sessions::Row const& row) {")
	requireAll("SessionsPage::AppendRow", row,
		"MakeCountryCircle(row)",
		"signOut.IsEnabled(!pending);",
		`Loc("sessions_signing_out")`,
		"Automation::AutomationProperties::SetName(signOut, H(row.signOutName));",
		"ConfirmSignOut(sessionId);",
		`Loc("sessions_copy_id")`,
		"CopyId(sessionId);",
		`Loc("sessions_action_failed")`,
		"row.line1Spoken", "row.line2Spoken", "row.line3Spoken",
		"Automation::AutomationProperties::SetFullDescription(copy, H(row.idText));")
	requireAll("SessionsPage::CopyId", body("void SessionsPage::CopyId(std::string const& sessionId) {"),
		"rows::CopyToClipboard(sessionId);", `Loc("copied")`)
	requireAll("MakeCountryCircle", body("Grid MakeCountryCircle(sessions::Row const& row) {"),
		"sessions::CircleColorFor(", "urnet::getColorHex(code)", "shapes::Ellipse dot;",
		"kit::MakeRowPathIcon(sessions::GlyphPath(row.glyph)", "255, 255, 255, 255")
	requireAll("SessionsPage::AppendBulk", body("void SessionsPage::AppendBulk(Panel const& host, sessions::View const& view) {"),
		"if (!view.bulkShown) return;", `Loc("sessions_sign_out_all_others")`, "ConfirmSignOutOthers();",
		"bulkButton_.IsEnabled(!pending);")
	render := body("void SessionsPage::RenderBody(sessions::View const& view) {")
	requireAll("SessionsPage::RenderBody", render,
		`Loc("sessions_load_failed")`, `Loc("try_again")`, `Loc("sessions_unsupported")`,
		"sessions::SignInRequiredKey(", `Loc("sessions_empty")`, `Loc("sessions_refresh_failed")`,
		`Loc("sessions_last_used_help")`, `Loc("sessions_legacy_note")`, "RestoreFocus(focused);")
	requireAll("SessionsPage::Render", body("void SessionsPage::Render(bool force) {"),
		"w_.SessionsRefreshRing().IsActive(view.refreshing);", `Loc("please_login_to_urnetwork")`)

	// the window: open from the Account row, closed by its back button, by any
	// rail navigation and by a sign-out, polled with the presentation
	window := stripComments(readAppSource(t, "MainWindow.xaml.cpp"))
	windowBody := func(signature string) string {
		return definitionBody(t, "MainWindow.xaml.cpp", window, signature)
	}
	requireOrder("MainWindow::OpenSessions", windowBody("void MainWindow::OpenSessions() {"),
		"sessionsOpen_ = true;", "AccountView().Visibility(Visibility::Collapsed);",
		"SessionsView().Visibility(Visibility::Visible);", "sessions_->Open();")
	requireOrder("MainWindow::CloseSessions", windowBody("void MainWindow::CloseSessions() {"),
		"sessionsOpen_ = false;", "sessions_->Close();", "SessionsView().Visibility(Visibility::Collapsed);",
		"AccountView().Visibility(Visibility::Visible);", "account_->FocusSessionsNav();")
	requireAll("MainWindow::OnNavSelectionChanged", windowBody("void MainWindow::OnNavSelectionChanged("),
		"sessions_->Close();", "SessionsView().Visibility(Visibility::Collapsed);")
	requireAll("MainWindow::ApplyAuthState", windowBody("void MainWindow::ApplyAuthState("),
		"sessions_->ResetForSignOut();", "if (sessionsOpen_) CloseSessions();")
	requireAll("MainWindow::SetPresentationActive", windowBody("void MainWindow::SetPresentationActive(bool active) {"),
		"sessions_->SetPresentationActive(active);")
	requireAll("MainWindow::~MainWindow", windowBody("MainWindow::~MainWindow() {"), "sessions_.reset();")
	requireAll("MainWindow::ApplyStrings", window, "sessions_->ApplyStrings();")
	requireAll("MainWindow::MainWindow", windowBody("MainWindow::MainWindow() {"),
		"sessions_ = std::make_unique<urnw::SessionsPage>(*this);")
	account := stripComments(readAppSource(t, "AccountPage.cpp"))
	requireAll("AccountPage::BuildSessionsNav",
		definitionBody(t, "AccountPage.cpp", account, "void AccountPage::BuildSessionsNav() {"),
		"w_.AccountSessionsNavHost()", `Loc("sessions_title")`, "w_.OpenSessions();")

	// the markup: the page in Account's place, and the row's host right after
	// the profile, ahead of the security rows
	markup := readAppSource(t, "MainWindow.xaml")
	for _, name := range []string{"SessionsView", "SessionsPane", "SessionsBackButton", "SessionsPaneTitle",
		"SessionsRefreshRing", "SessionsRefreshButton", "SessionsRefreshText", "SessionsHost", "SessionsInfo"} {
		if !strings.Contains(markup, `x:Name="`+name+`"`) {
			t.Errorf("MainWindow.xaml: missing %s", name)
		}
	}
	profile := strings.Index(markup, `<StackPanel x:Name="AccountProfileExtra" />`)
	sessions := strings.Index(markup, `<StackPanel x:Name="AccountSessionsNavHost" />`)
	security := strings.Index(markup, `<StackPanel x:Name="AccountSecurityHost" />`)
	if profile < 0 || sessions < 0 || security < 0 || !(profile < sessions && sessions < security) {
		t.Error("MainWindow.xaml: the Sessions row's host must sit directly after the profile, ahead of the security rows")
	} else if strings.Contains(markup[profile+1:sessions], "<StackPanel") || strings.Contains(markup[profile+1:sessions], "<Border") {
		t.Error("MainWindow.xaml: nothing may sit between the profile and the Sessions row")
	}

	project := readAppSource(t, "App.vcxproj")
	for _, item := range []string{`<ClCompile Include="SessionsPage.cpp" />`, `<ClInclude Include="SessionsPage.h" />`,
		`<ClInclude Include="SessionsPresentation.h" />`, `<ClInclude Include="SessionGlyphs.h" />`,
		`<ClInclude Include="RelativeTimeSpan.h" />`} {
		if !strings.Contains(project, item) {
			t.Errorf("App.vcxproj does not list %s", item)
		}
	}
}

// RelativeTime reaches days and a date through RelativeTimeSpan.h's buckets,
// the date the user's short date and the spoken form the long date and time,
// both in the user's time zone.
func TestRelativeTimeReachesDaysAndADate(t *testing.T) {
	stats := stripComments(readAppSource(t, "StatsFormat.cpp"))
	relative := definitionBody(t, "StatsFormat.cpp", stats, "std::string RelativeTime(int64_t thenMillis, int64_t nowMillis) {")
	for _, want := range []string{"relativetime::SpanFor(thenMillis, nowMillis)", "relativetime::KeyFor(span.unit)",
		"case relativetime::Unit::Date:", "return FormatLocalDate(thenMillis);"} {
		if !strings.Contains(relative, want) {
			t.Errorf("RelativeTime: missing %s", want)
		}
	}
	if regexp.MustCompile(`"(?:now|seconds_ago_abbrev|minutes_ago_abbrev|hours_ago_abbrev)"`).MatchString(relative) {
		t.Error("RelativeTime names its keys itself; they are RelativeTimeSpan.h's KeyFor")
	}
	date := definitionBody(t, "StatsFormat.cpp", stats, "std::string FormatLocalDate(int64_t unixMillis) {")
	for _, want := range []string{"LocalSystemTime(unixMillis, local)", "GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE"} {
		if !strings.Contains(date, want) {
			t.Errorf("FormatLocalDate: missing %s", want)
		}
	}
	dateTime := definitionBody(t, "StatsFormat.cpp", stats, "std::string FormatLocalDateTime(int64_t unixMillis) {")
	for _, want := range []string{"DATE_LONGDATE", "GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS"} {
		if !strings.Contains(dateTime, want) {
			t.Errorf("FormatLocalDateTime: missing %s", want)
		}
	}
	local := definitionBody(t, "StatsFormat.cpp", stats, "bool LocalSystemTime(int64_t unixMillis, SYSTEMTIME& local) {")
	if !strings.Contains(local, "SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)") {
		t.Error("LocalSystemTime no longer converts to the user's time zone")
	}
}

// Every Api the app creates or replaces reports "windows" and this build's
// version, and so does the service's Api for each space it imports; the
// hard-coded "0.0.1" is gone from everything that reported it (the sdk's
// client info, the service's devices, the product events and the log
// uploads); and the Api's confirmed rejection of the account credential (a
// 401, or this session signed out from the Sessions page) runs the app's
// sign-out, as the device's does.
func TestSessionsClientInfoAndAppVersion(t *testing.T) {
	header := stripComments(readAppSource(t, "SdkHost.h"))
	if !strings.Contains(header, "std::string appVersion_ = urnw::version::kString;") {
		t.Error("SdkHost.h: appVersion_ must be this build's version (urnw::version::kString)")
	}
	if !strings.Contains(header, "#include \"Version.h\"") {
		t.Error("SdkHost.h must include Version.h for urnw::version::kString")
	}
	host := stripComments(readAppSource(t, "SdkHost.cpp"))
	bind := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::BindApiLocked() {")
	for _, want := range []string{
		"api_->setClientInfo(urnet::newClientInfo(kClientDeviceType, appVersion_));",
		"apiLogoutSub_.reset();",
		"apiLogoutSub_.emplace(api_->addAuthLogoutListener(",
		"if (loggedIn_.load(std::memory_order_acquire) && onAuthInvalid_) onAuthInvalid_();",
	} {
		if !strings.Contains(bind, want) {
			t.Errorf("SdkHost::BindApiLocked: missing %s", want)
		}
	}
	if !strings.Contains(host, `constexpr const char* kClientDeviceType = "windows";`) {
		t.Error(`SdkHost.cpp: the client info's device type must be "windows"`)
	}
	// every Api the host takes is bound at once: Initialize's and
	// ApplyNetworkServer's, and any later one
	assignments := regexp.MustCompile(`api_ = networkSpace_->getApi\(\);\s*BindApiLocked\(\);`).FindAllString(host, -1)
	if len(assignments) != 2 || strings.Count(host, "api_ = ") != 2 {
		t.Errorf("SdkHost.cpp binds %d of its %d Api assignments; every one must call BindApiLocked straight after",
			len(assignments), strings.Count(host, "api_ = "))
	}
	for _, signature := range []string{"bool SdkHost::Initialize() {", "bool SdkHost::ApplyNetworkServer("} {
		if !strings.Contains(definitionBody(t, "SdkHost.cpp", host, signature), "BindApiLocked();") {
			t.Errorf("%s does not bind the Api it creates", signature)
		}
	}
	if !strings.Contains(definitionBody(t, "SdkHost.cpp", host, "SdkHost::~SdkHost() {"), "apiLogoutSub_.reset();") {
		t.Error("SdkHost's destructor leaves the Api's logout listener subscribed")
	}
	// what the app version reaches
	for _, want := range []string{"cfg.app_version = appVersion_;", "request.app_version = appVersion_;",
		"std::make_unique<ClientEventQueue>(networkSpace_->handle(), appVersion_,"} {
		if !strings.Contains(host, want) {
			t.Errorf("SdkHost.cpp no longer sends %s", want)
		}
	}

	// the service's Api, which the app's SetClientInfo does not reach
	service := stripComments(readServiceSource(t, "TunnelController.cpp"))
	imported := definitionBody(t, "TunnelController.cpp", service, "urnet::NetworkSpace TunnelController::ImportNetworkSpaceLocked(")
	if !strings.Contains(imported, `space.getApi().setClientInfo(urnet::newClientInfo("windows", version::kString));`) {
		t.Error("TunnelController::ImportNetworkSpaceLocked must set the imported space's Api client info to windows and this build's version")
	}

	// no source sends the old placeholder version
	for _, file := range []string{"SdkHost.h", "SdkHost.cpp"} {
		if strings.Contains(stripComments(readAppSource(t, file)), `"0.0.1"`) {
			t.Errorf("%s still carries the placeholder version 0.0.1", file)
		}
	}
	for _, file := range []string{"TunnelController.cpp", "main.cpp"} {
		if strings.Contains(stripComments(readServiceSource(t, file)), `"0.0.1"`) {
			t.Errorf("Service/%s carries the placeholder version 0.0.1", file)
		}
	}
}

// Every row of the Account list leads with a mark, as android's and apple's
// do: a Segoe Fluent glyph, the closest meaning to the other apps' mark for
// the same row, and the Sessions row's head-outline path. Each builder of a
// row in that list passes one, so a row added without a mark fails here.
func TestAccountRowsLeadWithAMark(t *testing.T) {
	// the marks, by row
	settings := stripComments(readAppSource(t, "SettingsPage.cpp"))
	account := stripComments(readAppSource(t, "AccountPage.cpp"))
	for _, constant := range []struct{ source, name, glyph string }{
		{settings, "kSignInMethodGlyph", `\uE8D7`},  // Permissions, a key: login methods and each method
		{settings, "kAuthCodeGlyph", `\uE75F`},      // Dialpad: the auth code
		{settings, "kClientIdGlyph", `\uE8EC`},      // Tag: the client id
		{settings, "kSignOutGlyph", `\uF3B1`},       // SignOut
		{settings, "kDeleteAccountGlyph", `\uE74D`}, // Delete
		{account, "kPasswordGlyph", `\uE72E`},       // Lock: the password
		{account, "kReferralsGlyph", `\uEB51`},      // Heart: android's and apple's referrals mark
	} {
		if !strings.Contains(constant.source, "constexpr wchar_t "+constant.name+`[] = L"`+constant.glyph+`";`) {
			t.Errorf("%s must be %s", constant.name, constant.glyph)
		}
	}

	// every row-kit call in the Account list's builders passes a mark: the
	// rows kit's as its second argument (after the host), the pane kit's as its
	// first
	rowCall := regexp.MustCompile(`\b(Row|ButtonRow|NavRow|ValueActionRow|MakePaneTwoLineRow|MakePaneTwoLineRowButton)\(`)
	builders := []struct{ name, source, signature string }{
		{"SettingsPage.cpp", settings, "void SettingsPage::BuildSecuritySection(Panel const& host) {"},
		{"SettingsPage.cpp", settings, "void SettingsPage::RenderAuthMethods(rows::FieldState state) {"},
		{"SettingsPage.cpp", settings, "void SettingsPage::BuildDangerSection() {"},
		{"AccountPage.cpp", account, "void AccountPage::BuildProfileExtra() {"},
		{"AccountPage.cpp", account, "void AccountPage::BuildSessionsNav() {"},
		{"AccountPage.cpp", account, "void AccountPage::BuildReferralsNav() {"},
	}
	calls := 0
	for _, builder := range builders {
		body := definitionBody(t, builder.name, builder.source, builder.signature)
		for _, match := range rowCall.FindAllStringSubmatchIndex(body, -1) {
			calls++
			name := body[match[2]:match[3]]
			arguments := sessionsCallArguments(body, match[1]-1)
			index := 1
			if strings.HasPrefix(name, "MakePaneTwoLineRow") {
				index = 0
			}
			mark := ""
			if index < len(arguments) {
				mark = arguments[index]
			}
			if !strings.HasPrefix(mark, "kit::MakeRowGlyph(") && !strings.HasPrefix(mark, "kit::MakeRowPathIcon(") {
				t.Errorf("%s: %s(...) builds an Account row with no leading mark (%q)", builder.signature, name, mark)
			}
		}
	}
	if calls < 9 {
		t.Errorf("found only %d Account row builders' calls; update this contract", calls)
	}
	sessionsNav := definitionBody(t, "AccountPage.cpp", account, "void AccountPage::BuildSessionsNav() {")
	if !strings.Contains(sessionsNav, "kit::MakeRowPathIcon(glyph::kSessionFaceProfilePath, glyph::kSessionGlyphViewBox)") {
		t.Error("the Sessions row must lead with the head-outline path (SessionGlyphs.h)")
	}

	// a load renders the sign-in methods long after the section was built: as
	// pane rows, with their mark, whatever the row kit's mode is then
	render := definitionBody(t, "SettingsPage.cpp", settings, "void SettingsPage::RenderAuthMethods(rows::FieldState state) {")
	scope := strings.Index(render, "const PaneModeScope paneMode;")
	clear := strings.Index(render, "authMethodsPanel_.Children().Clear();")
	if scope < 0 || clear < 0 || scope > clear {
		t.Error("SettingsPage::RenderAuthMethods must render in pane mode from its first row (PaneModeScope)")
	}
	if strings.Contains(render, "rows::PaneMode()") {
		t.Error("SettingsPage::RenderAuthMethods still branches on the caller's row mode")
	}

	// the markup rows: the profile's name (Contact, the person android's and
	// apple's Profile rows show) and the sign-in it is reached by (ContactInfo)
	markup := readAppSource(t, "MainWindow.xaml")
	for _, row := range []struct{ name, glyph string }{
		{`<Button x:Name="NetworkNameRow"`, "&#xE77B;"},
		{`<Border x:Name="AccountAuthRow"`, "&#xE779;"},
	} {
		at := strings.Index(markup, row.name)
		if at < 0 {
			t.Fatalf("MainWindow.xaml: %s is gone; update this contract", row.name)
		}
		firstIcon := regexp.MustCompile(`<FontIcon Glyph="([^"]+)"\s+Style="\{StaticResource UrRowIconStyle\}"`).FindStringSubmatch(markup[at:])
		firstText := strings.Index(markup[at:], "<TextBlock")
		iconAt := strings.Index(markup[at:], "<FontIcon")
		if firstIcon == nil || firstIcon[1] != row.glyph || iconAt < 0 || iconAt > firstText {
			t.Errorf("MainWindow.xaml: %s must lead with the row mark %s", row.name, row.glyph)
		}
	}

	// the kit: a mark is the row icon's look, Raw for automation; a path fills
	// nonzero as its SVG does and is scaled from its box
	kit := stripComments(readAppSource(t, "UrComponents.cpp"))
	glyph := definitionBody(t, "UrComponents.cpp", kit, "IconElement MakeRowGlyph(winrt::hstring const& glyph) {")
	for _, want := range []string{`StyleByKey(L"UrRowIconStyle")`, "AccessibilityView::Raw"} {
		if !strings.Contains(glyph, want) {
			t.Errorf("kit::MakeRowGlyph: missing %s", want)
		}
	}
	path := definitionBody(t, "UrComponents.cpp", kit, "PathIcon MakeRowPathIcon(")
	for _, want := range []string{"Data='F1 ", "scale.ScaleX(size / viewBox);", "icon.Data().Transform(scale);",
		"urnw::colors::MutedBrush()", "AccessibilityView::Raw"} {
		if !strings.Contains(path, want) {
			t.Errorf("kit::MakeRowPathIcon: missing %s", want)
		}
	}
}

// The top-level arguments of the call whose "(" is at `open`, trimmed; string
// and character literals are read whole, so a "," or ")" in one splits nothing.
func sessionsCallArguments(code string, open int) []string {
	arguments := []string{}
	depth := 0
	start := open + 1
	for at := open; at < len(code); at++ {
		switch character := code[at]; {
		case character == '"' || character == '\'' && opensCharacterLiteral(code, at):
			for at++; at < len(code) && code[at] != character; at++ {
				if code[at] == '\\' {
					at++
				}
			}
		case character == '(' || character == '[' || character == '{':
			depth++
		case character == ')' || character == ']' || character == '}':
			depth--
			if depth == 0 {
				return append(arguments, strings.TrimSpace(code[start:at]))
			}
		case character == ',' && depth == 1:
			arguments = append(arguments, strings.TrimSpace(code[start:at]))
			start = at + 1
		}
	}
	return arguments
}

// A session with no country takes the colour this app already gives an
// unknown country: the provider globe's.
func TestSessionsUnknownCountryColourIsTheGlobes(t *testing.T) {
	globe := readAppSource(t, "ProviderGlobe.cpp")
	globeColour := regexp.MustCompile(`kUnknownCountryColor\{255, (0x[0-9A-Fa-f]{2}), (0x[0-9A-Fa-f]{2}), (0x[0-9A-Fa-f]{2})\}`).FindStringSubmatch(globe)
	sessions := readAppSource(t, "SessionsPresentation.h")
	sessionsColour := regexp.MustCompile(`kUnknownCountryRgb\{(0x[0-9A-Fa-f]{2}), (0x[0-9A-Fa-f]{2}), (0x[0-9A-Fa-f]{2})\}`).FindStringSubmatch(sessions)
	if globeColour == nil || sessionsColour == nil {
		t.Fatal("cannot read the unknown-country colours; update this contract")
	}
	for i := 1; i <= 3; i++ {
		if !strings.EqualFold(globeColour[i], sessionsColour[i]) {
			t.Errorf("the sessions' unknown-country colour %v is not the globe's %v", sessionsColour[1:], globeColour[1:])
			break
		}
	}
}

// The icons the Sessions screen draws are Material Design Icons, Apache-2.0:
// the notices the MSI installs carry their license.
func TestSessionsIconsLicenseNotice(t *testing.T) {
	root := repositoryRoot(t)
	notices, err := os.ReadFile(filepath.Join(root, "app", "THIRD-PARTY-NOTICES.txt"))
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"Material Design Icons 7.4.47 - Pictogrammers", "Pictogrammers Free License",
		"Apache License", "TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION", "SessionGlyphs.h"} {
		if !strings.Contains(string(notices), want) {
			t.Errorf("THIRD-PARTY-NOTICES.txt is missing %q", want)
		}
	}
	glyphs := readAppSource(t, "SessionGlyphs.h")
	if count := strings.Count(glyphs, "inline constexpr const wchar_t* k"); count != 9 {
		t.Errorf("SessionGlyphs.h carries %d paths; the notice names nine", count)
	}
}
