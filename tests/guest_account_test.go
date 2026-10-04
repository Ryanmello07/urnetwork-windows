// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// functionBody returns the body of the function whose definition starts with
// signature (braces balanced from the first '{' after it), or "".
func functionBody(source string, signature string) string {
	at := strings.Index(source, signature)
	if at < 0 {
		return ""
	}
	open := strings.Index(source[at:], "{")
	if open < 0 {
		return ""
	}
	open += at
	depth := 0
	for i := open; i < len(source); i++ {
		switch source[i] {
		case '{':
			depth++
		case '}':
			depth--
			if depth == 0 {
				return source[open : i+1]
			}
		}
	}
	return ""
}

// Compile and execute the legacy guest conversion spec (App/GuestConversion.h,
// UPGRADE.md A4/D8): a refreshed guest (jwt claim gone, server `guest` set) is
// still a guest, and the conversion adds a sign-in to the guest's own network,
// refreshes, sends and checks the code, and never signs out.
func TestGuestConversion(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("guest conversion tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "guest-conversion-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "App"),
		filepath.Join(root, "app", "tools", "guest-conversion-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build guest conversion tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("guest conversion: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// A legacy guest network (UPGRADE.md D8, A4, S6): the server removed the guest
// upgrade routes and the SDK's UpgradeGuest now always fails, so nothing may
// call it (or create guest networks). Every upgrade and create-account entry
// for a guest converts the network in place (GuestConversionSheet: add a
// sign-in, verify it) and never signs out, and no checkout opens for a guest.
// Who is a guest includes the server's `guest`, since a refreshed jwt has lost
// its GuestMode claim.
func TestLegacyGuestAccountFlow(t *testing.T) {
	root := repositoryRoot(t)
	appDir := filepath.Join(root, "app", "src", "App")
	read := func(name string) string {
		source, err := os.ReadFile(filepath.Join(appDir, name))
		if err != nil {
			t.Fatal(err)
		}
		return string(source)
	}

	entries, err := os.ReadDir(appDir)
	if err != nil {
		t.Fatal(err)
	}
	for _, entry := range entries {
		name := entry.Name()
		if !strings.HasSuffix(name, ".cpp") && !strings.HasSuffix(name, ".h") &&
			!strings.HasSuffix(name, ".xaml") {
			continue
		}
		source := read(name)
		for _, dead := range []string{"upgradeGuest(", "UpgradeGuest(", "LoginAsGuest", "guest_mode = true", "GuestModeSheet", "OfferGuestSignOut"} {
			if strings.Contains(source, dead) {
				t.Errorf("%s: still has %s", name, dead)
			}
		}
	}

	login := read("LoginPage.cpp")
	open := functionBody(login, "winrt::fire_and_forget LoginPage::OpenGuestConversion(")
	if open == "" {
		t.Fatal("LoginPage.cpp: no OpenGuestConversion")
	}
	if !strings.Contains(open, "urnw::GuestConversionSheet::Create(") {
		t.Error("OpenGuestConversion: does not open the conversion sheet")
	}
	if strings.Contains(open, "Logout") {
		t.Error("OpenGuestConversion: signs out")
	}
	menu := functionBody(login, "void LoginPage::OnAccountMenu(")
	if !strings.Contains(menu, "OpenGuestConversion()") {
		t.Error("the account menu's Create account does not open the guest conversion")
	}
	if !strings.Contains(menu, "Balance().Current().guest") {
		t.Error("the account menu reads only the jwt claim, which a refresh clears")
	}

	// the sheet adds and verifies on this network, and never signs in or out
	sheets := read("SettingsSheets.cpp")
	session := functionBody(sheets, "class SdkGuestConversionSession")
	for _, want := range []string{"sdk_.api().addAuth(", "sdk_.api().authVerify(", "sdk_.RefreshJwt()", "balance_.Refresh()"} {
		if !strings.Contains(session, want) {
			t.Errorf("SdkGuestConversionSession: missing %s", want)
		}
	}
	for _, unwanted := range []string{"Logout", "sdk_.VerifyCode("} {
		if strings.Contains(session, unwanted) {
			t.Errorf("SdkGuestConversionSession: has %s", unwanted)
		}
	}

	window := read("MainWindow.xaml.cpp")
	for _, signature := range []string{
		"winrt::fire_and_forget MainWindow::ShowUpgradeSheet()",
		"winrt::fire_and_forget MainWindow::ShowUpgradeCheckout(bool yearly)",
		"void MainWindow::OnOpenUpgrade(",
	} {
		body := functionBody(window, signature)
		if !strings.Contains(body, "if (balance_.guest)") ||
			!(strings.Contains(body, "OpenGuestConversion()") || strings.Contains(body, "DivertGuestToConversion(")) {
			t.Errorf("%s: a guest is not diverted to the conversion", signature)
		}
	}

	// the balance store reads the server's guest, not only the jwt claim
	store := read("SubscriptionBalance.cpp")
	for _, want := range []string{
		"serverGuest_ = result.guest.value_or(false);",
		"snapshot_.guest = IsGuestNetwork(jwtGuest_, serverGuest_);",
	} {
		if !strings.Contains(store, want) {
			t.Errorf("SubscriptionBalance.cpp: missing %s", want)
		}
	}
	if strings.Contains(store, "snapshot_.guest = jwt->GuestMode;") {
		t.Error("SubscriptionBalance.cpp: guest is still the jwt claim alone")
	}

	resources := read(filepath.Join("Strings", "en", "Resources.resw"))
	for _, key := range []string{"guest_convert_explanation", "sign_in_method_added_successfully"} {
		if !strings.Contains(resources, `name="`+key+`"`) {
			t.Errorf("en Resources.resw: missing %s", key)
		}
	}
	for _, key := range []string{"guest_sign_out_balance_warning", "guest_sign_out_and_create_account"} {
		if strings.Contains(resources, `name="`+key+`"`) {
			t.Errorf("en Resources.resw: still has %s", key)
		}
	}
}

// The network label (Connect, the status strip, the account menu) follows the
// same guest rule as the purchase gates: the jwt claim OR the balance's guest.
// The claim alone labels a refreshed guest (the refresh drops the claim) by
// its network name, so the label is re-applied when the balance's guest
// changes.
func TestGuestNetworkIdentity(t *testing.T) {
	root := repositoryRoot(t)
	source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "MainWindow.xaml.cpp"))
	if err != nil {
		t.Fatal(err)
	}
	window := string(source)

	auth := functionBody(window, "void MainWindow::ApplyAuthState(")
	if auth == "" {
		t.Fatal("MainWindow.xaml.cpp: no ApplyAuthState")
	}
	for _, claimOnly := range []string{"SetNetworkIdentity(networkName, guestMode)", "statusGuest_ = guestMode", "ApplyAccountIdentity(networkName, guestMode"} {
		if strings.Contains(auth, claimOnly) {
			t.Errorf("ApplyAuthState: labels the network from the jwt claim alone (%s)", claimOnly)
		}
	}
	if !strings.Contains(auth, "ApplyNetworkIdentity()") {
		t.Error("ApplyAuthState: does not apply the network identity")
	}

	identity := functionBody(window, "void MainWindow::ApplyNetworkIdentity()")
	if identity == "" {
		t.Fatal("MainWindow.xaml.cpp: no ApplyNetworkIdentity")
	}
	if !strings.Contains(identity, "urnw::IsGuestNetwork(identityJwtGuest_, balance_.guest)") {
		t.Error("ApplyNetworkIdentity: guest is not the claim or the balance's guest")
	}
	for _, want := range []string{
		"connect_->SetNetworkIdentity(identityNetworkName_, guest)",
		"statusGuest_ = guest;",
		"login_->ApplyAccountIdentity(identityNetworkName_, guest,",
	} {
		if !strings.Contains(identity, want) {
			t.Errorf("ApplyNetworkIdentity: missing %s", want)
		}
	}

	balance := functionBody(window, "void MainWindow::OnBalanceChanged(")
	if !strings.Contains(balance, "if (guestChanged) ApplyNetworkIdentity();") {
		t.Error("OnBalanceChanged: a change in the server's guest does not relabel the network")
	}
}

