// SPDX-License-Identifier: MPL-2.0
#include "PipeClient.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <stdexcept>

#include "Ids.h"
#include "Log.h"
#include "Protocol.h"

namespace urnw {

PipeClient::~PipeClient() { Close(); }

bool PipeClient::Connect() {
  // Already up: do not tear a live channel down to rebuild it.
  if (connected_.load()) return true;

  // RECLAIM WHATEVER THE LAST CONNECTION LEFT, BEFORE ANYTHING ELSE.
  //
  // When the service dies the reader thread breaks out of its loop and clears
  // connected_ — but the std::thread object stays JOINABLE and pipe_ stays a
  // live HANDLE. Reconnecting without this line therefore did two things: it
  // leaked the old handle, and it MOVE-ASSIGNED over a joinable std::thread,
  // which is defined to call std::terminate. So the first successful reconnect
  // after a service restart killed the app outright, and it killed it inside
  // BootstrapSession — i.e. on the one path whose whole job is to recover from
  // the service having gone away. Close() is idempotent and joins the dead
  // reader, which is all that is needed.
  Close();

  // The pipe is a message-mode byte stream we frame by newline. Retry briefly
  // if the single instance is momentarily busy between clients.
  //
  // "Briefly" is load-bearing and used not to be. This loop ran 20 attempts
  // with a 500ms WaitNamedPipeW between them, so a pipe that EXISTS but never
  // frees an instance cost the caller a full 10 SECONDS -- measured at 10.19s
  // on a machine where urnetworkd was running and the control pipe was already
  // held by another client. That is not hypothetical: SdkHost::Initialize calls
  // Connect() synchronously during startup, BEFORE the main window is created,
  // so the whole app sat invisible for ten seconds and then appeared -- which
  // also swallowed the window reveal, because the animation played at the
  // instant the window finally showed and read as "it finally opened".
  //
  // A server that closes one client and accepts the next does it in single-
  // digit milliseconds, so the budget only ever needed to cover that handoff.
  // Failing fast is what the caller already expects: SdkHost documents this
  // call as ok if the service is not up yet, and retries it on demand.
  constexpr int kConnectAttempts = 5;
  constexpr DWORD kBusyWaitMs = 100;  // total budget ~500ms, was ~10s
  const auto connectStart = std::chrono::steady_clock::now();
  for (int attempt = 0; attempt < kConnectAttempts; ++attempt) {
    HANDLE h = ::CreateFileW(ids::kControlPipeName, GENERIC_READ | GENERIC_WRITE,
                             0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                             nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      pipe_ = h;
      connected_.store(true);
      stopping_.store(false);
      reader_ = std::thread([this] { ReaderLoop(); });
      return true;
    }
    if (::GetLastError() != ERROR_PIPE_BUSY) {
      return false;  // service not running
    }
    ::WaitNamedPipeW(ids::kControlPipeName, kBusyWaitMs);
  }
  // Say it, rather than returning a bare false. A pipe that is present but
  // never free is a different fault from a service that is not running, and
  // the old code made the two indistinguishable at the call site.
  const auto waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - connectStart)
                            .count();
  LogWarn("pipe: control pipe present but no instance came free after {} attempts ({}ms) -- "
          "continuing without the service; the connection is retried on demand",
          kConnectAttempts, waitedMs);
  return false;
}

void PipeClient::Close() {
  stopping_.store(true);
  if (pipe_) {
    ::CancelIoEx(static_cast<HANDLE>(pipe_), nullptr);
    ::CloseHandle(static_cast<HANDLE>(pipe_));
    pipe_ = nullptr;
  }
  if (reader_.joinable()) reader_.join();
  connected_.store(false);
}

bool PipeClient::WriteLine(const std::string& line) {
  // Overlapped write with a completion wait; the control channel messages are
  // tiny (JSON), so a synchronous-style overlapped write is fine.
  OVERLAPPED ov{};
  ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ov.hEvent) return false;
  bool ok = false;
  DWORD written = 0;
  if (::WriteFile(static_cast<HANDLE>(pipe_), line.data(),
                  static_cast<DWORD>(line.size()), nullptr, &ov) ||
      ::GetLastError() == ERROR_IO_PENDING) {
    if (::GetOverlappedResult(static_cast<HANDLE>(pipe_), &ov, &written, TRUE)) {
      ok = (written == line.size());
    }
  }
  ::CloseHandle(ov.hEvent);
  return ok;
}

void PipeClient::ReaderLoop() {
  std::string buffer;
  char chunk[4096];
  OVERLAPPED ov{};
  ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);

  while (!stopping_.load()) {
    ::ResetEvent(ov.hEvent);
    DWORD read = 0;
    BOOL ok = ::ReadFile(static_cast<HANDLE>(pipe_), chunk, sizeof(chunk),
                         nullptr, &ov);
    if (!ok && ::GetLastError() != ERROR_IO_PENDING) break;
    if (!::GetOverlappedResult(static_cast<HANDLE>(pipe_), &ov, &read, TRUE)) break;
    if (read == 0) continue;

    buffer.append(chunk, read);
    size_t nl;
    while ((nl = buffer.find('\n')) != std::string::npos) {
      std::string line = buffer.substr(0, nl);
      buffer.erase(0, nl + 1);
      if (line.empty()) continue;

      nlohmann::json j;
      try {
        j = nlohmann::json::parse(line);
      } catch (const std::exception& e) {
        LogWarn("control: bad json from service: {}", e.what());
        continue;
      }

      const std::string type = proto::TypeOf(j);
      if (type == proto::msg::kEvent) {
        if (onEvent_) onEvent_(j);
      } else {
        // a reply — hand it to the waiting Call
        std::scoped_lock lock(replyMutex_);
        reply_ = std::move(j);
        haveReply_ = true;
        replyCv_.notify_all();
      }
    }
  }

  ::CloseHandle(ov.hEvent);
  // exchange, not store: the handler fires ONCE per connection, and Close()
  // racing this must not produce a second one.
  const bool wasConnected = connected_.exchange(false);

  // An orderly Close() is not a disconnection to report — the caller asked for
  // it and already knows. Only an unasked-for break is news, and it is the only
  // signal this process gets that the service (and with it the tun, its routes
  // and the whole WFP session) is gone.
  if (wasConnected && !stopping_.load()) {
    LogWarn("control: the connection to the URnetwork service dropped. The "
            "service owns the tunnel, so if its process exited the wintun "
            "adapter and every filter it installed went with it — nothing is "
            "connected and nothing is protected as of now.");
    if (onDisconnect_) onDisconnect_();
  }
}

nlohmann::json PipeClient::Call(const nlohmann::json& request, int timeoutMs) {
  std::scoped_lock callLock(callMutex_);
  if (!connected_.load()) throw std::runtime_error("control channel not connected");

  {
    std::scoped_lock lock(replyMutex_);
    haveReply_ = false;
  }

  const std::string line = request.dump() + "\n";
  if (!WriteLine(line)) throw std::runtime_error("control channel write failed");

  std::unique_lock lock(replyMutex_);
  if (!replyCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                         [this] { return haveReply_; })) {
    throw std::runtime_error("control channel timeout");
  }
  return std::move(reply_);
}

}  // namespace urnw
