// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"regexp"
	"strings"
	"testing"
)

// The call sites of the owner's 2026-10-05 decision "Windows sign-out: stop
// the tunnel and provider, the same as Quit": Windows ending the session
// reaches the same Shutdown and the same service stop as the tray's Quit. What
// each ending stops runs in app_lifetime_test.go; the window procedure and
// AppController need Windows, so these read their sources with every comment
// blanked.

// WM_ENDSESSION -> onSessionEnd -> Shutdown(SessionEnd) -> SdkHost::Quit. The
// tray's window takes the end of the session only when it is real (wParam)
// and not the Restart Manager closing the app (ENDSESSION_CLOSEAPP), stops
// before it returns, and leaves WM_QUERYENDSESSION to DefWindowProc, so the
// app never holds a sign-out or a shutdown up. The Restart Manager's close is
// a close request, as an installer's WM_CLOSE is: Windows Installer asks it,
// in the installing session, for every install that does not close the app
// itself (one run by hand), so those installs leave no file in use either.
func TestSessionEndStopsAsQuit(t *testing.T) {
	tray := stripComments(readAppSource(t, "TrayIcon.cpp"))
	wndProc := definitionBody(t, "TrayIcon.cpp", tray, "LRESULT CALLBACK TrayIcon::WndProc(")
	if count := strings.Count(wndProc, "case WM_ENDSESSION:"); count != 1 {
		t.Fatalf("TrayIcon::WndProc handles WM_ENDSESSION %d times, want once", count)
	}
	end := quitSourceFrom(t, "TrayIcon::WndProc", wndProc, "case WM_ENDSESSION:", "return 0;")
	signOutRequireInOrder(t, "TrayIcon::WndProc's WM_ENDSESSION case", end,
		regexp.QuoteMeta("if (wParam && !(lParam & ENDSESSION_CLOSEAPP)) {"),
		regexp.QuoteMeta("if (self->cb_.onSessionEnd) self->cb_.onSessionEnd();"),
		regexp.QuoteMeta("} else if (wParam) {"),
		regexp.QuoteMeta("if (self->cb_.onCloseRequest) self->cb_.onCloseRequest();"),
		regexp.QuoteMeta("}"),
		regexp.QuoteMeta("return 0;"))
	quitForbid(t, "TrayIcon::WndProc's WM_ENDSESSION case", end,
		"the end of a session stops as Quit does, through its own callback",
		"cb_.onQuit", "DefWindowProc")
	if split := strings.Index(end, "} else if (wParam) {"); split >= 0 {
		quitForbid(t, "TrayIcon::WndProc's session-end branch", end[:split],
			"the end of a session is no close request", "cb_.onCloseRequest")
		quitForbid(t, "TrayIcon::WndProc's Restart Manager branch", end[split:],
			"the Restart Manager closing the app for an installer stops nothing", "cb_.onSessionEnd")
	}
	if strings.Contains(wndProc, "WM_QUERYENDSESSION") {
		t.Error("TrayIcon::WndProc handles WM_QUERYENDSESSION: the app must not hold a " +
			"sign-out or a shutdown up")
	}
	header := stripComments(readAppSource(t, "TrayIcon.h"))
	provideRequire(t, "TrayIcon.h", header, "std::function<void()> onSessionEnd;")

	controller := appControllerSource(t)
	onSessionEnd := quitSourceFrom(t, "AppController::Start", controller,
		"cb.onSessionEnd = [this] {", "};")
	provideRequire(t, "the tray's session-end callback", onSessionEnd,
		"Shutdown(lifetime::Ending::SessionEnd);")
	quitForbid(t, "the tray's session-end callback", onSessionEnd,
		"Shutdown stops the service, once, after the window and the tray",
		"sdk_.", "OnUi(")
	// Shutdown asks the plan, and the plan stops the service for this ending
	// as for the Quit (app-lifetime-tests.cpp), through the one guarded call
	shutdown := definitionBody(t, "AppController.cpp", controller,
		"void AppController::Shutdown(lifetime::Ending ending) {")
	provideRequire(t, "AppController::Shutdown", shutdown,
		"const lifetime::Plan plan = lifetime::PlanFor(ending);",
		"if (plan.stopService) sdk_.Quit();")
}
