// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The call sites of the owner's 2026-10-05 decision "autostart on system start
// should launch only the tray icon": every launch the user starts opens the
// window, and an autostart shows only the tray icon. What each launch asks is
// decided by instance::ActionFor (instance_handover_test.go); OnLaunched,
// AppController and the activation code need Windows and WinRT, so these read
// their sources with every comment blanked.

// One rule for every launch. AppController::ServeLaunch asks ActionFor and
// does what it says: route a deep link, open the window for the user's launch,
// nothing for an autostart. The instance's own launch is served that way once
// the controller is up, before the gate opens, and every launch the gate
// serves (a relaunch of the running instance, one held while it started) goes
// through the same call. A launch that starts the app after waiting out a
// quitting instance is that instance's own launch.
func TestLaunchWindowEveryLaunchIsServedAsItAsks(t *testing.T) {
	controller := appControllerSource(t)
	serve := definitionBody(t, "AppController.cpp", controller,
		"void AppController::ServeLaunch(const instance::LaunchRequest& request) {")
	signOutRequireInOrder(t, "AppController::ServeLaunch", serve,
		regexp.QuoteMeta("const instance::LaunchAction action = instance::ActionFor(request);"),
		regexp.QuoteMeta("case instance::LaunchAction::HandleDeepLink:"),
		regexp.QuoteMeta("HandleDeepLink(request.deepLink);"),
		regexp.QuoteMeta("case instance::LaunchAction::ShowWindow:"),
		regexp.QuoteMeta("ShowWindow(nullptr);"),
		regexp.QuoteMeta("case instance::LaunchAction::TrayOnly:"),
		regexp.QuoteMeta("return;"))
	trayOnly := quitSourceFrom(t, "AppController::ServeLaunch", serve,
		"case instance::LaunchAction::TrayOnly:", "return;")
	quitForbid(t, "ServeLaunch's tray-only case", trayOnly,
		"an autostart shows only the tray icon", "ShowWindow(", "HandleDeepLink(")

	app := stripComments(readAppSource(t, "App.xaml.cpp"))
	launched := definitionBody(t, "App.xaml.cpp", app, "void App::OnLaunched(LaunchActivatedEventArgs const&) {")
	signOutRequireInOrder(t, "App::OnLaunched", launched,
		regexp.QuoteMeta("urnw::SetApp(std::move(controller));"),
		regexp.QuoteMeta("urnw::App().ServeLaunch(urnw::OwnLaunchRequest());"),
		regexp.QuoteMeta("urnw::Activations().Open("),
		regexp.QuoteMeta("[](const urnw::instance::LaunchRequest& request) { urnw::App().ServeLaunch(request); });"))
	quitForbid(t, "App::OnLaunched", launched,
		"every launch goes through ServeLaunch's one rule", "ShowWindow(", "HandleDeepLink(")

	// the own launch: its deep link, and whether an autostart started it
	own := definitionBody(t, "AppController.cpp", controller, "instance::LaunchRequest OwnLaunchRequest() {")
	provideRequire(t, "OwnLaunchRequest", own,
		"DeepLinkFromActivation(", "GetActivatedEventArgs()", ".autostart = LaunchedByAutostart()",
		"if (request.deepLink.empty()) request.deepLink = LaunchDeepLink();")
	glue := stripComments(readAppSource(t, "SingleInstance.cpp"))
	provideRequire(t, "LaunchedByAutostart", definitionBody(t, "SingleInstance.cpp", glue, "bool LaunchedByAutostart() {"),
		"return instance::HasArgument(::GetCommandLineW(), instance::kAutostartArgument);")

	// a redirected launch: the other process's command line says the same
	redirected := definitionBody(t, "AppController.cpp", controller,
		"instance::LaunchRequest LaunchRequestFromActivation(")
	signOutRequireInOrder(t, "LaunchRequestFromActivation", redirected,
		regexp.QuoteMeta("instance::LaunchRequest request{.deepLink = DeepLinkFromActivation(args)};"),
		regexp.QuoteMeta("args.Kind() == lifecycle::ExtendedActivationKind::Launch"),
		regexp.QuoteMeta("request.autostart ="),
		regexp.QuoteMeta("instance::HasArgument(launchArgs.Arguments(), instance::kAutostartArgument);"))
	take := definitionBody(t, "main.cpp", appMainSource(t),
		"void TakeRedirectedLaunch(AppActivationArguments const& redirected) {")
	signOutRequireInOrder(t, "TakeRedirectedLaunch", take,
		regexp.QuoteMeta("urnw::instance::LaunchRequest request = urnw::LaunchRequestFromActivation(redirected);"),
		regexp.QuoteMeta("urnw::Activations().Take("),
		regexp.QuoteMeta("std::move(request),"))

	// ServeLaunch is the one place a launch opens the window
	for name, source := range appSourceFiles(t, ".cpp") {
		code := stripComments(source)
		if strings.Contains(code, "OwnLaunchRequest()") && name != "AppController.cpp" && name != "App.xaml.cpp" {
			t.Errorf("%s reads the own launch outside App::OnLaunched", name)
		}
	}
}

// Nothing registers an autostart today: the installer has no Run key, Startup
// shortcut or scheduled task, and the app writes none (launch-at-login is an
// open M5 item). A registration added later must pass the autostart argument,
// or every sign-in would open the window, so this fails one that does not.
func TestLaunchWindowAutostartRegistrationsPassTheAutostartArgument(t *testing.T) {
	if !strings.Contains(stripComments(readCommonSource(t, "InstanceHandover.h")),
		`inline constexpr std::wstring_view kAutostartArgument = L"--autostart";`) {
		t.Fatal("InstanceHandover.h no longer names the autostart argument --autostart")
	}
	packagePath := filepath.Join(repositoryRoot(t), "app", "installer", "Package.wxs")
	data, err := os.ReadFile(packagePath)
	if err != nil {
		t.Fatal(err)
	}
	installer := regexp.MustCompile(`(?s)<!--.*?-->`).ReplaceAllString(string(data), "")
	for _, pattern := range []string{
		`(?is)<RegistryKey[^>]*CurrentVersion\\Run[^>]*>.*?</RegistryKey>`,
		`(?is)<RegistryValue[^>]*CurrentVersion\\Run[^>]*/>`,
		`(?is)<Directory[^>]*Id="StartupFolder".*?</Directory>`,
		`(?is)<StandardDirectory[^>]*Id="StartupFolder".*?</StandardDirectory>`,
		`(?is)<CustomAction[^>]*schtasks[^>]*/?>`,
	} {
		for _, registration := range regexp.MustCompile(pattern).FindAllString(installer, -1) {
			if !strings.Contains(registration, "--autostart") {
				t.Errorf("Package.wxs registers an autostart without --autostart:\n%s", registration)
			}
		}
	}
	for name, source := range appSourceFiles(t, ".cpp", ".h") {
		code := stripComments(source)
		for _, marker := range []string{`CurrentVersion\\Run`, "FOLDERID_Startup", "StartupTask", "schtasks"} {
			if strings.Contains(code, marker) && !strings.Contains(code, "kAutostartArgument") {
				t.Errorf("%s registers an autostart (%s) without instance::kAutostartArgument", name, marker)
			}
		}
	}
}
