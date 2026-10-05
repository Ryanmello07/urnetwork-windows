// SPDX-License-Identifier: MPL-2.0

// The call sites of "send feedback with logs" whether or not a tunnel runs
// (support inbox 2090). The decisions are pure and run in log_upload_test.go;
// what makes the service upload its own logs with no session, on a device that
// touches nothing else and is retired again, and what makes the app ask it
// first off the UI thread and fall back to its DeviceRemote, lives in the
// service and in SdkHost, which need Windows and WinRT. So these read their
// sources, comments stripped so prose cannot satisfy a contract.

package tests

import (
	"strings"
	"testing"
)

// The upload_logs branch of ControlServer::Handle, through the next branch.
func uploadLogsBranch(t *testing.T) string {
	t.Helper()
	source := stripComments(readServiceSource(t, "ControlServer.cpp"))
	start := strings.Index(source, "type == proto::msg::kUploadLogs")
	if start < 0 {
		t.Fatal("ControlServer.cpp no longer serves upload_logs")
	}
	end := strings.Index(source[start:], "} else if")
	if end < 0 {
		t.Fatal("cannot find the end of the upload_logs branch in ControlServer.cpp")
	}
	return source[start : start+end]
}

// The service validates the feedback id (it becomes part of the url) and the
// identity a standalone device would register under before anything is
// touched, hands the controller the diagnostics writer for the carrier's line,
// and answers with the device that carries the upload.
func TestLogUploadWiringServiceValidatesBeforeUploading(t *testing.T) {
	branch := uploadLogsBranch(t)
	provideRequire(t, "upload_logs", branch,
		"request.get<proto::UploadLogs>()",
		"proto::LooksLikeFeedbackId(req.feedback_id)",
		"rpcsession::IsPairableInstanceId(req.instance_id)",
		"req.by_jwt.empty()",
		"req.network_space_json.empty()",
		"reply.log_upload_carrier = carrier;")
	provideRequireOrder(t, "upload_logs", branch,
		"proto::LooksLikeFeedbackId(req.feedback_id)", "tunnel_.UploadLogs(req, error, carrier,")
	provideRequireOrder(t, "upload_logs", branch,
		"tunnel_.UploadLogs(req, error, carrier,", "diagnostics_.NoteLogUpload(chosen);")
}

// The device that runs carries it; with none, a standalone device is built as
// the provider-only device is, after the refusals and after the last one is
// retired, in provide mode never, and nothing after that.
func TestLogUploadWiringStandaloneDeviceTouchesNoMachineState(t *testing.T) {
	upload := definitionBody(t, "TunnelController.cpp", tunnelControllerSource(t),
		"bool TunnelController::UploadLogs(")
	provideRequire(t, "UploadLogs", upload,
		"logupload::CarrierFor(device_.has_value(), providerDevice_ != nullptr)",
		"device = &*device_;",
		"device = providerDevice_.get();",
		"SweepAbandonedTeardowns()",
		"SelfRestartPending()",
		"slot->space = ImportNetworkSpaceLocked(request.network_space_json);",
		"slot->device->setProvideControlMode(\"never\");",
		"logUpload_ = slot;")
	provideRequireOrder(t, "UploadLogs", upload, "lock.try_lock_for(kStopLockBudget)",
		"logupload::CarrierFor(")
	provideRequireOrder(t, "UploadLogs", upload, "logupload::StandaloneRefusalFor(",
		"RetireLogUploadDeviceLocked();")
	provideRequireOrder(t, "UploadLogs", upload, "RetireLogUploadDeviceLocked();",
		"NewDeviceLocked(*slot->space")
	provideRequireOrder(t, "UploadLogs", upload, "NewDeviceLocked(*slot->space",
		"slot->device->setProvideControlMode(\"never\");")
	for _, forbidden := range []string{
		"Wintun::Load(", "WintunAdapter::Create(", "std::make_unique<NetworkConfig>",
		"ApplyWfpLocked(", "wfp_.Apply(", "setRpcServer(", "std::make_unique<EgressMonitor>",
		"std::make_unique<PacketPump>", "SetActiveMarker(", "splitTunnel_.",
		"PushExcludedToDriver(", "setFlowOwnerLookup(", "setEgressInterfaceIndex(",
		"SetStateLocked(", " device_ = ", "providerDevice_ = ", "networkSpace_ = ",
		"OpenProviderStatsLocked(", "WatchProviderNetworkLocked(",
	} {
		if strings.Contains(upload, forbidden) {
			t.Errorf("UploadLogs must not call %q: the standalone device changes nothing on "+
				"this machine", forbidden)
		}
	}
}