// The conversion sheet's Resend follows the rate limit the way the login
// verify step does (GuestConversion's ResendCooldown, pinned in
// guest-conversion-tests.cpp): off until the retry time, with the notice
// re-rendered every second to count the minutes down.
func TestGuestConversionResendCooldownWiring(t *testing.T) {
	root := repositoryRoot(t)
	source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "SettingsSheets.cpp"))
	if err != nil {
		t.Fatal(err)
	}
	sheets := string(source)
	render := functionBody(sheets, "void GuestConversionSheet::Render()")
	if render == "" {
		t.Fatal("SettingsSheets.cpp: no GuestConversionSheet::Render")
	}
	for _, want := range []string{
		"dialog_.IsSecondaryButtonEnabled(conversion_->CanResend());",
		"if (conversion_->CoolingDown())",
		"cooldownTimer_.Start();",
	} {
		if !strings.Contains(render, want) {
			t.Errorf("GuestConversionSheet::Render: missing %s", want)
		}
	}
	build := functionBody(sheets, "void GuestConversionSheet::Build(")
	if !strings.Contains(build, "cooldownTimer_.Interval(std::chrono::seconds(1));") {
		t.Error("GuestConversionSheet::Build: no one-second countdown tick")
	}
	session := functionBody(sheets, "class SdkGuestConversionSession")
	if !strings.Contains(session, "Now() override { return ResendCooldown::Clock::now(); }") {
		t.Error("SdkGuestConversionSession: no steady clock for the cooldown")
	}
}

