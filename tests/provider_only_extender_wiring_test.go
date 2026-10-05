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

// The provider-only device's extender role (support inbox 1521). While
// disconnected the provider is the service's provider-only device, and the
// Earnings extender row and extender plot read what a session's DeviceRemote
// reports for its device: the extender status with the setting beside it, and
// the controller's extender series. get_provider_stats now carries both. The
// pure parts run in provide-protocol-tests.cpp and extender-tests.cpp; these
// read the service and app sources, which need Windows and WinRT, with
// comments stripped so prose cannot satisfy a contract.

// The service opens the role's reading with the statistics, answers it with
// no device call, and retires it with the device.
func TestProviderOnlyExtenderServiceWiring(t *testing.T) {
	source := tunnelControllerSource(t)
	open := definitionBody(t, "TunnelController.cpp", source,
		"void TunnelController::OpenProviderStatsLocked()")
	provideRequire(t, "OpenProviderStatsLocked", open,
		"providerDevice_->addExtenderProvideStatusChangeListener(",
		"extender->Store(providerDevice_->getExtenderProvideStatus());",
		"extenderSetting = providerDevice_->getProvideExtender();",
		"providerExtenderSub_ = std::move(extenderSub);",
		"providerExtender_ = std::move(extender);",
		"providerExtenderSetting_ = extenderSetting;")
	// subscribed before the first read, so no change is lost in between
	provideRequireOrder(t, "OpenProviderStatsLocked", open,
		"providerDevice_->addExtenderProvideStatusChangeListener(",
		"providerDevice_->getExtenderProvideStatus()")
	// stored where get_provider_stats reads, under its lock
	provideRequireOrder(t, "OpenProviderStatsLocked", open,
		"std::scoped_lock lock(providerStatsMutex_);", "providerExtender_ = std::move(extender);")
	// the listener holds a share of the reading, never the controller
	listener := handlerSource(t, "OpenProviderStatsLocked", open,
		"addExtenderProvideStatusChangeListener(")
	provideRequire(t, "the extender status listener", listener, "[extender]",
		"extender->Store(std::move(status));")
	requireNone(t, "the extender status listener", listener, "this", "providerDevice_")
	// its own best effort: a reading that fails costs the role, never the
	// statistics opened before it
	failure := open[strings.Index(open, "addExtenderProvideStatusChangeListener("):]
	catch := strings.Index(failure, "catch (const std::exception&)")
	stored := strings.Index(failure, "std::scoped_lock lock(providerStatsMutex_);")
	if catch < 0 || stored < catch {
		t.Fatal("OpenProviderStatsLocked must catch a failed extender reading before it stores the statistics")
	}
	failure = failure[catch:stored]
	provideRequire(t, "the extender reading's failure", failure,
		"extenderSub = urnet::Sub{};", "extender.reset();")
	requireNone(t, "the extender reading's failure", failure, "return")

	stats := definitionBody(t, "TunnelController.cpp", source,
		"proto::ProviderStats TunnelController::ProviderStats()")
	provideRequire(t, "ProviderStats", stats,
		"providerStatsVc_->getExtenderThroughputPoints()",
		"stats.extender_points = *points;",
		"providerExtender_->Load()",
		"stats.extender_provide_status = *status;",
		"stats.provide_extender = providerExtenderSetting_;")
	// the copies the listener and the opening left: never a call into the device
	requireNone(t, "ProviderStats", stats, "getExtenderProvideStatus(", "getProvideExtender(",
		"providerDevice_")

	retire := definitionBody(t, "TunnelController.cpp", source,
		"void TunnelController::RetireProviderDeviceLocked()")
	provideRequire(t, "RetireProviderDeviceLocked", retire,
		"extenderSub = std::move(providerExtenderSub_);",
		"providerExtender_.reset();",
		"providerExtenderSetting_ = false;")
	provideRequireOrder(t, "RetireProviderDeviceLocked", retire,
		"std::scoped_lock lock(providerStatsMutex_);", "extenderSub = std::move(providerExtenderSub_);")
	worker := retire[strings.Index(retire, "RunBounded("):]
	provideRequire(t, "the retire worker", worker, "extenderSub = std::move(extenderSub)")
	// unsubscribed before the controller and the device it listens to close
	provideRequireOrder(t, "the retire worker", worker, "extenderSub = urnet::Sub{};",
		"device->closeContractViewController(*statsVc);")
	provideRequireOrder(t, "the retire worker", worker, "extenderSub = urnet::Sub{};",
		"device->close();")
	// Sub::reset() releases the handle without unsubscribing (PacketPump.cpp)
	requireNone(t, "the retire worker", worker, "extenderSub.reset()")

	header := stripComments(readServiceSource(t, "TunnelController.h"))
	provideRequire(t, "TunnelController.h", header,
		"class LatestExtenderProvideStatus {",
		"urnet::Sub providerExtenderSub_;",
		"std::shared_ptr<LatestExtenderProvideStatus> providerExtender_;",
		"bool providerExtenderSetting_ = false;")
}

