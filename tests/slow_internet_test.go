// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os/exec"
	"regexp"
	"sort"
	"strings"
	"testing"
)

// What the app says about a slow internet connection (App/SlowInternet.h) as
// a C++ spec, with negative controls that rewrite a copy of the header; and
// its use, read with comments blanked: SdkHost's session-scoped feed gated on
// the remote, AppController's dwell, tray and balloon, the window's pill, the
// Settings toggle, and the store keys the windows app looks up.

// Execute the spec: the formats, the dwell, the tray's decision and one
// balloon per episode.
func TestSlowInternet(t *testing.T) {
	project := readAppSource(t, "App.vcxproj")
	for _, item := range []string{
		`<ClInclude Include="SlowInternet.h" />`,
		`<ClInclude Include="SlowInternetPill.h" />`,
		`<ClCompile Include="SlowInternetPill.cpp" />`,
	} {
		if !strings.Contains(project, item) {
			t.Errorf("App.vcxproj does not list %s", item)
		}
	}
	program := slowSpecProgram(t, "slow-internet-tests.cpp", "SlowInternet.h", nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("slow internet: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

func requireSlowInternetFailure(t *testing.T, old, replacement string, wants ...string) {
	t.Helper()
	requireSlowSpecFailure(t, "slow-internet-tests.cpp", "SlowInternet.h", func(source string) string {
		return replaceSlowOnce(t, "SlowInternet.h", source, old, replacement)
	}, wants...)
}

// The slow state at the first slow reading flaps with every blip.
func TestSlowInternetRejectsAppearingAtOnce(t *testing.T) {
	requireSlowInternetFailure(t, "inline constexpr int64_t kDwellAppearMillis = 30'000;",
		"inline constexpr int64_t kDwellAppearMillis = 0;",
		"FAIL a steady slow level appears after 30 s", "FAIL blips never show")
}

// Without the minimum stay the slow state leaves with the first normal reading.
func TestSlowInternetRejectsNoMinimumStay(t *testing.T) {
	requireSlowInternetFailure(t, "inline constexpr int64_t kDwellStayMillis = 60'000;",
		"inline constexpr int64_t kDwellStayMillis = 0;",
		"FAIL the stay and the recovered state")
}

// "Internet back to normal" for 3 s precedes the end.
func TestSlowInternetRejectsSkippingTheRecoveredState(t *testing.T) {
	requireSlowInternetFailure(t, "inline constexpr int64_t kRecoveredMillis = 3'000;",
		"inline constexpr int64_t kRecoveredMillis = 0;",
		"FAIL the stay and the recovered state")
}

// A balloon on every tray update rather than once an episode.
func TestSlowInternetRejectsABalloonPerUpdate(t *testing.T) {
	requireSlowInternetFailure(t, "      view.balloonDue = !facts.notified;\n",
		"      view.balloonDue = true;\n",
		"FAIL the tray decision", "FAIL one balloon per episode")
}

// The slow state over a status that says something more urgent.
func TestSlowInternetRejectsTheSlowStateOverOtherStatuses(t *testing.T) {
	requireSlowInternetFailure(t, "  if (!facts.connected) return view;\n", "",
		"FAIL the tray decision", "FAIL the balloon's conditions")
}

// An episode that never ends posts one balloon for good.
func TestSlowInternetRejectsAnEpisodeThatNeverEnds(t *testing.T) {
	requireSlowInternetFailure(t, "    if (presentation_.phase == Phase::Hidden) notified_ = false;\n", "",
		"FAIL one balloon per episode", "two episodes post two balloons (1)")
}

// Slow time older than the minute still counted.
func TestSlowInternetRejectsTheWholeHistory(t *testing.T) {
	requireSlowInternetFailure(t, "    const int64_t windowStart = nowMillis - kDwellWindowMillis;\n",
		"    const int64_t windowStart = 0;\n",
		"FAIL the history ages out")
}

// Slow again while recovering began another episode, and another balloon.
func TestSlowInternetRejectsANewEpisodeWhileRecovering(t *testing.T) {
	requireSlowInternetFailure(t,
		"          Enter(Phase::Slow, nowMillis);\n        } else if (kRecoveredMillis <= nowMillis - since_) {",
		"          Enter(Phase::Hidden, nowMillis);\n        } else if (kRecoveredMillis <= nowMillis - since_) {",
		"FAIL slow again during the recovered state", "FAIL one balloon per episode")
}

// 9,950 ms rounded into the one-decimal form read "10.0 s".
func TestSlowInternetRejectsTenPointZeroSeconds(t *testing.T) {
	requireSlowInternetFailure(t, "  if (tenths < 100) {\n", "  if (millis < 10000) {\n",
		"FAIL the delay format", `9950 ms reads "10.0 s", not "10 s"`)
}

// A limited rate under 500 bit/s read "0 kb/s".
func TestSlowInternetRejectsZeroKilobits(t *testing.T) {
	requireSlowInternetFailure(t, "static_cast<long long>(std::max<int64_t>(1, kilobits))",
		"static_cast<long long>(kilobits)",
		"FAIL the rate format", `1 bit/s reads "0 kb/s", not "1 kb/s"`)
}

// Equal delays drew the upload amber, as the worse.
func TestSlowInternetRejectsAWorseDirectionForEqualDelays(t *testing.T) {
	requireSlowInternetFailure(t, "  if (downDelayMillis < upDelayMillis) return Direction::Up;",
		"  if (downDelayMillis <= upDelayMillis) return Direction::Up;",
		"FAIL each direction shows its delay", "FAIL a direction with no delay shows its limited rate")
}

// The worse direction by the limited rate rather than the standing delay.
func TestSlowInternetRejectsTheWorseDirectionByRate(t *testing.T) {
	requireSlowInternetFailure(t,
		"  content.worse = WorseDirection(reading.upQueueDelayMillis, reading.downQueueDelayMillis);",
		"  content.worse = WorseDirection(reading.upBitsPerSecond, reading.downBitsPerSecond);",
		"FAIL a direction with no delay shows its limited rate")
}

// With room for one direction the better one stayed.
func TestSlowInternetRejectsKeepingTheBetterDirection(t *testing.T) {
	requireSlowInternetFailure(t,
		"      content.down.clear();\n    } else {\n      content.up.clear();",
		"      content.up.clear();\n    } else {\n      content.down.clear();",
		"FAIL the better direction goes first, then the numbers", "FAIL the pill fits its room")
}

// Short of room, the numbers went before the better direction alone.
func TestSlowInternetRejectsDroppingBothDirectionsAtOnce(t *testing.T) {
	requireSlowInternetFailure(t, "  if (width(one) <= room) return one;\n", "",
		"FAIL the pill fits its room")
}

// The reading going away read "Internet back to normal".
func TestSlowInternetRejectsARecoveryWhenTheReadingGoesAway(t *testing.T) {
	requireSlowInternetFailure(t, "  if (!reading || !shown) return std::nullopt;",
		"  if (!shown) return std::nullopt;",
		"FAIL a reading that goes away is no recovery")
}

// The pill stayed up under the can't-reach banner.
func TestSlowInternetRejectsThePillOverTheBanner(t *testing.T) {
	requireSlowInternetFailure(t, "  if (hidden) return std::nullopt;\n",
		"  if (hidden && !reading) return std::nullopt;\n",
		"FAIL a reading that goes away is no recovery")
}

// The numbers re-read while the pill says "Internet back to normal", and on
// the tray's 30 s rather than the pill's 10 s.
func TestSlowInternetRejectsThePillsRefreshOutsideTheSlowContent(t *testing.T) {
	requireSlowInternetFailure(t, "  return presenting && shown && !shown->recovered;",
		"  return presenting && shown;",
		"FAIL the pill refreshes while it shows the numbers")
	requireSlowInternetFailure(t, "inline constexpr int64_t kPillRefreshMillis = 10'000;",
		"inline constexpr int64_t kPillRefreshMillis = 30'000;",
		"FAIL the pill refreshes while it shows the numbers")
}

// SdkHost's feed: session-scoped (the tray follows it while the window is
// hidden), registered before its first read, read off mutex_ by the device's
// handle, shown only over a connected remote, and ended, as unavailable, with
// the session.
func TestSlowInternetSdkHostWiring(t *testing.T) {
	header := stripComments(readAppSource(t, "SdkHost.h"))
	provideRequire(t, "SdkHost.h", header,
		"using TransportDelayHandler = std::function<void(std::optional<slow::Reading>)>;",
		"void SetTransportDelayHandler(TransportDelayHandler h) { onTransportDelay_ = std::move(h); }",
		"void RefreshTransportDelay() { transportDelayReader_.Request(); }",
		"CoalescedWorker transportDelayReader_{[this] { ReadTransportDelayOnWorker(); }};")

	host := sdkHostSource(t)
	bootstrap := definitionBody(t, "SdkHost.cpp", host, "bool SdkHost::BootstrapSession(")
	quitRequireSequence(t, "SdkHost::BootstrapSession", bootstrap,
		regexp.QuoteMeta("BeginTransportDelay(generation, device_->handle());"),
		regexp.QuoteMeta("NoteTransportDelayRemote(generation, remoteConnected);"),
		regexp.QuoteMeta("subs_.push_back(device_->addRemoteChangeListener(remoteChanged));"),
		regexp.QuoteMeta("subs_.push_back(device_->addTransportDelayChangeListener("),
		regexp.QuoteMeta("NoteTransportDelay(generation, ReadingOf(*delay));"),
		regexp.QuoteMeta("transportDelayReader_.Request();"))
	if strings.Count(host, "addTransportDelayChangeListener(") != 1 {
		t.Error("SdkHost.cpp must subscribe the transport delay once, in the session's subs_")
	}
	requireNone(t, "SdkHost.cpp", host, "presentationSubs_.push_back(device_->addTransportDelayChangeListener")

	teardown := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::TeardownSessionLocked(bool stopTunnel) {")
	quitRequireSequence(t, "SdkHost::TeardownSessionLocked", teardown,
		regexp.QuoteMeta("EndTransportDelay();"), regexp.QuoteMeta("subs_.clear();"))
	for _, signature := range []string{"SdkHost::~SdkHost(", "void SdkHost::QuitInOrder("} {
		quitRequireSequence(t, signature, functionBody(host, signature),
			regexp.QuoteMeta("transportDelayReader_.Stop();"), regexp.QuoteMeta("std::scoped_lock lock(mutex_);"))
	}

	reader := functionBody(host, "void SdkHost::ReadTransportDelayOnWorker(")
	quitRequireSequence(t, "SdkHost::ReadTransportDelayOnWorker", reader,
		regexp.QuoteMeta("device = transportDelayDevice_;"),
		regexp.QuoteMeta("char* json = urnet_device_get_transport_delay(device);"),
		regexp.QuoteMeta("urnet_free_string(json);"),
		regexp.QuoteMeta("if (reading) NoteTransportDelay(generation, std::move(reading));"))
	quitForbid(t, "SdkHost::ReadTransportDelayOnWorker", reader,
		"the read runs off mutex_ and by the device's handle, so a teardown cannot free what it reads",
		"device_->", "lock(mutex_")

	note := functionBody(host, "void SdkHost::NoteTransportDelay(")
	provideRequire(t, "SdkHost::NoteTransportDelay", note,
		"if (generation == 0 || generation != transportDelayGeneration_) return;",
		"PublishTransportDelayLocked();")
	remote := functionBody(host, "void SdkHost::NoteTransportDelayRemote(")
	provideRequire(t, "SdkHost::NoteTransportDelayRemote", remote,
		"if (generation == 0 || generation != transportDelayGeneration_) return;",
		"const bool reconnected = remoteConnected && !transportDelayRemote_;",
		"transportDelayReader_.Request();")
	publish := functionBody(host, "void SdkHost::PublishTransportDelayLocked(")
	quitRequireSequence(t, "SdkHost::PublishTransportDelayLocked", publish,
		regexp.QuoteMeta("if (transportDelayRemote_) value = transportDelayReading_;"),
		regexp.QuoteMeta("if (value == transportDelayPublished_) return;"),
		regexp.QuoteMeta("if (onTransportDelay_) onTransportDelay_(std::move(value));"))
	end := functionBody(host, "void SdkHost::EndTransportDelay(")
	quitRequireSequence(t, "SdkHost::EndTransportDelay", end,
		regexp.QuoteMeta("transportDelayGeneration_ = 0;"),
		regexp.QuoteMeta("transportDelayReading_.reset();"),
		regexp.QuoteMeta("transportDelayRemote_ = false;"),
		regexp.QuoteMeta("PublishTransportDelayLocked();"))
}

// AppController: the feed marshalled to the UI thread, the dwell observed on
// every reading, tunnel status and timer, the tray's icon, tooltip and balloon
// decided through the header over the connection status, the timers stopped
// with the app, and the pill fed while the window presents.
func TestSlowInternetAppControllerWiring(t *testing.T) {
	app := appControllerSource(t)
	start := definitionBody(t, "AppController.cpp", app, "void AppController::Start() {")
	provideRequire(t, "AppController::Start", start,
		"sdk_.SetTransportDelayHandler([this](std::optional<slow::Reading> reading) {",
		"OnUi([this, reading] { OnTransportDelay(reading); });",
		"slowNotify_ = LoadAppPrefs().value(kSlowInternetNotifyPrefKey, true);",
		"slowTimer_.IsRepeating(false);",
		"slowRefreshTimer_.Interval(std::chrono::milliseconds(slow::kNumbersRefreshMillis));",
		"slowRefreshTimer_.Tick([this](auto const&, auto const&) { sdk_.RefreshTransportDelay(); });")
	provideRequireOrder(t, "AppController::Start", start, "sdk_.SetTransportDelayHandler(", "sdk_.Initialize()")
	provideRequire(t, "AppController.cpp", app, `constexpr char kSlowInternetNotifyPrefKey[] = "notify_slow_internet";`)
	notify := definitionBody(t, "AppController.cpp", app, "void AppController::SetSlowInternetNotify(bool on) {")
	provideRequire(t, "AppController::SetSlowInternetNotify", notify,
		"slowNotify_ = on;", "SaveAppPref(kSlowInternetNotifyPrefKey, on);")

	observe := definitionBody(t, "AppController.cpp", app, "void AppController::ObserveSlowInternet() {")
	provideRequire(t, "AppController::ObserveSlowInternet", observe,
		"if (transportDelay_ && facts.signedIn && facts.statusKnown && facts.tunnel == tray::Tunnel::Up) {",
		"depth = slow::DepthOf(*transportDelay_);",
		"const slow::Presentation presentation = slowTray_.Observe(depth, now);",
		"if (0 <= presentation.nextMillis) {",
		"const bool refreshing = presentation.phase == slow::Phase::Slow;")
	tunnel := definitionBody(t, "AppController.cpp", app, "void AppController::OnTunnelState(const proto::TunnelStatus& status) {")
	quitRequireSequence(t, "AppController::OnTunnelState", tunnel,
		regexp.QuoteMeta("lastTunnelStatus_ = status;"),
		regexp.QuoteMeta("ObserveSlowInternet();"),
		regexp.QuoteMeta("UpdateTray();"))
	delay := definitionBody(t, "AppController.cpp", app,
		"void AppController::OnTransportDelay(std::optional<slow::Reading> reading) {")
	quitRequireSequence(t, "AppController::OnTransportDelay", delay,
		regexp.QuoteMeta("transportDelay_ = std::move(reading);"),
		regexp.QuoteMeta("ObserveSlowInternet();"),
		regexp.QuoteMeta("UpdateTray();"),
		regexp.QuoteMeta("if (windowVisible_ && window_) {"),
		regexp.QuoteMeta("self->OnTransportDelayChanged(transportDelay_);"))

	update := definitionBody(t, "AppController.cpp", app, "void AppController::UpdateTray() {")
	quitRequireSequence(t, "AppController::UpdateTray", update,
		regexp.QuoteMeta("const tray::View view = tray::Decide(CurrentTrayFacts());"),
		regexp.QuoteMeta("slowTray_.Decide(view.status == tray::Status::Connected, slowNotify_);"),
		`tray_\.SetState\(view\.connectedIcon \? TrayState::NoProvideConnect\s*:\s*TrayState::NoProvideNoConnect,\s*slowView\.icon\);`,
		regexp.QuoteMeta("switch (view.status) {"),
		regexp.QuoteMeta("switch (slowView.tip) {"),
		regexp.QuoteMeta("tray_.SetTooltip(tip);"),
		regexp.QuoteMeta("if (slowView.postBalloon) {"),
		regexp.QuoteMeta(`LogInfo("app: slow internet notice posted");`),
		regexp.QuoteMeta("tray_.ShowBalloon("))
	slowTip := quitSourceFrom(t, "AppController::UpdateTray", update, "case slow::Tip::Slow:", "break;")
	provideRequire(t, "the tray's slow tooltip", slowTip,
		`Localized(fallback ? "slow_internet_fallback_title" : "slow_internet")`,
		"slow::SlowStatus(", "transportDelay_.value_or(slow::Reading{}), tip.size() + 3)")
	recoveredTip := quitSourceFrom(t, "AppController::UpdateTray", update, "case slow::Tip::Recovered:", "break;")
	provideRequire(t, "the tray's recovered tooltip", recoveredTip, `Localized("slow_internet_recovered")`)
	balloon := quitSourceFrom(t, "AppController::UpdateTray", update, "if (slowView.postBalloon) {", "}")
	provideRequire(t, "the slow balloon", balloon,
		`Localized(fallback ? "slow_internet_fallback_title" : "slow_internet")`,
		`Localized(fallback ? "slow_internet_fallback_body" : "slow_internet_still_connected")`)
	if strings.Count(app, `"app: slow internet notice posted"`) != 1 {
		t.Error("AppController.cpp posts the slow balloon outside UpdateTray's one decision")
	}

	shutdown := definitionBody(t, "AppController.cpp", app, "void AppController::Shutdown(lifetime::Ending ending) {")
	quitRequireSequence(t, "AppController::Shutdown", shutdown,
		regexp.QuoteMeta("if (slowTimer_) slowTimer_.Stop();"),
		regexp.QuoteMeta("if (slowRefreshTimer_) slowRefreshTimer_.Stop();"),
		regexp.QuoteMeta("tray_.Destroy();"))
	reconcile := definitionBody(t, "AppController.cpp", app, "void AppController::ReconcileWindowPresentation() {")
	provideRequire(t, "AppController::ReconcileWindowPresentation", reconcile,
		"self->OnTransportDelayChanged(transportDelay_);", "sdk_.RefreshTransportDelay();")
}

// The window: the pane count pushed with the fold gate, the reading relayed
// to the connect page, the pill in the rightmost pane header on screen and
// hidden while the can't-reach banner is open.
func TestSlowInternetWindowWiring(t *testing.T) {
	window := stripComments(readAppSource(t, "MainWindow.xaml.cpp"))
	breakpoint := definitionBody(t, "MainWindow.xaml.cpp", window, "void MainWindow::ApplyBreakpoint() {")
	quitRequireSequence(t, "MainWindow::ApplyBreakpoint", breakpoint,
		regexp.QuoteMeta("connect_->ApplyPaneCFolded(!connectThree);"),
		regexp.QuoteMeta("connect_->ApplyPaneCount(connectThree ? 3 : twoPanes ? 2 : 1);"))
	relay := definitionBody(t, "MainWindow.xaml.cpp", window,
		"void MainWindow::OnTransportDelayChanged(std::optional<urnw::slow::Reading> const& reading) {")
	provideRequire(t, "MainWindow::OnTransportDelayChanged", relay, "connect_->ApplyTransportDelay(reading);")

	connect := stripComments(readAppSource(t, "ConnectPage.cpp"))
	provideRequire(t, "ConnectPage::Initialize",
		definitionBody(t, "ConnectPage.cpp", connect, "void ConnectPage::Initialize() {"), "BuildSlowPill();")
	provideRequire(t, "ConnectPage::SetPresentationActive",
		definitionBody(t, "ConnectPage.cpp", connect, "void ConnectPage::SetPresentationActive(bool active) {"),
		"if (slowPill_) slowPill_->SetPresentationActive(active);")
	provideRequire(t, "ConnectPage::ApplyTransportDelay",
		definitionBody(t, "ConnectPage.cpp", connect,
			"void ConnectPage::ApplyTransportDelay(std::optional<urnw::slow::Reading> const& reading) {"),
		"if (slowPill_) slowPill_->Apply(reading);")
	quitRequireSequence(t, "ConnectPage::SlowPillHost",
		definitionBody(t, "ConnectPage.cpp", connect, "Controls::Panel ConnectPage::SlowPillHost(int panes) {"),
		regexp.QuoteMeta("if (3 <= panes) return w_.PaneCSlowHost();"),
		regexp.QuoteMeta("if (panes == 2) return w_.PaneBSlowHost();"),
		regexp.QuoteMeta("return w_.PaneASlowHost();"))
	// the pill fits beside the title of the header it is in, the same pane
	quitRequireSequence(t, "ConnectPage::PlaceSlowPill",
		definitionBody(t, "ConnectPage.cpp", connect, "void ConnectPage::PlaceSlowPill() {"),
		regexp.QuoteMeta("SlowPillHost(paneCount_).Children().Append(slowPill_->Root());"),
		regexp.QuoteMeta("slowPill_->SetTitle(3 <= paneCount_"),
		regexp.QuoteMeta("w_.PaneCTitle()"),
		regexp.QuoteMeta("paneCount_ == 2 ? w_.PaneBTitle()"),
		regexp.QuoteMeta("w_.PaneATitle());"))
	provideRequire(t, "ConnectPage::BuildSlowPill",
		definitionBody(t, "ConnectPage.cpp", connect, "void ConnectPage::BuildSlowPill() {"),
		"Sdk().RefreshTransportDelay();",
		"w_.RegionPromptBar().RegisterPropertyChangedCallback(",
		"InfoBar::IsOpenProperty()",
		"page.slowPill_->SetSuppressed(self->RegionPromptBar().IsOpen());",
		"slowPill_->SetSuppressed(w_.RegionPromptBar().IsOpen());")

	// one host per pane header, in that header: after its title and before
	// the pane's body
	xaml := readAppSource(t, "MainWindow.xaml")
	at := func(name string) int {
		index := strings.Index(xaml, `x:Name="`+name+`"`)
		if index < 0 {
			t.Fatalf("MainWindow.xaml has no %s", name)
		}
		return index
	}
	for _, pane := range []struct{ title, host, body string }{
		{"PaneATitle", "PaneASlowHost", "PaneAScroll"},
		{"PaneBTitle", "PaneBSlowHost", "ConnectionsVerdictBar"},
		{"PaneCTitle", "PaneCSlowHost", "PaneCBody"},
	} {
		if !(at(pane.title) < at(pane.host) && at(pane.host) < at(pane.body)) {
			t.Errorf("MainWindow.xaml: %s is not in the header of %s's pane", pane.host, pane.title)
		}
	}
	// the rates keep their 12 from the title as a margin, so the empty slot
	// beside them moves nothing
	if !regexp.MustCompile(`x:Name="ThroughputText" Grid\.Column="1"\s+HorizontalAlignment="Right" Margin="12,0,0,0"`).MatchString(xaml) {
		t.Error("MainWindow.xaml: ThroughputText no longer keeps its 12 from the title as a margin")
	}
}

// The pill: the tunnel drawn from the header's geometry with mitered corners,
// settling about the vanishing point; the fades and the pill's own fade per
// §3, with "Show animations" off dropping only the scale; variant A+, a black
// box with square corners in a 35% amber outline that nothing opens from; its
// content, recovery, fit and re-read decided by the header; one element read;
// the store's words.
func TestSlowInternetPillWiring(t *testing.T) {
	pill := stripComments(readAppSource(t, "SlowInternetPill.cpp"))
	provideRequire(t, "TunnelShapes::TunnelShapes",
		definitionBody(t, "SlowInternetPill.cpp", pill, "TunnelShapes::TunnelShapes(double size) {"),
		"const tunnel::Square geometry = tunnel::SquareAt(size, index);",
		"path.StrokeLineJoin(media::PenLineJoin::Miter);",
		"scale.CenterX(vanishing);", "scale.CenterY(vanishing);",
		"colors::MakeBrush(colors::kUrAmber)")
	provideRequire(t, "TunnelShapes::SetDepth",
		definitionBody(t, "SlowInternetPill.cpp", pill, "void TunnelShapes::SetDepth(int depth, bool animate, bool scale) {"),
		"tunnel::FadesFor(from, depth)", "if (scale) {", "tunnel::kSettleScale")
	provideRequire(t, "SlowInternetPill::AnimationsEnabled",
		definitionBody(t, "SlowInternetPill.cpp", pill, "bool SlowInternetPill::AnimationsEnabled() const {"),
		"UISettings().AnimationsEnabled()")

	// the look: 0x59 of 0xFF is the 35% outline
	provideRequire(t, "SlowInternetPill.cpp", pill,
		"constexpr winrt::Windows::UI::Color kPillFill{255, 0, 0, 0};",
		"constexpr uint8_t kOutlineAlpha = 0x59;",
		"constexpr double kPillHeight = 30;")
	provideRequire(t, "SlowInternetPill::SlowInternetPill",
		definitionBody(t, "SlowInternetPill.cpp", pill,
			"SlowInternetPill::SlowInternetPill(std::function<void()> refresh) : refresh_(std::move(refresh)) {"),
		"root_.Background(colors::MakeBrush(kPillFill));",
		"root_.BorderBrush(colors::MakeBrush(colors::WithAlpha(colors::kUrAmber, kOutlineAlpha)));",
		"root_.CornerRadius(CornerRadiusHelper::FromUniformRadius(0));",
		"root_.MinHeight(kPillHeight);",
		"check_.Background(colors::MakeBrush(colors::kUrGreen));",
		"label_ = MakeText(14, colors::kUrAmber);")
	provideRequire(t, "SlowInternetPill::MakeDirection",
		definitionBody(t, "SlowInternetPill.cpp", pill,
			"SlowInternetPill::DirectionText SlowInternetPill::MakeDirection(wchar_t const* arrow) {"),
		"documents::Typography::SetNumeralAlignment(direction.number, FontNumeralAlignment::Tabular);")
	// a readout: no button, nothing to click, nothing that opens
	header := stripComments(readAppSource(t, "SlowInternetPill.h"))
	for name, source := range map[string]string{"SlowInternetPill.cpp": pill, "SlowInternetPill.h": header} {
		requireNone(t, name, source, "Button", "Flyout", "Click(", "Tapped(")
	}

	// what it shows next, as Apple's host draws it
	quitRequireSequence(t, "SlowInternetPill::Render",
		definitionBody(t, "SlowInternetPill.cpp", pill, "void SlowInternetPill::Render() {"),
		regexp.QuoteMeta("if (!presentationActive_) return;"),
		regexp.QuoteMeta("slow::NextPillContent(previous, reading_, suppressed_);"),
		regexp.QuoteMeta("UpdateRefresh();"),
		regexp.QuoteMeta("if (previous) Leave();"),
		regexp.QuoteMeta("if (!previous || !previous->recovered) Recover(*next);"),
		`tunnel_\.SetDepth\(slow::DepthOf\(\*reading_\),\s*true, AnimationsEnabled\(\)\);`,
		regexp.QuoteMeta("FadeIn();"))
	quitRequireSequence(t, "SlowInternetPill::Recover",
		definitionBody(t, "SlowInternetPill.cpp", pill, "void SlowInternetPill::Recover(slow::PillContent const& next) {"),
		`tunnel_\.SetDepth\(0,\s*true, AnimationsEnabled\(\)\);`,
		regexp.QuoteMeta("SetFace(true,"),
		regexp.QuoteMeta("FadeOut(slow::kRecoveredMillis);"))
	provideRequire(t, "SlowInternetPill::Leave",
		definitionBody(t, "SlowInternetPill.cpp", pill, "void SlowInternetPill::Leave() {"),
		"FadeOut(tunnel::ContainerHideDelayFor(from, 0));")
	provideRequire(t, "SlowInternetPill::FadeIn",
		definitionBody(t, "SlowInternetPill.cpp", pill, "void SlowInternetPill::FadeIn() {"),
		"tunnel::kFadeInMillis")
	provideRequire(t, "SlowInternetPill::FadeOut",
		definitionBody(t, "SlowInternetPill.cpp", pill, "void SlowInternetPill::FadeOut(int64_t beginMillis) {"),
		"tunnel::kFadeOutMillis, beginMillis", "shown_.reset();", "UpdateRefresh();")
	// off screen it is put away, so the replay on show is no recovery
	provideRequire(t, "SlowInternetPill::SetPresentationActive",
		definitionBody(t, "SlowInternetPill.cpp", pill, "void SlowInternetPill::SetPresentationActive(bool active) {"),
		"if (active) return;", "shown_.reset();", "root_.Visibility(Visibility::Collapsed);", "UpdateRefresh();")
	provideRequire(t, "SlowInternetPill::UpdateRefresh",
		definitionBody(t, "SlowInternetPill.cpp", pill, "void SlowInternetPill::UpdateRefresh() {"),
		"slow::PillRefreshes(shown_, presentationActive_)",
		"std::chrono::milliseconds(slow::kPillRefreshMillis)",
		"if (refresh_) refresh_();")
	provideRequire(t, "SlowInternetPill::Fit",
		definitionBody(t, "SlowInternetPill.cpp", pill, "void SlowInternetPill::Fit() {"),
		"headerWidth - Natural(title_, title_.Text())",
		"slow::FitPill(content_, room,")

	// the store's words, and one element read: the face showing, named in full
	text := definitionBody(t, "SlowInternetPill.cpp", pill, "void SlowInternetPill::ApplyText() {")
	quitRequireSequence(t, "SlowInternetPill::ApplyText", text,
		`Loc\(content_\.label == slow::PillContent::Label::Fallback\s*\?\s*"slow_internet_fallback_short"\s*:\s*"slow_internet_poor_signal"\)`,
		regexp.QuoteMeta(`Loc("slow_internet_recovered")`),
		regexp.QuoteMeta(`Loc("slow_internet_upload_delay")`),
		regexp.QuoteMeta(`Loc("slow_internet_download_delay")`),
		regexp.QuoteMeta("slow::PillName(label, content_, upWords, downWords)"),
		regexp.QuoteMeta("slow::PillName(recovered, content_, upWords, downWords)"),
		regexp.QuoteMeta("Fit();"))
	provideRequire(t, "SlowInternetPill::SetFace",
		definitionBody(t, "SlowInternetPill.cpp", pill, "void SlowInternetPill::SetFace(bool recovered, bool animate) {"),
		"label_, recovered ? AccessibilityView::Raw : AccessibilityView::Content",
		"recoveredText_, recovered ? AccessibilityView::Content : AccessibilityView::Raw")
}

// Settings > General's toggle writes the tray's preference through
// PageContext, and starts from it.
func TestSlowInternetSettingWiring(t *testing.T) {
	settings := stripComments(readAppSource(t, "SettingsPage.cpp"))
	provideRequire(t, "SettingsPage::BuildGeneralSection",
		definitionBody(t, "SettingsPage.cpp", settings, "void SettingsPage::BuildGeneralSection(Panel const& host) {"),
		`slowInternetNotify_ = ToggleRow(card, Loc("slow_internet_notify_toggle"), hstring{});`,
		"slowInternetNotify_.IsOn(urnw::pages::SlowInternetNotify());",
		"urnw::pages::SetSlowInternetNotify(slowInternetNotify_.IsOn());")
	context := stripComments(readAppSource(t, "PageContext.cpp"))
	provideRequire(t, "PageContext.cpp", context,
		"bool SlowInternetNotify() { return urnw::App().SlowInternetNotify(); }",
		"void SetSlowInternetNotify(bool on) { urnw::App().SetSlowInternetNotify(on); }")
}

// The windows app looks up exactly these slow-internet keys: the tray's and
// the balloon's, the Settings toggle, and the A+ pill's. The Apple-only "Slow"
// and the Live Activity toggle never; nor what only the old pill and its
// flyout used, "Fallback connection", the flyout's body and "Delays cleared",
// which the store has deprecated for windows.
func TestSlowInternetLooksUpTheWindowsKeys(t *testing.T) {
	want := []string{
		"slow_internet",
		"slow_internet_download_delay",
		"slow_internet_fallback_body",
		"slow_internet_fallback_short",
		"slow_internet_fallback_title",
		"slow_internet_notify_toggle",
		"slow_internet_poor_signal",
		"slow_internet_recovered",
		"slow_internet_still_connected",
		"slow_internet_upload_delay",
	}
	literal := regexp.MustCompile(`"(slow_internet[a-z_]*)"`)
	found := map[string]bool{}
	files := appSourceFiles(t, ".cpp", ".h")
	for _, name := range sortedNames(files) {
		for _, match := range literal.FindAllStringSubmatch(stripComments(files[name]), -1) {
			found[match[1]] = true
		}
	}
	got := []string{}
	for key := range found {
		got = append(got, key)
	}
	sort.Strings(got)
	if strings.Join(got, ",") != strings.Join(want, ",") {
		t.Errorf("the app looks up the slow-internet keys %v, want the windows set %v", got, want)
	}
}
