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
// The upload runs on a thread of its own (Flight), never under the session
// lock: the sdk zips the log directory inside its UploadLogs call, up to the
// upload's cap read from disk. The request is answered once the upload is
// admitted, and its outcome reaches the app through the status the service
// pushes when it ends (CompletionFor).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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

// ---- the upload in flight ----------------------------------------------------

// Where the upload in flight is, as status reports it
// (proto::TunnelStatus::log_upload_state).
enum class FlightState {
  None,      // nothing asked since the service started
  Running,   // the zip or the post is under way
  Uploaded,  // the server took it
  Refused,   // the server answered with an error (its rate limit, its cap)
  Failed,    // it did not reach the server, or never reported
};

// The wire value ("" for none).
constexpr const char* ToString(FlightState state) {
  switch (state) {
    case FlightState::None: return "";
    case FlightState::Running: return "running";
    case FlightState::Uploaded: return "uploaded";
    case FlightState::Refused: return "refused";
    case FlightState::Failed: return "failed";
  }
  return "";
}

// The state for a wire value; one this build does not know is none.
constexpr FlightState FlightStateFromString(std::string_view value) {
  if (value == "running") return FlightState::Running;
  if (value == "uploaded") return FlightState::Uploaded;
  if (value == "refused") return FlightState::Refused;
  if (value == "failed") return FlightState::Failed;
  return FlightState::None;
}

// The upload has an outcome.
constexpr bool IsFinished(FlightState state) {
  return state == FlightState::Uploaded || state == FlightState::Refused ||
         state == FlightState::Failed;
}

// How long an upload may run without an outcome before the flight gives up on
// it: the standalone device's bound.
inline constexpr int64_t kSilentUploadMaxMillis =
    std::chrono::duration_cast<std::chrono::milliseconds>(kStandaloneDeviceMaxLifetime).count();

// The service's upload in flight: one at a time, run on a thread of its own.
// The server admits one upload per network per 5 minutes and keeps one file
// per feedback, so a second upload while one runs would only be refused; a
// request then is answered busy.
//
// The request thread admits (Begin) and runs (Run) an upload under the session
// lock, and Status reads the flight (Read) without it. The upload's thread runs
// the sdk's call and its callback ends the upload (Finish); they hold a share
// of the flight and nothing of the service. The flight's lock is innermost, and
// it is never held across the call, a device's release or the finish hook.
// Made with std::make_shared: the upload's thread holds a share of it.
//
// The call's device stays alive until the call returns: a teardown that
// releases a device the call is on hands it to the flight instead
// (KeepUntilReturned), and the upload's thread releases it once out of the
// call. The device is closed by its teardown as always; the sdk's upload goes
// on on a closed device, through its network space's api.
class Flight : public std::enable_shared_from_this<Flight> {
 public:
  // What status reports of the flight.
  struct Reading {
    // 0 before the first upload
    int64_t id = 0;
    FlightState state = FlightState::None;
    Carrier carrier = Carrier::Standalone;
  };

  // `firstId` numbers the first upload. The service starts it from its start
  // time, so that an app waiting on an id across a service restart does not
  // take another upload's outcome for its own.
  explicit Flight(int64_t firstId) : nextId_(firstId) {}

  Flight(const Flight&) = delete;
  Flight& operator=(const Flight&) = delete;

  // Called after an upload ends (Finish), on the thread that ended it and
  // outside the flight's lock: the service pushes its status then.
  void SetOnFinished(std::function<void()> onFinished) {
    std::scoped_lock lock(mutex_);
    onFinished_ = std::move(onFinished);
  }

  // No finish hook runs once this returns: a call of it that is running is
  // waited out. Never from the hook, which it would wait for.
  void ClearOnFinished() {
    std::unique_lock lock(mutex_);
    onFinished_ = nullptr;
    wake_.wait(lock, [this] { return notifying_ == 0; });
  }

  // Admits an upload on `carrier`: its id, or 0 when one is in flight, or its
  // thread is still in the sdk's call.
  int64_t Begin(Carrier carrier, int64_t nowMillis) {
    std::scoped_lock lock(mutex_);
    ExpireSilentWithLock(nowMillis);
    if (BusyWithLock()) return 0;
    reading_.id = nextId_++;
    reading_.state = FlightState::Running;
    reading_.carrier = carrier;
    sinceMillis_ = nowMillis;
    return reading_.id;
  }

  // Runs `call`, the sdk's upload on the device behind `deviceHandle`, on a
  // thread of its own, and returns at once. Once `call` returns, the devices
  // kept for it (KeepUntilReturned) are released on that thread. `call` must
  // touch nothing of the caller; it ends the upload through Finish.
  void Run(int64_t id, uint64_t deviceHandle, std::function<void()> call) {
    {
      std::scoped_lock lock(mutex_);
      calling_ = true;
      callingId_ = id;
      callingDeviceHandle_ = deviceHandle;
    }
    std::thread([flight = shared_from_this(), id, call = std::move(call)] {
      try {
        call();
      } catch (...) {
        flight->Finish(id, FlightState::Failed);
      }
      // released here, outside the lock: a release is a call into the sdk
      std::vector<std::shared_ptr<void>> keptDevices = flight->Returned(id);
      keptDevices.clear();
      flight->Released(id);
    }).detach();
  }

