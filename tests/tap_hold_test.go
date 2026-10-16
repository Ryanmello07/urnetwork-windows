// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// A tap's state held until the pass that ran it has published (App/TapHold.h,
// owner report 2026-10-16) as a C++ spec, with negative controls that rewrite a
// copy of the header, and the SdkHost, AppController, ConnectPage, MainWindow
// and tray lines that wire it, read with every comment blanked.

func tapHoldProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("tap hold tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "TapHold.h"))
	if err != nil {
		t.Fatal(err)
	}
	content := string(source)
	if mutate != nil {
		content = mutate(content)
		if content == string(source) {
			t.Fatal("negative control did not change the production TapHold.h")
		}
	}
	if err := os.WriteFile(filepath.Join(includeDir, "TapHold.h"), []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	program := filepath.Join(includeDir, "tap-hold-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I"+includeDir,
		filepath.Join(root, "app", "tools", "tap-hold-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build tap hold tests: %v\n%s", err, output)
	}
	return program
}

// The spec: a connect tap shows connecting and a disconnect tap disconnected
// at once, a push read before the pass ran the tap cannot flip them back, the
// push read after it replaces them, the newer tap wins, and a tap that goes
// without running never holds the view forever.
func TestTapHold(t *testing.T) {
	if !strings.Contains(readAppSource(t, "App.vcxproj"), `<ClInclude Include="TapHold.h" />`) {
		t.Error("App.vcxproj does not list TapHold.h")
	}
	if output, err := exec.Command(tapHoldProgram(t, nil)).CombinedOutput(); err != nil {
		t.Fatalf("tap hold: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

func requireTapHoldFailure(t *testing.T, old, replacement, want string) {
	t.Helper()
	program := tapHoldProgram(t, func(source string) string {
		if strings.Count(source, old) != 1 {
			t.Fatalf("TapHold.h no longer has exactly one %q; update this control", old)
		}
		return strings.Replace(source, old, replacement, 1)
	})
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

// The defect: every push applied as it came, so one read before the worker ran
// the tap flipped the view back.
func TestTapHoldRejectsApplyingEveryPush(t *testing.T) {
	requireTapHoldFailure(t,
		"  if (hold && Releases(*hold, settled)) return std::nullopt;\n  return hold;",
		"  (void)settled;\n  (void)hold;\n  return std::nullopt;",
		"a push read before the tap cannot flip it back")
}

// A settle that forgets the pass in flight ends the hold while the pass runs.
func TestTapHoldRejectsForgettingThePassInFlight(t *testing.T) {
	requireTapHoldFailure(t,
		"  if (inFlight != 0 && (oldest == 0 || inFlight < oldest)) oldest = inFlight;\n",
		"  (void)inFlight;\n",
		"a push read while the pass runs keeps it")
}

// One past the tap's own number would keep the hold through the pass's own
// publish.
func TestTapHoldRejectsReleasingOnePastTheTap(t *testing.T) {
	requireTapHoldFailure(t, "return hold.gesture <= settled;", "return hold.gesture < settled;",
		"the push read after the pass replaces it with what the tap left")
}

// A view that ignores its hold shows the reported status over the tap.
func TestTapHoldRejectsAViewThatIgnoresTheHold(t *testing.T) {
	requireTapHoldFailure(t,
		"  if (hold) return hold->kind == Kind::Connect ? connecting : disconnected;\n",
		"  (void)hold;\n  (void)disconnected;\n",
		"a disconnect tap shows disconnected at once")
}

// A late notice of an older tap must not replace a newer hold.
func TestTapHoldRejectsAnOlderTapReplacingANewer(t *testing.T) {
	requireTapHoldFailure(t, "  if (hold && tap.gesture < hold->gesture) return hold;\n", "  (void)hold;\n",
		"an older tap's notice does not replace a newer hold")
}

// SdkHost numbers the gesture it records and tells the views outside the
// slot's lock; the worker keeps the pass's gesture until just before its final
// publish; ReadStats stamps the settle point before it reads a status.
func TestTapHoldSdkHostWiring(t *testing.T) {
	host := stripComments(readAppSource(t, "SdkHost.cpp"))
	request := functionBody(host, "void SdkHost::RequestSession(")
	quitRequireSequence(t, "SdkHost::RequestSession", request,
		`\[&\] \{`, `std::scoped_lock lock\(pendingMutex_\);`,
		`if \(IsGesture\(request\.kind\)\) recordedGesture = request\.gesture = \+\+gestureIssued_;`,
		`pending_ = std::move\(request\);`, `\}\(\);`,
		`if \(recordedGesture != 0 && onGesture_\) onGesture_\(recordedGesture, tapKind\);`)
	worker := functionBody(host, "void SdkHost::SessionWorkerLoop(")
	quitRequireSequence(t, "SdkHost::SessionWorkerLoop", worker,
		`req = std::move\(pending_\);`, `gestureInFlight_ = req\.gesture;`,
		`ConnectLocked\(req\);`, `gestureInFlight_ = 0;`, `PublishStats\(\);\s*\}\s*\}$`)
	read := functionBody(host, "LiveStats SdkHost::ReadStats(")
	quitRequireSequence(t, "SdkHost::ReadStats", read,
		`s\.readSequence = `, `s\.gestureSettled = taphold::SettledThrough\(`,
		`gestureIssued_, pendingRequested_ \? pending_\.gesture : 0, gestureInFlight_\)`,
		`if \(connectVc_\) \{`)
	header := stripComments(readAppSource(t, "SdkHost.h"))
	if !strings.Contains(header, "return kind == ConnectKind::BestAvailable || kind == ConnectKind::Location ||\n           kind == ConnectKind::Selected || kind == ConnectKind::Disconnect;") {
		t.Error("SdkHost::IsGesture must count every connect kind and Disconnect, and nothing else")
	}
}

// The tray and the window take the tap inside the gesture, and a push read
// after it settled ends it; the page shows the start wait, the hold, or the
// reported status; a held connect reads connecting in the tray.
func TestTapHoldViewWiring(t *testing.T) {
	controller := stripComments(readAppSource(t, "AppController.cpp"))
	handler := handlerSource(t, "AppController.cpp", controller, "sdk_.SetGestureHandler(")
	quitRequireSequence(t, "the gesture handler", handler,
		`uiThread_\.HasThreadAccess\(\)`, `OnTapRecorded\(gesture, kind\);`, `OnUi\(`)
	recorded := functionBody(controller, "void AppController::OnTapRecorded(")
	quitRequireSequence(t, "AppController::OnTapRecorded", recorded,
		`trayTap_ = taphold::AfterTap\(`, `UpdateTray\(\);`, `self->OnTapRecorded\(gesture, kind\);`)
	onStats := functionBody(controller, "void AppController::OnStats(")
	quitRequireSequence(t, "AppController::OnStats", onStats,
		`statsReadOrder_\.Admit\(stats\.readSequence\)`,
		`trayTap_ = taphold::AfterSnapshot\(trayTap_, stats\.gestureSettled\);`, `UpdateTray\(\);`)
	facts := functionBody(controller, "tray::Facts AppController::CurrentTrayFacts(")
	quitRequireSequence(t, "AppController::CurrentTrayFacts", facts,
		`HeldTrayHealth\(\)`, `facts\.health = held \? held : trayHealth_;`,
		`facts\.connectHeld = held == health::State::Connecting;`)
	held := functionBody(controller, "std::optional<health::State> AppController::HeldTrayHealth(")
	quitRequireSequence(t, "AppController::HeldTrayHealth", held,
		`authState_ != AuthState::LoggedIn\) return std::nullopt;`)
	reconcile := functionBody(controller, "void AppController::ReconcileWindowPresentation(")
	quitRequireSequence(t, "ReconcileWindowPresentation", reconcile,
		`if \(trayTap_\) self->OnTapRecorded\(trayTap_->gesture, trayTap_->kind\);`)

	page := stripComments(readAppSource(t, "ConnectPage.cpp"))
	apply := functionBody(page, "void ConnectPage::ApplyStats(")
	quitRequireSequence(t, "ConnectPage::ApplyStats", apply,
		`reportedConnectStatus_ = ParseConnectStatus\(stats\.connectionStatus\);`,
		`tapHold_ = urnw::taphold::AfterSnapshot\(tapHold_, stats\.gestureSettled\);`,
		`reportedHealth_ = stats\.health;`, `ShowTapState\(\);`)
	quitForbid(t, "ConnectPage::ApplyStats", apply,
		"a push must not write the shown status past the hold",
		"connectStatus_ = ", "health_ = ")
	tapped := functionBody(page, "void ConnectPage::OnTapRecorded(")
	quitRequireSequence(t, "ConnectPage::OnTapRecorded", tapped,
		`tapHold_ = urnw::taphold::AfterTap\(`, `ShowTapState\(\);`, `ApplyConnectStatus\(\);`)
	show := functionBody(page, "void ConnectPage::ShowTapState(")
	quitRequireSequence(t, "ConnectPage::ShowTapState", show,
		`connectStatus_ = urnw::taphold::Shown\(startWaiting_, tapHold_, reportedConnectStatus_,`,
		`health_ = urnw::taphold::Shown\(startWaiting_, tapHold_, reportedHealth_,`)

	window := stripComments(readAppSource(t, "MainWindow.xaml.cpp"))
	forward := functionBody(window, "void MainWindow::OnTapRecorded(")
	quitRequireSequence(t, "MainWindow::OnTapRecorded", forward, `connect_->OnTapRecorded\(gesture, kind\);`)

	trayState := stripComments(readAppSource(t, "TrayConnectionState.h"))
	if !strings.Contains(trayState, "if (facts.tunnel == Tunnel::Starting || facts.connectHeld) {") {
		t.Error("TrayConnectionState.h does not read a held connect as a connect in progress")
	}
}
