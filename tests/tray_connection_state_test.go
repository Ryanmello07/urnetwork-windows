// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"testing"
)

// What the tray says about the connection (App/TrayConnectionState.h) as a C++
// spec, with negative controls that rewrite a copy of the header, and the
// tray's use of it in AppController and TrayIcon, read with comments blanked.

// Compile the spec (app/tools/tray-connection-state-tests.cpp) against a copy
// of TrayConnectionState.h; `mutate`, when set, rewrites the copy.
func trayStateTestProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("tray state tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "TrayConnectionState.h"))
	if err != nil {
		t.Fatal(err)
	}
	content := string(source)
	if mutate != nil {
		content = mutate(content)
		if content == string(source) {
			t.Fatal("negative control did not change the production TrayConnectionState.h")
		}
	}
	if err := os.WriteFile(filepath.Join(includeDir, "TrayConnectionState.h"), []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	program := filepath.Join(includeDir, "tray-connection-state-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I"+includeDir,
		"-I"+filepath.Join(root, "app", "src", "Common"),
		filepath.Join(root, "app", "tools", "tray-connection-state-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build tray state tests: %v\n%s", err, output)
	}
	return program
}

// Execute the spec: Connected only with the tunnel up, the tunnel up reads its
// health, and the tunnel-down lines keep their order.
func TestTrayConnectionState(t *testing.T) {
	if !strings.Contains(readAppSource(t, "App.vcxproj"), `<ClInclude Include="TrayConnectionState.h" />`) {
		t.Error("App.vcxproj does not list TrayConnectionState.h")
	}
	program := trayStateTestProgram(t, nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("tray state: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The spec folds a stand-in for proto::TunnelState by its enumerators' names:
// they must be exactly Protocol.h's, so a new state fails here until the tray
// decides what it says.
func TestTrayConnectionStateFoldsEveryServiceState(t *testing.T) {
	protocol := stripComments(readCommonSource(t, "Protocol.h"))
	match := regexp.MustCompile(`enum class TunnelState \{([^}]*)\}`).FindStringSubmatch(protocol)
	if match == nil {
		t.Fatal("Protocol.h no longer declares enum class TunnelState; update this contract")
	}
	names := func(body string) []string {
		var out []string
		for _, part := range strings.Split(body, ",") {
			if name := strings.TrimSpace(part); name != "" {
				out = append(out, name)
			}
		}
		sort.Strings(out)
		return out
	}
	spec, err := os.ReadFile(filepath.Join(repositoryRoot(t), "app", "tools", "tray-connection-state-tests.cpp"))
	if err != nil {
		t.Fatal(err)
	}
	standIn := regexp.MustCompile(`enum class StandInTunnelState \{([^}]*)\}`).FindStringSubmatch(string(spec))
	if standIn == nil {
		t.Fatal("the spec no longer declares StandInTunnelState; update this contract")
	}
	if got, want := strings.Join(names(standIn[1]), ","), strings.Join(names(match[1]), ","); got != want {
		t.Errorf("the spec folds the states %s, Protocol.h has %s", got, want)
	}
	for _, name := range names(match[1]) {
		if !strings.Contains(string(spec), "StandInTunnelState::"+name+",") {
			t.Errorf("the spec does not fold the service state %s", name)
		}
	}
}

// The text of the first `case <status>:` in body through its break.
func trayStatusCase(t *testing.T, body, status string) string {
	t.Helper()
	opener := "case tray::Status::" + status + ":"
	start := strings.Index(body, opener)
	if start < 0 {
		t.Fatalf("AppController::UpdateTray has no %s; update this contract", opener)
	}
	end := strings.Index(body[start:], "break;")
	if end < 0 {
		t.Fatalf("AppController::UpdateTray: %s has no break", opener)
	}
	return body[start : start+end]
}

// AppController decides the tray's status, icon and menu item through the
// header, from the sign-in, the service's last status and the last stats
// push's health, and names each status with the connect page's or the tray's
// own string; TrayIcon offers the item only when the decision has one.
func TestTrayConnectionStateWiring(t *testing.T) {
	app := stripComments(readAppSource(t, "AppController.cpp"))
	update := definitionBody(t, "AppController.cpp", app, "void AppController::UpdateTray()")
	quitRequireSequence(t, "AppController::UpdateTray", update,
		regexp.QuoteMeta("const tray::View view = tray::Decide(CurrentTrayFacts());"),
		regexp.QuoteMeta("tray_.SetState(view.connectedIcon ? TrayState::NoProvideConnect"),
		regexp.QuoteMeta("switch (view.status) {"),
		regexp.QuoteMeta("tray_.SetTooltip(tip);"))
	quitForbid(t, "AppController::UpdateTray", update,
		"the decision reads the status; a second reading here can disagree with it",
		"lastTunnelStatus_", "connected_", "trayHealth_")
	for status, id := range map[string]string{
		"Connected":         `Localized("connected")`,
		"FindingProviders":  `"conn_finding_providers"`,
		"Degraded":          `"conn_degraded"`,
		"Connecting":        `Localized("connecting_status_indicator")`,
		"Failed":            `"conn_failed"`,
		"Disconnecting":     `"conn_tray_disconnecting"`,
		"BlockedKillSwitch": `"conn_tray_blocked_kill_switch"`,
		"BlockedNoTunnel":   `"conn_tray_blocked_no_tunnel"`,
		"FailsafeStopped":   `"conn_tray_failsafe_stopped"`,
	} {
		if !strings.Contains(trayStatusCase(t, update, status), id) {
			t.Errorf("AppController::UpdateTray: %s is not named %s", status, id)
		}
	}
	if strings.Contains(trayStatusCase(t, update, "None"), "status =") {
		t.Error("AppController::UpdateTray: no status must leave the app name alone")
	}

	facts := definitionBody(t, "AppController.cpp", app, "tray::Facts AppController::CurrentTrayFacts() const")
	for _, want := range []string{
		"facts.signedIn = authState_ == AuthState::LoggedIn;",
		"facts.statusKnown = lastTunnelStatus_.has_value();",
		"facts.tunnel = tray::TunnelOf(lastTunnelStatus_->state);",
		"facts.routesInstalled = lastTunnelStatus_->routes_installed;",
		"facts.wfpState = lastTunnelStatus_->wfp_state;",
		"facts.failsafeStop = proto::IsFailsafeStop(lastTunnelStatus_->stop_reason);",
		"facts.health = trayHealth_;",
		"facts.actionIsDisconnect = gesture::ActionIsDisconnect(CurrentServiceFacts(), TrayHealth());",
	} {
		if !strings.Contains(facts, want) {
			t.Errorf("AppController::CurrentTrayFacts: missing %s", want)
		}
	}

	toggle := quitSourceFrom(t, "AppController.cpp", app, "cb.onConnectToggle = [this] {", "cb.connectAction = ")
	quitRequireSequence(t, "the tray's connect toggle", toggle,
		regexp.QuoteMeta("const tray::Action action = tray::ActionFor(CurrentTrayFacts());"),
		regexp.QuoteMeta("if (action == tray::Action::None) return;"),
		regexp.QuoteMeta("urnw::balance::RouteConnectGesture(action == tray::Action::Disconnect,"))
	if !strings.Contains(app, "cb.connectAction = [this] { return tray::ActionFor(CurrentTrayFacts()); };") {
		t.Error("AppController.cpp: the menu's item is not the tray decision's")
	}
	if strings.Contains(app, "cb.isConnected") {
		t.Error("AppController.cpp: the tray still labels its item by a bare connected flag")
	}

	trayHeader := stripComments(readAppSource(t, "TrayIcon.h"))
	if !strings.Contains(trayHeader, "std::function<tray::Action()> connectAction;") ||
		strings.Contains(trayHeader, "isConnected") {
		t.Error("TrayIcon.h: the menu's item must come from the tray decision's action")
	}
	trayIcon := stripComments(readAppSource(t, "TrayIcon.cpp"))
	menu := definitionBody(t, "TrayIcon.cpp", trayIcon, "void TrayIcon::ShowContextMenu(POINT pt)")
	quitRequireSequence(t, "TrayIcon::ShowContextMenu", menu,
		regexp.QuoteMeta("const tray::Action action = cb_.connectAction ? cb_.connectAction() : tray::Action::None;"),
		regexp.QuoteMeta("if (action != tray::Action::None) {"),
		regexp.QuoteMeta("::AppendMenuW(menu, MF_STRING, kMenuConnect,"),
		regexp.QuoteMeta(`Localized(action == tray::Action::Disconnect ? "disconnect" : "connect")`))
}