  // A teardown is about to release `device`, the device behind `deviceHandle`.
  // While the upload's call is on it, the flight keeps it until the call
  // returns, and this answers null. Otherwise it is handed back for the caller
  // to release now.
  std::shared_ptr<void> KeepUntilReturned(uint64_t deviceHandle, std::shared_ptr<void> device) {
    std::scoped_lock lock(mutex_);
    if (!calling_ || callingDeviceHandle_ != deviceHandle || deviceHandle == 0) return device;
    keptDevices_.push_back(std::move(device));
    return nullptr;
  }

  // The upload `id` ended with `state`, once, and the finish hook runs. Nothing
  // for an upload no longer in flight, or one that has ended already.
  void Finish(int64_t id, FlightState state) {
    std::function<void()> onFinished;
    {
      std::scoped_lock lock(mutex_);
      if (reading_.id != id || IsFinished(reading_.state) || !IsFinished(state)) return;
      reading_.state = state;
      onFinished = onFinished_;
      if (onFinished) notifying_ += 1;
    }
    if (!onFinished) return;
    onFinished();
    {
      std::scoped_lock lock(mutex_);
      notifying_ -= 1;
    }
    wake_.notify_all();
  }

  // The flight now. An upload that has run kSilentUploadMaxMillis without an
  // outcome is given up on (failed), so that a post that never reports cannot
  // keep every later upload out.
  Reading Read(int64_t nowMillis) {
    std::scoped_lock lock(mutex_);
    ExpireSilentWithLock(nowMillis);
    return reading_;
  }

  // Waits up to `budget` for the upload's thread to be out of the sdk's call,
  // with the devices kept for it released; true once it is. For the service's
  // teardown and for tests.
  bool WaitReturned(std::chrono::milliseconds budget) {
    std::unique_lock lock(mutex_);
    return wake_.wait_for(lock, budget, [this] { return !calling_ && !releasing_; });
  }

 private:
  // The thread is out of the sdk's call: the devices kept for it, to release.
  // From here a device is handed back to its teardown (KeepUntilReturned).
  std::vector<std::shared_ptr<void>> Returned(int64_t id) {
    std::scoped_lock lock(mutex_);
    if (callingId_ != id) return {};
    calling_ = false;
    releasing_ = true;
    callingDeviceHandle_ = 0;
    return std::exchange(keptDevices_, {});
  }

  // The kept devices are released: the thread is done with the upload.
  void Released(int64_t id) {
    {
      std::scoped_lock lock(mutex_);
      if (callingId_ != id) return;
      releasing_ = false;
    }
    wake_.notify_all();
  }

  bool BusyWithLock() const {
    return calling_ || releasing_ || reading_.state == FlightState::Running;
  }

  void ExpireSilentWithLock(int64_t nowMillis) {
    if (reading_.state == FlightState::Running &&
        nowMillis - sinceMillis_ >= kSilentUploadMaxMillis) {
      reading_.state = FlightState::Failed;
    }
  }

  std::mutex mutex_;
  std::condition_variable wake_;
  int64_t nextId_ = 1;
  Reading reading_;
  int64_t sinceMillis_ = 0;
  std::function<void()> onFinished_;
  // finish hooks running now (ClearOnFinished waits them out)
  int notifying_ = 0;
  // the upload's thread is in the sdk's call, on this upload's device; then
  // releases the devices kept for it
  bool calling_ = false;
  bool releasing_ = false;
  int64_t callingId_ = 0;
  uint64_t callingDeviceHandle_ = 0;
  std::vector<std::shared_ptr<void>> keptDevices_;
};

// ---- the app's half ---------------------------------------------------------
// After the server accepted the feedback with the box ticked, the app asks the
// service first. The service's acceptance ends it, and so does its answer that
// an upload is in flight already: the server would refuse a second one. Any
// other answer — no service, a service that predates the verb ("unknown
// request type"), a refusal — falls back to what the app did before the verb
// existed: the DeviceRemote's UploadLogs while a session is bound, nothing
// otherwise. A lost reply can therefore cost a second upload, which the server
// refuses (one per network per 5 minutes, one file per feedback).
enum class ServiceAnswer {
  Accepted,  // the service admitted the upload
  Busy,      // an upload is in flight already
  NotTaken,  // no service, an older service, a refusal
};

enum class AppStep {
  Done,          // the service took it, or has one in flight
  DeviceRemote,  // the old path
  Skip,          // nothing can carry it
};

constexpr AppStep AppStepAfterService(ServiceAnswer answer, bool deviceRemoteBound) {
  if (answer != ServiceAnswer::NotTaken) return AppStep::Done;
  if (deviceRemoteBound) return AppStep::DeviceRemote;
  return AppStep::Skip;
}

// The upload's outcome, once the service's status names the upload the app
// waits on (`pendingId`, from the reply) as finished; none before, and none
// for another upload.
constexpr std::optional<FlightState> CompletionFor(int64_t pendingId, int64_t statusId,
                                                   FlightState statusState) {
  if (pendingId == 0 || statusId != pendingId || !IsFinished(statusState)) return std::nullopt;
  return statusState;
}

}  // namespace urnw::logupload