// The sdk's own upload, with the server's feedback id, after the line naming
// the carrier (written through the hook once the device is chosen, and never
// directly: ServiceDiagnostics is the uploaded log's one writer); the callback
// only tells the waiter, which retires the standalone device on that report or
// at the bound; a device built before a failure is closed.
func TestLogUploadWiringUploadAndRetirement(t *testing.T) {
	source := tunnelControllerSource(t)
	upload := definitionBody(t, "TunnelController.cpp", source, "bool TunnelController::UploadLogs(")
	provideRequireOrder(t, "UploadLogs", upload, "logUpload_ = slot;",
		"if (noteCarrier) noteCarrier(carrierName);")
	provideRequireOrder(t, "UploadLogs", upload, "if (noteCarrier) noteCarrier(carrierName);",
		"device->uploadLogs(")
	if strings.Contains(upload, "logAppInfo(") {
		t.Error("UploadLogs must write the carrier's line through the hook, not into the log itself")
	}
	provideRequire(t, "UploadLogs", upload,
		"request.feedback_id,",
		"slot->uploadReported = true;",
		"slot->reported.notify_all();",
		"slot->reported.wait_for(slotLock, logupload::kStandaloneDeviceMaxLifetime,",
		"return slot->uploadReported || !slot->device;",
		".detach();")
	// the failure path is the catch block alone: the waiter's close further down
	// must not stand in for it
	catchAt := strings.Index(upload, "catch (const std::exception&)")
	catchEnd := -1
	if catchAt >= 0 {
		catchEnd = strings.Index(upload[catchAt:], "return false;")
	}
	if catchAt < 0 || catchEnd < 0 ||
		!strings.Contains(upload[catchAt:catchAt+catchEnd], "CloseLogUploadDevice(slot);") {
		t.Error("UploadLogs must close a standalone device built before a failure, not only release it")
	}
	// the capture list is the slot alone: the detached waiter may outlive the
	// controller
	waiterAt := strings.Index(upload, "std::thread([slot]")
	waiterEnd := -1
	if waiterAt >= 0 {
		waiterEnd = strings.Index(upload[waiterAt:], ".detach();")
	}
	if waiterAt < 0 || waiterEnd < 0 {
		t.Fatal("UploadLogs must start a detached waiter that owns only the slot")
	}
	if !strings.Contains(upload[waiterAt:waiterAt+waiterEnd], "CloseLogUploadDevice(slot);") {
		t.Error("the waiter must close the standalone device itself")
	}

	closeBody := definitionBody(t, "TunnelController.cpp", source,
		"void TunnelController::CloseLogUploadDevice(")
	provideRequire(t, "CloseLogUploadDevice", closeBody,
		"std::scoped_lock slotLock(slot->mutex);",
		"device = std::move(slot->device);",
		"device->close();",
		"AbandonHazard::HoldsSessionDevice")
	// taken under the slot's lock, closed outside it
	provideRequireOrder(t, "CloseLogUploadDevice", closeBody, "slot->space.reset();\n  }",
		"RunBounded(")
}

