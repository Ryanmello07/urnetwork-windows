// "Send feedback with logs" whether or not a tunnel runs (support inbox 2090):
// which device in the service carries the upload, when the service refuses to
// build one for it, how long that one lives, and what the app does with the
// service's answer. The wire contract is Protocol.h's upload_logs; this is the
// decision both halves make, WinRT-free and SDK-free so the tools harness runs
// it on any host (tools/log-upload-tests.cpp), the way ProvideLifecycle.h is
// for the provider-only device.
//
// The logs support reads are the service's: the sdk's UploadLogs zips the glog
// files of the process it runs in. The app's DeviceRemote reaches the service's
// DeviceLocal only while a session runs, so a report sent while disconnected,
// held by the kill switch or failing to connect carried no logs. The service
// now uploads its own logs on request, on whichever device runs:
//
//   tunnel      the session's DeviceLocal (what the DeviceRemote reached)
//   provider    the provider-only device, while there is no session
//   standalone  neither runs: a device built for the upload from the request's
//               credentials, as start_provider builds its device, with provide
//               mode never and nothing else, retired once the upload reports
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>

namespace urnw::logupload {

enum class Carrier {
  Tunnel,
  Provider,
  Standalone,
};

// The reply's carrier (proto::Reply::log_upload_carrier) and the log's word.
constexpr const char* ToString(Carrier carrier) {
  switch (carrier) {
    case Carrier::Tunnel: return "tunnel";
    case Carrier::Provider: return "provider";
    case Carrier::Standalone: return "standalone";
  }
  return "standalone";
}

// The device that carries the upload. The session's first: it is the one the
// DeviceRemote path always reached, and a provider-only device never runs
// beside it. A standalone device only when neither runs, because a second
// device under the same identity would compete with the first.
constexpr Carrier CarrierFor(bool sessionDevice, bool providerDevice) {
  if (sessionDevice) return Carrier::Tunnel;
  if (providerDevice) return Carrier::Provider;
  return Carrier::Standalone;
}

// Why the service builds no standalone device, decided before anything is
// built. Two of the provider-only device's refusals and no others: a held
// device would run under the same identity, and a restarting service would
// abandon the upload. The kill switch's armed floor is deliberately not one:
// it permits this service's own image, and the upload is the service talking
// to URnetwork's API as a reconnect would, not traffic for anyone else.
enum class StandaloneRefusal {
  None,
  DeviceStillHeld,
  RestartPending,
};

constexpr StandaloneRefusal StandaloneRefusalFor(bool deviceStillHeld, bool restartPending) {
  if (deviceStillHeld) return StandaloneRefusal::DeviceStillHeld;
  if (restartPending) return StandaloneRefusal::RestartPending;
  return StandaloneRefusal::None;
}

// The refusal as the control reply's error. English, for the log; the app acts
// on the reply's `ok`, never on this text.
constexpr const char* RefusalReason(StandaloneRefusal refusal) {
  switch (refusal) {
    case StandaloneRefusal::DeviceStillHeld:
      return "a previous teardown is still holding this device's identity";
    case StandaloneRefusal::RestartPending:
      return "this service is restarting itself";
    case StandaloneRefusal::None:
      break;
  }
  return "";
}

// How long a standalone device may wait for its upload to report. The upload
// has no deadline of its own (the sdk's streaming POST is bounded by its dial
// and HTTP/2 progress limits only), and the device connects to the platform
// like any other, so it must not outlive a stuck upload by much. 30 minutes
// carries the server's 100 MB cap at under half a megabit per second.
inline constexpr std::chrono::minutes kStandaloneDeviceMaxLifetime{30};

// ---- the app's half ---------------------------------------------------------
// After the server accepted the feedback with the box ticked, the app asks the
// service first. Anything but its acceptance — no service, a service that
// predates the verb ("unknown request type"), a refusal — falls back to what
// the app did before the verb existed: the DeviceRemote's UploadLogs while a
// session is bound, nothing otherwise. A lost reply can therefore cost a second
// upload, which the server refuses (one per network per 5 minutes, one file
// per feedback).
enum class AppStep {
  Done,          // the service took it
  DeviceRemote,  // the old path
  Skip,          // nothing can carry it
};

constexpr AppStep AppStepAfterService(bool serviceAccepted, bool deviceRemoteBound) {
  if (serviceAccepted) return AppStep::Done;
  if (deviceRemoteBound) return AppStep::DeviceRemote;
  return AppStep::Skip;
}

}  // namespace urnw::logupload