// The app puts the answer where a session's device puts its own: the extender
// points in the drawer cache the Earnings extender chart reads, and the status
// through the one feed both extender rows take, marked as the provider-only
// device's. It takes the status off again wherever that device stops being the
// provider, and never touches a session's.
func TestProviderOnlyExtenderAppWiring(t *testing.T) {
	source := sdkHostSource(t)
	show := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::ShowProviderOnlyStatsLocked(const proto::ProviderStats& stats)")
	provideRequire(t, "ShowProviderOnlyStatsLocked", show,
		"provider.extenderPoints = proto::ExtenderPointsOf<urnet::ThroughputPoint>(stats);",
		"lastExtenderPoints_ = provider.extenderPoints;",
		"ExtenderProvideStatusViewOf(",
		"proto::ExtenderProvideStatusOf<urnet::ExtenderProvideStatus>(stats)",
		"return stats.provide_extender;",
		"extender.providerOnly = true;",
		"PublishExtenderProvideView(std::move(extender));")
	provideRequireOrder(t, "ShowProviderOnlyStatsLocked", show,
		"extender.providerOnly = true;", "PublishExtenderProvideView(std::move(extender));")
	clear := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::ClearProviderOnlyStatsLocked(bool providerGone)")
	provideRequire(t, "ClearProviderOnlyStatsLocked", clear, "lastExtenderPoints_.clear();")

	// one dedup baseline for a session's device and the provider-only device
	publish := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::PublishExtenderProvideStatus(std::optional<urnet::ExtenderProvideStatus> status)")
	provideRequire(t, "PublishExtenderProvideStatus", publish,
		"PublishExtenderProvideView(std::move(view));")
	requireNone(t, "PublishExtenderProvideStatus", publish, "onExtenderProvideStatus_(",
		"lastExtenderProvideStatus_ = ")
	view := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::PublishExtenderProvideView(ExtenderProvideStatusView view)")
	provideRequire(t, "PublishExtenderProvideView", view,
		"if (view == lastExtenderProvideStatus_ && !extenderProvideRepublish_) return;",
		"lastExtenderProvideStatus_ = view;",
		"onExtenderProvideStatus_(std::move(view));")

	forget := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::ForgetProviderOnlyExtenderStatus()")
	provideRequire(t, "ForgetProviderOnlyExtenderStatus", forget,
		"if (!lastExtenderProvideStatus_.providerOnly) return;",
		"lastExtenderProvideStatus_ = {};",
		"onExtenderProvideStatus_({});")
	provideRequireOrder(t, "ForgetProviderOnlyExtenderStatus", forget,
		"if (!lastExtenderProvideStatus_.providerOnly) return;", "lastExtenderProvideStatus_ = {};")

	// Every place the loop lets the provider-only device's readings go (a
	// session's device took over, or no provider runs) lets its extender status
	// go with them, or the Earnings row would keep a role nothing runs.
	loop := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::ProviderOnlyStatsLoop()")
	resets := strings.Count(loop, "ResetProviderOnlyStatus();")
	if resets < 3 {
		t.Fatalf("ProviderOnlyStatsLoop resets the readings in %d places; update this contract", resets)
	}
	paired := regexp.MustCompile(`ResetProviderOnlyStatus\(\);\s*ForgetProviderOnlyExtenderStatus\(\);`)
	if forgets := len(paired.FindAllString(loop, -1)); forgets != resets {
		t.Errorf("ProviderOnlyStatsLoop lets the readings go in %d places but the extender status in "+
			"only %d of them", resets, forgets)
	}

	// The Connect page's row is the switch, which writes through a session's
	// device only: hidden for the provider-only device's status, and never
	// written while hidden. The Earnings row has no switch and shows it.
	connect := stripComments(readAppSource(t, "ConnectPage.cpp"))
	row := definitionBody(t, "ConnectPage.cpp", connect, "void ConnectPage::ApplyExtenderProvideRow()")
	provideRequire(t, "ConnectPage::ApplyExtenderProvideRow", row,
		"const Visibility shown = model.switchVisible ? Visibility::Visible : Visibility::Collapsed;")
	toggled := definitionBody(t, "ConnectPage.cpp", connect, "void ConnectPage::OnExtenderToggled(")
	provideRequireOrder(t, "ConnectPage::OnExtenderToggled", toggled,
		"if (!urnw::ExtenderProvideRowModelFor(extenderProvideView_).switchVisible) return;",
		"Sdk().SetProvideExtender(on);")
	wallet := stripComments(readAppSource(t, "WalletPage.cpp"))
	walletRow := definitionBody(t, "WalletPage.cpp", wallet, "void WalletPage::ApplyExtenderProvideRow()")
	provideRequire(t, "WalletPage::ApplyExtenderProvideRow", walletRow, "model.visible ? Visibility::Visible")
	requireNone(t, "WalletPage::ApplyExtenderProvideRow", walletRow, "switchVisible")
	// the extender plot's gate is the pushed status's `enabled`, whichever device
	walletState := definitionBody(t, "WalletPage.cpp", wallet,
		"void WalletPage::ApplyExtenderProvideState(urnw::ExtenderProvideStatusView const& view)")
	provideRequire(t, "WalletPage::ApplyExtenderProvideState", walletState,
		"extenderRunning_ = view.enabled;")
	requireNone(t, "WalletPage::ApplyExtenderProvideState", walletState, "providerOnly")
}