// The standalone device never shares a moment with another device under the
// same identity: every teardown (and so every bring-up) and every
// provider-only start retire it first, the provider's after its refusals.
func TestLogUploadWiringEveryOtherDeviceRetiresIt(t *testing.T) {
	source := tunnelControllerSource(t)
	stop := definitionBody(t, "TunnelController.cpp", source,
		"void TunnelController::StopLocked(bool finalDisarm)")
	provideRequireOrder(t, "StopLocked", stop, "RetireProviderDeviceLocked();",
		"RetireLogUploadDeviceLocked();")
	provideRequireOrder(t, "StopLocked", stop, "RetireLogUploadDeviceLocked();",
		"TearDownSessionLocked();")
	start := definitionBody(t, "TunnelController.cpp", source,
		"bool TunnelController::StartProvider(")
	provideRequireOrder(t, "StartProvider", start, "provide::ProviderStartRefusal(mode, state)",
		"RetireLogUploadDeviceLocked();")
	provideRequireOrder(t, "StartProvider", start, "RetireLogUploadDeviceLocked();",
		"NewDeviceLocked(*providerSpace_")
	retire := definitionBody(t, "TunnelController.cpp", source,
		"void TunnelController::RetireLogUploadDeviceLocked()")
	provideRequireOrder(t, "RetireLogUploadDeviceLocked", retire,
		"CloseLogUploadDevice(logUpload_);", "logUpload_.reset();")
}

// The app asks the service first with the credentials start_provider carries,
// without holding its lock across the pipe call, and falls back to the
// DeviceRemote only when the service did not take it; the page leaves the UI
// thread before any of it, and still attaches only after the server accepted
// the feedback with the box ticked.
func TestLogUploadWiringAppAsksTheServiceFirst(t *testing.T) {
	client := definitionBody(t, "ServiceClient.cpp", stripComments(readAppSource(t, "ServiceClient.cpp")),
		"bool ServiceClient::UploadLogs(")
	provideRequire(t, "ServiceClient::UploadLogs", client,
		"proto::Request(proto::msg::kUploadLogs, body)",
		"*carrier = r.log_upload_carrier;",
		"return r.ok;")

	upload := definitionBody(t, "SdkHost.cpp", sdkHostSource(t),
		"void SdkHost::UploadFeedbackLogs(const std::string& feedbackId)")
	provideRequire(t, "UploadFeedbackLogs", upload,
		"request.feedback_id = feedbackId;",
		"request.by_jwt = localState_->getByClientJwt();",
		"request.instance_id = localState_->getInstanceId();",
		"request.device_description = DeviceDescription();",
		"request.device_spec = DeviceSpec();",
		"request.app_version = appVersion_;",
		"request.network_space_json = networkSpace_->toJson();",
		"logupload::AppStepAfterService(serviceAccepted, device_.has_value())")
	provideRequireOrder(t, "UploadFeedbackLogs", upload,
		"request.network_space_json = networkSpace_->toJson();",
		"service_.UploadLogs(request, &carrier, &error)")
	provideRequireOrder(t, "UploadFeedbackLogs", upload,
		"service_.UploadLogs(request, &carrier, &error)", "device_->uploadLogs(feedbackId,")
	// the session is read under the lock in its own scope, and the pipe call is
	// made with no lock held: the next lock is taken only after it returns
	firstLock := strings.Index(upload, "std::scoped_lock lock(mutex_);")
	call := strings.Index(upload, "service_.UploadLogs(")
	if firstLock < 0 || call < 0 || !strings.Contains(upload[firstLock:call], "\n  }\n") ||
		strings.Contains(upload[firstLock+1:call], "std::scoped_lock lock(mutex_);") {
		t.Error("UploadFeedbackLogs must release mutex_ before asking the service")
	}

	page := stripComments(readAppSource(t, "SettingsPage.cpp"))
	attach := definitionBody(t, "SettingsPage.cpp", page,
		"winrt::fire_and_forget SettingsPage::UploadLogs(std::string feedbackId)")
	provideRequireOrder(t, "SettingsPage::UploadLogs", attach, "co_await winrt::resume_background();",
		"sdk->UploadFeedbackLogs(feedbackId);")
	if strings.Contains(page, "device().uploadLogs(") {
		t.Error("SettingsPage must not upload through the DeviceRemote itself; SdkHost decides")
	}
	provideRequire(t, "OnSendFeedback", page,
		"if (attachLogs && !feedbackId.empty()) page.UploadLogs(feedbackId);")
}
