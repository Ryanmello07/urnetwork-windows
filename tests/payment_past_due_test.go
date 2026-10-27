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

// The past-due notice for a failed Stripe renewal: while the balance carries
// payment_past_due, the Account plan pane shows "Your UR Pro payment didn't go
// through..." with "Update payment method". That action opens ur.io's Manage
// Subscription signed in with a one-time auth code. The rules are
// App/PaymentPastDue.h, run here by app/tools/payment-past-due-tests.cpp. The
// sdk adapter App/PaymentPastDueSdk.h is built against a generated header that
// has payment_past_due wherever one is. The WinRT surfaces, which only a
// Windows build compiles, are read from their sources.

// The keys the notice looks up.
var paymentPastDueKeys = []string{"payment_past_due_notice", "payment_past_due_update_payment"}

// The source of a definition without comments and whitespace, so a statement
// reads the same however it is wrapped.
func pastDueCode(source string) string {
	return strings.Join(strings.Fields(stripComments(source)), "")
}

// Each of `wants`, in this order, in `body` (already through pastDueCode).
func requirePastDueOrder(t *testing.T, where, body string, wants ...string) {
	t.Helper()
	at := 0
	for _, want := range wants {
		next := strings.Index(body[at:], want)
		if next < 0 {
			if strings.Contains(body, want) {
				t.Errorf("%s: %s is out of order", where, want)
			} else {
				t.Errorf("%s: missing %s", where, want)
			}
			return
		}
		at += next + len(want)
	}
}