// A purchase entry that sent a guest to the conversion continues to the
// checkout it was opening once the conversion is done and the guest clears
// (GuestUpgradeContinuation, pinned in guest-conversion-tests.cpp); it used to
// close back to where the user started. The plan card's "Create an account"
// asked only for the conversion and does not continue.
func TestGuestPurchaseContinuesAfterConversion(t *testing.T) {
	root := repositoryRoot(t)
	appDir := filepath.Join(root, "app", "src", "App")
	read := func(name string) string {
		source, err := os.ReadFile(filepath.Join(appDir, name))
		if err != nil {
			t.Fatal(err)
		}
		return string(source)
	}
	window := read("MainWindow.xaml.cpp")
	for _, signature := range []string{
		"winrt::fire_and_forget MainWindow::ShowUpgradeSheet()",
		"winrt::fire_and_forget MainWindow::ShowUpgradeCheckout(bool yearly)",
	} {
		if !strings.Contains(functionBody(window, signature), "DivertGuestToConversion(") {
			t.Errorf("%s: a converted guest does not continue to its checkout", signature)
		}
	}
	if !strings.Contains(functionBody(window, "void MainWindow::OnOpenUpgrade("), "login_->OpenGuestConversion();") {
		t.Error("OnOpenUpgrade: the plan card's Create an account should open only the conversion")
	}
	divert := functionBody(window, "void MainWindow::DivertGuestToConversion(")
	for _, want := range []string{
		"guestUpgrade_.Divert(std::move(checkout));",
		"if (done) self->guestUpgrade_.ConversionDone();",
		"self->guestUpgrade_.ConversionClosed();",
		"self->guestUpgrade_.Poll(self->balance_.guest);",
	} {
		if !strings.Contains(divert, want) {
			t.Errorf("DivertGuestToConversion: missing %s", want)
		}
	}
	if !strings.Contains(functionBody(window, "void MainWindow::OnBalanceChanged("), "guestUpgrade_.Poll(balance_.guest);") {
		t.Error("OnBalanceChanged: the continuation does not wait for the guest to clear")
	}
	open := functionBody(read("LoginPage.cpp"), "winrt::fire_and_forget LoginPage::OpenGuestConversion(")
	if !strings.Contains(open, "if (onClosed) onClosed(*done);") {
		t.Error("OpenGuestConversion: does not report the close")
	}
}