// A reader that drops the status, or reads the provider series as the
// extender's, shows the role wrong while disconnected; the spec must say so.
func TestProvideProtocolRejectsDroppedExtenderFields(t *testing.T) {
	extra := provideJsonFlags(t)
	requireProvideFailure(t, "provide-protocol-tests.cpp", map[string]func(string) string{
		"Protocol.h": func(source string) string {
			return strings.Replace(source,
				"  if (auto it = j.find(\"extender_provide_status\"); it != j.end() && it->is_object())\n"+
					"    v.extender_provide_status = *it;\n", "", 1)
		},
	}, "provider stats: extender_provide_status round-trips", extra...)
	requireProvideFailure(t, "provide-protocol-tests.cpp", map[string]func(string) string{
		"Protocol.h": func(source string) string {
			return strings.Replace(source,
				"return detail::PointsOf<Point>(stats.extender_points);",
				"return detail::PointsOf<Point>(stats.provider_points);", 1)
		},
	}, "readers: the extender points come back, oldest first", extra...)
	requireProvideFailure(t, "provide-protocol-tests.cpp", map[string]func(string) string{
		"Protocol.h": func(source string) string {
			return strings.Replace(source, "  get(\"provide_extender\", v.provide_extender);\n", "", 1)
		},
	}, "provider stats: provide_extender round-trips", extra...)
}

// Build and run the extender spec on copies of ExtenderPresentation.h and .cpp
// with `mutate` applied to `file`, and require that it fails naming `want`.
func requireExtenderPresentationFailure(t *testing.T, file string, mutate func(string) string,
	want string) {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("extender tests require a C++20 compiler: ", err)
	}
	appDir := filepath.Join(repositoryRoot(t), "app")
	sourceDir := filepath.Join(appDir, "src", "App")
	dir := t.TempDir()
	for _, name := range []string{"ExtenderPresentation.h", "ExtenderPresentation.cpp"} {
		source, err := os.ReadFile(filepath.Join(sourceDir, name))
		if err != nil {
			t.Fatal(err)
		}
		content := string(source)
		if name == file {
			content = mutate(content)
			if content == string(source) {
				t.Fatalf("negative control did not change the production %s", name)
			}
		}
		if err := os.WriteFile(filepath.Join(dir, name), []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(dir, "extender-tests")
	build := exec.Command(compiler, "-std=c++20", "-I"+dir, "-I"+sourceDir,
		"-I"+filepath.Join(appDir, "third_party", "qrcodegen"),
		filepath.Join(appDir, "tools", "extender-tests.cpp"),
		filepath.Join(dir, "ExtenderPresentation.cpp"),
		filepath.Join(appDir, "third_party", "qrcodegen", "qrcodegen.cpp"),
		"-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build extender tests: %v\n%s", err, output)
	}
	run := exec.Command(program)
	run.Dir = filepath.Join(appDir, "tools")
	output, err := run.CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

// A switch shown over the provider-only device's status would be a dead switch
// (N1), and a feed that cannot tell the two devices apart would keep a
// session's switch hidden after it takes over with the same reading.
func TestExtenderPresentationRejectsASwitchOverTheProviderOnlyDevice(t *testing.T) {
	requireExtenderPresentationFailure(t, "ExtenderPresentation.cpp", func(source string) string {
		return strings.Replace(source, "model.switchVisible = view.supported && !view.providerOnly;",
			"model.switchVisible = view.supported;", 1)
	}, "keeps the switch's row hidden")
	requireExtenderPresentationFailure(t, "ExtenderPresentation.h", func(source string) string {
		return strings.Replace(source, " && providerOnly == o.providerOnly;", ";", 1)
	}, "the provider-only device's reading alone is a change")
	requireExtenderPresentationFailure(t, "ExtenderPresentation.cpp", func(source string) string {
		return strings.Replace(source, "  guess.providerOnly = current.providerOnly;\n", "", 1)
	}, "a guess over the provider-only device's status stays its")
}
