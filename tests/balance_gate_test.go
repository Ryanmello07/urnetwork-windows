// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// Compile and execute the insufficient-balance gate (Common/BalanceGate.h): the
// connect button keeps Disconnect out of balance, the balance reaction never
// disconnects, the notice is posted once per out-of-balance episode, and a
// connect gesture out of balance starts nothing while a live session is kept.
func TestBalanceGate(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("balance gate tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "balance-gate-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "Common"),
		filepath.Join(root, "app", "tools", "balance-gate-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build balance gate tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("balance gate: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Every connect entry point passes the start-connect gate (BalanceGate.h)
// before it records a session request, so out of balance the tray, a row or
// the connect button cannot start the tunnel. A reattach and Disconnect are
// never gated: a session that runs out of balance stays up.
func TestConnectEntryPointsAdmitStartConnect(t *testing.T) {
	host := stripLineComments(readAppSource(t, "SdkHost.cpp"))
	entries := []string{
		"void SdkHost::ConnectBestAvailable()",
		"void SdkHost::Connect(const std::string& connectLocationJson)",
		"void SdkHost::Connect(const urnet::ConnectLocation& location)",
		"void SdkHost::ConnectFromRow(const urnet::ConnectLocation& location)",
		"void SdkHost::ConnectBestAvailableFromRow()",
	}
	connectKinds := 0
	for _, signature := range entries {
		body := definitionBody(t, "SdkHost.cpp", host, signature)
		admit := strings.Index(body, "AdmitStartConnect(")
		request := strings.Index(body, "RequestSession(")
		if admit < 0 || request < 0 || request < admit {
			t.Errorf("%s: records a session request without AdmitStartConnect first", signature)
		}
		connectKinds += strings.Count(body, "r.kind = ConnectKind::")
	}
	// no other place builds a connecting request
	if n := strings.Count(host, "r.kind = ConnectKind::BestAvailable") +
		strings.Count(host, "r.kind = ConnectKind::Location"); n != connectKinds {
		t.Errorf("SdkHost.cpp: %d connecting requests, %d of them behind the gate", n, connectKinds)
	}
	for _, signature := range []string{
		"void SdkHost::EnsureSession(const char* reason, bool automaticRecovery)",
		"void SdkHost::Disconnect()",
	} {
		if strings.Contains(definitionBody(t, "SdkHost.cpp", host, signature), "AdmitStartConnect(") {
			t.Errorf("%s: gated on balance; a live session must never be dropped or stranded", signature)
		}
	}

	app := stripLineComments(readAppSource(t, "AppController.cpp"))
	start := strings.Index(app, "cb.onConnectToggle = [this] {")
	end := strings.Index(app, "cb.isConnected = ")
	if start < 0 || end < start {
		t.Fatal("AppController.cpp: no tray connect toggle; update this contract")
	}
	if !strings.Contains(app[start:end], "urnw::balance::RouteConnectGesture(") {
		t.Errorf("AppController.cpp: the tray's Connect does not route through RouteConnectGesture")
	}
	if !strings.Contains(app, "sdk_.SetStartConnectGate(") {
		t.Errorf("AppController.cpp: the start-connect gate is never set")
	}

	page := stripLineComments(readAppSource(t, "ConnectPage.cpp"))
	toggle := definitionBody(t, "ConnectPage.cpp", page, "void ConnectPage::OnConnectToggle(")
	admit := strings.Index(toggle, "AdmitStartConnect(")
	optimistic := strings.Index(toggle, "connectStatus_ = ConnectStatus::Connecting")
	if admit < 0 || optimistic < 0 || optimistic < admit {
		t.Errorf("ConnectPage::OnConnectToggle: flips to Connecting before the start-connect gate")
	}
}

// The start-connect gate reads the out-of-balance latch, not the raw contract
// status the SDK resets on the user's Disconnect, and the window's gate and
// banner read the same latched state.
func TestStartConnectGateReadsTheLatch(t *testing.T) {
	app := stripLineComments(readAppSource(t, "AppController.cpp"))
	onStats := definitionBody(t, "AppController.cpp", app, "void AppController::OnStats(const LiveStats& stats)")
	if strings.Contains(onStats, "insufficientBalance_ = stats.insufficientBalance") {
		t.Errorf("AppController::OnStats: the gate reads the raw contract status, which Disconnect resets")
	}
	if !strings.Contains(onStats, "ObserveBalanceLatch()") {
		t.Errorf("AppController::OnStats: does not feed the out-of-balance latch")
	}
	if !strings.Contains(onStats, "latched.insufficientBalance = insufficientBalance_") ||
		strings.Contains(onStats, "OnStatsChanged(stats)") {
		t.Errorf("AppController::OnStats: the window gets the raw contract status, not the latched one")
	}
	observe := definitionBody(t, "AppController.cpp", app, "void AppController::ObserveBalanceLatch()")
	for _, want := range []string{"balanceLatch_.Observe(", "insufficientBalance_ = balanceLatch_.InsufficientBalance()"} {
		if !strings.Contains(observe, want) {
			t.Errorf("AppController::ObserveBalanceLatch: missing %s", want)
		}
	}
	auth := definitionBody(t, "AppController.cpp", app, "void AppController::OnAuthState(")
	if !strings.Contains(auth, "balanceLatch_.Reset()") {
		t.Errorf("AppController::OnAuthState: a sign-in or sign-out does not reset the latch")
	}
	gate := definitionBody(t, "AppController.cpp", app, "bool AppController::OutOfBalance() const")
	if !strings.Contains(gate, "insufficientBalance_") {
		t.Errorf("AppController::OutOfBalance: does not read the latched state")
	}
}