// Compile the past-due spec (app/tools/payment-past-due-tests.cpp,
// header-only). extra adds compiler arguments ahead of the source.
func buildPaymentPastDueTests(t *testing.T, extra ...string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("payment past due tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	appDir := filepath.Join(root, "app")
	program := filepath.Join(t.TempDir(), "payment-past-due-tests")
	arguments := []string{"-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + filepath.Join(appDir, "src", "App")}
	arguments = append(arguments, extra...)
	arguments = append(arguments, filepath.Join(appDir, "tools", "payment-past-due-tests.cpp"), "-o", program)
	if output, err := exec.Command(compiler, arguments...).CombinedOutput(); err != nil {
		t.Fatalf("build payment past due tests: %v\n%s", err, output)
	}
	return program
}

func runPaymentPastDueTests(t *testing.T, program string) {
	t.Helper()
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("payment past due: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The notice's rule (the field's presence, nothing else) and the action's url:
// the code percent-encoded as one query value, the bare url without a code,
// and ur.io's base always.
func TestPaymentPastDuePresentation(t *testing.T) {
	runPaymentPastDueTests(t, buildPaymentPastDueTests(t))
}

// The same spec with App/PaymentPastDueSdk.h built against a generated
// urnetwork_sdk.hpp that has payment_past_due: the server's balance json read
// through the adapter (the field, an older server's balance without it, null),
// the wire's update_payment_url kept out of the url, and the auth code's
// answers. The header is URNETWORK_SDK_INCLUDE, the vendored sdk
// (app/third_party/urnetwork-sdk/<arch>, git-ignored), or the sdk checkout
// beside this repository. A host with none of them, or only headers from
// before payment_past_due, skips.
func TestPaymentPastDueAgainstSdkHeader(t *testing.T) {
	root := repositoryRoot(t)
	sdkDir := paymentPastDueSdkHeaderDir(t, root)
	if sdkDir == "" {
		t.Skip("no urnetwork_sdk.hpp with payment_past_due (set URNETWORK_SDK_INCLUDE)")
	}
	jsonDir, found := jsonIncludeDir(root)
	if !found {
		t.Skip("no nlohmann/json.hpp (set URNETWORK_JSON_INCLUDE)")
	}
	extra := []string{"-DURNW_PAYMENT_PAST_DUE_TESTS_SDK", "-isystem", sdkDir}
	if jsonDir != "" {
		extra = append(extra, "-isystem", jsonDir)
	}
	t.Logf("against %s", filepath.Join(sdkDir, "urnetwork_sdk.hpp"))
	runPaymentPastDueTests(t, buildPaymentPastDueTests(t, extra...))
}

// The directory of a urnetwork_sdk.hpp that declares urnet::PaymentPastDue,
// or "".
func paymentPastDueSdkHeaderDir(t *testing.T, root string) string {
	t.Helper()
	explicit := os.Getenv("URNETWORK_SDK_INCLUDE")
	candidates := []string{}
	if explicit != "" {
		candidates = append(candidates, explicit)
	}
	for _, arch := range []string{"amd64", "arm64"} {
		candidates = append(candidates, filepath.Join(root, "app", "third_party", "urnetwork-sdk", arch))
	}
	// the monoroot: the sdk's own checkout beside this repository, when there is one
	candidates = append(candidates, filepath.Join(filepath.Dir(root), "sdk", "cgo", "include"))
	for _, dir := range candidates {
		header, err := os.ReadFile(filepath.Join(dir, "urnetwork_sdk.hpp"))
		if err != nil {
			if dir == explicit {
				t.Fatalf("URNETWORK_SDK_INCLUDE=%s: %v", explicit, err)
			}
			continue
		}
		if !strings.Contains(string(header), "struct PaymentPastDue {") {
			if dir == explicit {
				t.Logf("URNETWORK_SDK_INCLUDE=%s: urnet::PaymentPastDue is not declared", explicit)
				continue
			}
			t.Logf("%s: predates payment_past_due", dir)
			continue
		}
		return dir
	}
	return ""
}

// The new headers are part of the app build.
func TestPaymentPastDueIsInTheProject(t *testing.T) {
	project := readAppSource(t, "App.vcxproj")
	for _, want := range []string{
		`<ClInclude Include="PaymentPastDue.h" />`,
		`<ClInclude Include="PaymentPastDueSdk.h" />`,
	} {
		if !strings.Contains(project, want) {
			t.Errorf("App.vcxproj: missing %s", want)
		}
	}
}

// The balance store reads the field through the adapter into the snapshot;
// the window pushes the snapshot's view to the subscription section on every
// balance and after the section is built; the section builds the notice, a
// warning the user cannot close, above Manage Subscription and hidden until
// the view says so; and "Update payment method" asks for a one-use,
// five-minute code and opens ur.io's url built from that code alone, observing
// the launch.
func TestPaymentPastDueWiring(t *testing.T) {
	balance := stripComments(readAppSource(t, "SubscriptionBalance.cpp"))
	requirePastDueOrder(t, "SubscriptionBalanceStore::Apply",
		pastDueCode(definitionBody(t, "SubscriptionBalance.cpp", balance,
			"void SubscriptionBalanceStore::Apply(urnet::SubscriptionBalanceResult const& result)")),
		"snapshot_.paymentPastDue=pastdue::PastDueFromSdk(result);",
		"Publish();")
	if !strings.Contains(pastDueCode(readAppSource(t, "SubscriptionBalance.h")),
		"pastdue::PaymentPastDueViewpaymentPastDue;};") {
		t.Error("SubscriptionBalance.h: BalanceSnapshot does not carry the past-due view")
	}

	window := stripComments(readAppSource(t, "MainWindow.xaml.cpp"))
	requirePastDueOrder(t, "MainWindow::ApplyBalance",
		pastDueCode(definitionBody(t, "MainWindow.xaml.cpp", window, "void MainWindow::ApplyBalance(")),
		"if(settings_)settings_->ApplySubscriptionStore(balance_.subscriptionStoreFamily);",
		"if(settings_)settings_->ApplyPaymentPastDue(balance_.paymentPastDue);")
	// the section builds hidden on the first ApplyStrings: the view is replayed
	requirePastDueOrder(t, "MainWindow::ApplyStrings",
		pastDueCode(definitionBody(t, "MainWindow.xaml.cpp", window, "void MainWindow::ApplyStrings(")),
		"settings_->ApplyStrings();",
		"settings_->ApplyPaymentPastDue(balance_.paymentPastDue);")

	settings := stripComments(readAppSource(t, "SettingsPage.cpp"))
	body := func(signature string) string {
		return pastDueCode(definitionBody(t, "SettingsPage.cpp", settings, signature))
	}
	requirePastDueOrder(t, "SettingsPage::BuildSubscriptionSection", body("void SettingsPage::BuildSubscriptionSection("),
		"autocard=Card(host);",
		"pastDueNotice_=InfoBar();",
		"pastDueNotice_.Severity(InfoBarSeverity::Warning);",
		"pastDueNotice_.IsClosable(false);",
		"pastDueNotice_.IsOpen(false);",
		`pastDueNotice_.Message(Loc("payment_past_due_notice"));`,
		`pastDueUpdate_.Content(winrt::box_value(Loc("payment_past_due_update_payment")));`,
		`pastDueUpdate_,L"acceptance.account.payment-past-due-update");`,
		"pastDueUpdate_.Click([this](autoconst&,autoconst&){OpenUpdatePayment();});",
		"pastDueNotice_.ActionButton(pastDueUpdate_);",
		"pastDueNotice_.Visibility(Visibility::Collapsed);",
		"card.Children().Append(pastDueNotice_);",
		`manageSubscription_=NavRow(card,Loc("site_app_manage_subscription"),unused);`)
	apply := body("void SettingsPage::ApplyPaymentPastDue(")
	requirePastDueOrder(t, "SettingsPage::ApplyPaymentPastDue", apply,
		"if(!pastDueNotice_)return;",
		"constboolnotice=pastdue::NoticeShows(view);",
		"pastDueNotice_.IsOpen(notice);",
		"pastDueNotice_.Visibility(notice?Visibility::Visible:Visibility::Collapsed);")
	// the plan does not gate the notice: the server's field is the only rule
	for _, gate := range []string{"isPro", "storeFamily", "ShowsManageSubscription", "guest"} {
		if strings.Contains(apply, gate) {
			t.Errorf("SettingsPage::ApplyPaymentPastDue reads %s: the notice shows exactly while the balance carries payment_past_due", gate)
		}
	}

	open := body("winrt::fire_and_forget SettingsPage::OpenUpdatePayment(")
	requirePastDueOrder(t, "SettingsPage::OpenUpdatePayment", open,
		`if(!Sdk().apiReady()||!Sdk().IsLoggedIn()){LaunchUpdatePayment(pastdue::UpdatePaymentUrl(""));co_return;}`,
		"pastDueUpdate_.IsEnabled(false);",
		"Sdk().api().authCodeCreate(pastdue::UpdatePaymentAuthCodeArgs(),",
		"conststd::stringcode=pastdue::AuthCodeFromSdk(result,err);",
		"page.pastDueUpdate_.IsEnabled(true);",
		"page.LaunchUpdatePayment(pastdue::UpdatePaymentUrl(code));")
	launch := body("winrt::fire_and_forget SettingsPage::LaunchUpdatePayment(")
	requirePastDueOrder(t, "SettingsPage::LaunchUpdatePayment", launch,
		"launched=co_awaitwinrt::Windows::System::Launcher::LaunchUriAsync(winrt::Windows::Foundation::Uri(winrt::to_hstring(url)));",
		"if(launched)co_return;",
		`accountSnackbar_.Show(Loc("site_billing_portal_error"),InfoBarSeverity::Error);`)
	// the url and the code are a credential: never an argument of a log call
	// (the words in a log line's own text are not the values)
	logCall := regexp.MustCompile(`Log(Info|Warn|Error|Debug)\(([^;]*)\);`)
	literal := regexp.MustCompile(`"(?:[^"\\]|\\.)*"`)
	credential := regexp.MustCompile(`\b(url|code)\b`)
	for _, signature := range []string{
		"winrt::fire_and_forget SettingsPage::OpenUpdatePayment(",
		"winrt::fire_and_forget SettingsPage::LaunchUpdatePayment(",
	} {
		for _, call := range logCall.FindAllStringSubmatch(definitionBody(t, "SettingsPage.cpp", settings, signature), -1) {
			if credential.MatchString(literal.ReplaceAllString(call[2], `""`)) {
				t.Errorf("%s logs the url or the code: %s", signature, call[0])
			}
		}
	}
}

// The sdk's past-due surface is named in one place: PaymentPastDueSdk.h reads
// the balance's field and the auth code's answer. No source reads the wire's
// update_payment_url, and ur.io's url is spelled once, in PaymentPastDue.h, so
// a sign-in code can only ever be attached to it.
func TestPaymentPastDueSdkIsReadInOnePlace(t *testing.T) {
	for name, source := range appSourceFiles(t, ".cpp", ".h") {
		code := stripComments(source)
		for _, read := range []string{".payment_past_due", "urnet::PaymentPastDue", ".fail_time"} {
			if strings.Contains(code, read) && name != "PaymentPastDueSdk.h" {
				t.Errorf("%s names %s: the sdk's past-due field is read in PaymentPastDueSdk.h alone", name, read)
			}
		}
		if strings.Contains(code, "update_payment_url") {
			t.Errorf("%s reads update_payment_url: the action's url is pastdue::kManageSubscriptionUrl, never the wire's", name)
		}
		if strings.Contains(code, "ur.io/app/subscription") && name != "PaymentPastDue.h" {
			t.Errorf("%s spells ur.io's Manage Subscription url: it is pastdue::kManageSubscriptionUrl alone", name)
		}
		if strings.Contains(code, "pastdue::AuthCodeFromSdk(") && name != "SettingsPage.cpp" {
			t.Errorf("%s reads an auth code for the past-due action: only SettingsPage::OpenUpdatePayment asks for one", name)
		}
	}
}

// Both strings are looked up by the notice, are in the neutral catalog and
// tagged windows, and are translated in every language the app ships, with the
// plan's name kept as "UR Pro" and "Pro".
func TestPaymentPastDueStrings(t *testing.T) {
	root := repositoryRoot(t)
	settings := stripComments(readAppSource(t, "SettingsPage.cpp"))
	tagged, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "Strings", "windows-keys.txt"))
	if err != nil {
		t.Fatal(err)
	}
	taggedKeys := readCatalogTaggedKeys(string(tagged))
	for _, key := range paymentPastDueKeys {
		if !strings.Contains(settings, `Loc("`+key+`")`) {
			t.Errorf("the past-due notice never looks up %s", key)
		}
		if !taggedKeys[key] {
			t.Errorf("%s is not tagged windows", key)
		}
	}
	if got := reswValue(t, root, "en", "payment_past_due_notice"); got != "Your UR Pro payment didn't go through. Update your payment method to keep Pro." {
		t.Errorf("en payment_past_due_notice = %q", got)
	}
	if got := reswValue(t, root, "en", "payment_past_due_update_payment"); got != "Update payment method" {
		t.Errorf("en payment_past_due_update_payment = %q", got)
	}
	catalogs, err := filepath.Glob(filepath.Join(root, "app", "src", "App", "Strings", "*", "Resources.resw"))
	if err != nil {
		t.Fatal(err)
	}
	if len(catalogs) < 2 {
		t.Fatalf("found %d catalogs", len(catalogs))
	}
	for _, catalog := range catalogs {
		locale := filepath.Base(filepath.Dir(catalog))
		for _, key := range paymentPastDueKeys {
			value := reswValue(t, root, locale, key)
			if value == "" {
				t.Errorf("%s/Resources.resw has no %s", locale, key)
				continue
			}
			if locale != "en" && value == reswValue(t, root, "en", key) {
				t.Errorf("%s/Resources.resw: %s is the English text, not a translation", locale, key)
			}
		}
		notice := reswValue(t, root, locale, "payment_past_due_notice")
		if !strings.Contains(notice, "UR Pro") || strings.Count(notice, "Pro") < 2 {
			t.Errorf("%s/Resources.resw: payment_past_due_notice does not keep UR Pro and Pro: %q", locale, notice)
		}
	}
}

// The subscription rows live on the Account destination, so what they say
// goes on the window-level bar (AccountSnackbar). The page's own snackbar_ is
// on the Support destination, which is never on screen while the rows are, so
// a refused portal or a browser that did not open said nothing visible.
func TestSubscriptionRowsReportOnTheWindowLevelBar(t *testing.T) {
	settings := stripComments(readAppSource(t, "SettingsPage.cpp"))
	if !strings.Contains(pastDueCode(settings), "accountSnackbar_(window.AccountSnackbar(),window.DispatcherQueue())") {
		t.Error("SettingsPage's account snackbar is not on the window-level AccountSnackbar")
	}
	for _, signature := range []string{
		"winrt::fire_and_forget SettingsPage::OpenCustomerPortal(",
		"winrt::fire_and_forget SettingsPage::LaunchCustomerPortal(",
		"winrt::fire_and_forget SettingsPage::OpenUpdatePayment(",
		"winrt::fire_and_forget SettingsPage::LaunchUpdatePayment(",
	} {
		code := pastDueCode(definitionBody(t, "SettingsPage.cpp", settings, signature))
		// snackbar_ and settingsSnackbar() are the Support destination's bar
		// (accountSnackbar_ is spelled with a capital S, so it is not matched)
		for _, offScreen := range []string{"snackbar_.Show(", "settingsSnackbar().Show("} {
			if strings.Contains(code, offScreen) {
				t.Errorf("%s shows a message with %s, on the Support destination's bar", signature, offScreen)
			}
		}
	}
	requirePastDueOrder(t, "SettingsPage::OpenCustomerPortal",
		pastDueCode(definitionBody(t, "SettingsPage.cpp", settings, "winrt::fire_and_forget SettingsPage::OpenCustomerPortal(")),
		`accountSnackbar_.Show(Loc("please_login_to_urnetwork"),InfoBarSeverity::Warning);`)
	requirePastDueOrder(t, "SettingsPage::LaunchCustomerPortal",
		pastDueCode(definitionBody(t, "SettingsPage.cpp", settings, "winrt::fire_and_forget SettingsPage::LaunchCustomerPortal(")),
		`accountSnackbar_.Show(Loc("site_billing_portal_error"),InfoBarSeverity::Error);`)
}
