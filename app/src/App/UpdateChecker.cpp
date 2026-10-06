// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "UpdateChecker.h"

#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <format>
#include <fstream>
#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

#include "Log.h"
#include "Paths.h"
#include "ReleaseJson.h"
#include "ReleaseSelection.h"
#include "SingleInstance.h"
#include "Strings.h"
#include "UpdateFormats.h"
#include "Version.h"
#include "VersionGrammar.h"

namespace urnw {
namespace {

namespace fs = std::filesystem;
using std::chrono::steady_clock;

// The launch check waits out the startup rush (SDK init, service reconnect,
// the tray settling) rather than adding an HTTP request to it; the cadence is
// the spec's 6 hours.
constexpr auto kLaunchDelay = std::chrono::seconds(30);
constexpr auto kCheckInterval = std::chrono::hours(6);

// Response caps. The release LIST is JSON that should be a few hundred KB
// (a stable release may carry every platform's assets); the MSI is ~100 MB
// self-contained today. A cap is not a guess about the future, it
// is the refusal to stream an unbounded body into a file because a server
// said so.
constexpr std::uint64_t kMaxJsonBytes = 8ull * 1024 * 1024;
constexpr std::uint64_t kMaxMsiBytes = 1ull * 1024 * 1024 * 1024;

// The arch half of the MSI asset name grammar URnetwork-<version>-<x64|arm64>.msi
// (ReleaseSelection.h) — decided at compile time because a binary only ever
// updates itself to its own architecture.
#if defined(_M_ARM64)
constexpr const char kArch[] = "arm64";
#else
constexpr const char kArch[] = "x64";
#endif

// ---- the app's own preferences ----------------------------------------------
//
// Deliberately the same 20 lines as SdkHost.cpp's file-local pair, not a call
// into them: that unit keeps its helpers private on purpose, and the whole-
// object read-modify-write discipline (never serialize just your own key —
// that deletes everyone else's) is the part that must match, which the
// selftest cannot check but a reviewer can. A third preference site is the
// signal to promote this into Common/Paths.
// LoadAppPrefs / SaveAppPref moved to Common/Paths at the third
// preference site, as the note above prescribed.

constexpr char kAutoCheckPrefKey[] = "check_updates_automatically";

// ---- paths -------------------------------------------------------------------

// %LOCALAPPDATA%\URnetwork\updates (spec §5): a sibling of the app storage
// root rather than a second known-folder lookup, so a worktree's
// URNETWORK_APP_ROOT override isolates update downloads the same way it
// isolates everything else.
fs::path UpdatesDir() { return StorageRoot(/*isService=*/false).parent_path() / L"updates"; }

// Unbounded, like the service's own OwnExePath (Service/main.cpp): the
// portable folder can sit under a long-path-enabled tree, and a MAX_PATH
// truncation here would silently disable the stale-file cleanup.
fs::path OwnExePath() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = ::GetModuleFileNameW(nullptr, path.data(),
                                         static_cast<DWORD>(path.size()));
    if (n == 0) return {};
    if (n < path.size()) {
      path.resize(n);
      return fs::path(std::move(path));
    }
    if (path.size() >= 0x8000) return {};  // beyond the NT path limit: give up
    path.resize(path.size() * 2);
  }
}

// ---- WinHTTP -----------------------------------------------------------------

struct HInternet {
  HINTERNET h = nullptr;
  ~HInternet() {
    if (h) ::WinHttpCloseHandle(h);
  }
};

struct UrlParts {
  std::wstring host;
  std::wstring path;
  INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
};

// https only, always: both the API and the asset hosts are https, and a
// redirect that tried to step down to http is refused by WinHTTP's default
// redirect policy anyway — this just refuses it one step earlier.
std::optional<UrlParts> CrackHttpsUrl(std::wstring const& url) {
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength =
      static_cast<DWORD>(-1);
  if (!::WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc))
    return std::nullopt;
  if (uc.nScheme != INTERNET_SCHEME_HTTPS) return std::nullopt;
  UrlParts p;
  p.host.assign(uc.lpszHostName, uc.dwHostNameLength);
  p.path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
  if (uc.dwExtraInfoLength) p.path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
  p.port = uc.nPort;
  return p;
}

// The response's Date header in Unix seconds, or 0 when it has none.
std::int64_t ResponseDateUnixSeconds(HINTERNET request) {
  SYSTEMTIME date{};
  DWORD size = sizeof(date);
  if (!::WinHttpQueryHeaders(request, WINHTTP_QUERY_DATE | WINHTTP_QUERY_FLAG_SYSTEMTIME,
                             WINHTTP_HEADER_NAME_BY_INDEX, &date, &size,
                             WINHTTP_NO_HEADER_INDEX)) {
    return 0;
  }
  FILETIME file{};
  if (!::SystemTimeToFileTime(&date, &file)) return 0;
  ULARGE_INTEGER ticks{};
  ticks.LowPart = file.dwLowDateTime;
  ticks.HighPart = file.dwHighDateTime;
  // FILETIME counts 100 ns from 1601-01-01, 11644473600 s before Unix time
  return static_cast<std::int64_t>(ticks.QuadPart / 10000000ULL) - 11644473600LL;
}

// What a GET said beside its body.
struct FetchHeaders {
  // The Date header in Unix seconds, 0 when it had none.
  std::int64_t serverUnixSeconds = 0;
};

// One GET, streamed into `sink` chunk by chunk. GitHub requires a User-Agent
// on every request (a bare WinHTTP GET gets 403). With `followRedirects`
// false a redirect is a failure: the release list's URL names the repository
// by its id, and nothing may move it. The tray's own download of the MSI, a
// browser_download_url that 302s to a storage host, follows WinHTTP's
// default https->https policy; what it fetches is only ever shown to the user
// or checked, never handed to an elevated process. `cancelled` is polled
// between reads so a Stop() during a 100 MB download aborts within one chunk
// instead of finishing it.
bool FetchUrl(std::wstring const& url, const wchar_t* accept,
              std::uint64_t maxBytes, bool followRedirects,
              std::function<bool(const char*, DWORD)> const& sink,
              std::function<bool()> const& cancelled, FetchHeaders& headers,
              std::string& error) {
  const auto parts = CrackHttpsUrl(url);
  if (!parts) {
    error = "not an https url";
    return false;
  }

  const std::wstring userAgent =
      L"URnetwork-Windows/" + Widen(version::kString);
  HInternet session{::WinHttpOpen(userAgent.c_str(),
                                  WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                                  0)};
  if (!session.h) {
    error = std::format("WinHttpOpen failed: {}", ::GetLastError());
    return false;
  }
  ::WinHttpSetTimeouts(session.h, 10000, 10000, 30000, 30000);

  HInternet connection{
      ::WinHttpConnect(session.h, parts->host.c_str(), parts->port, 0)};
  if (!connection.h) {
    error = std::format("WinHttpConnect failed: {}", ::GetLastError());
    return false;
  }

  HInternet request{::WinHttpOpenRequest(connection.h, L"GET",
                                         parts->path.c_str(), nullptr,
                                         WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE)};
  if (!request.h) {
    error = std::format("WinHttpOpenRequest failed: {}", ::GetLastError());
    return false;
  }
  if (!followRedirects) {
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!::WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy,
                            sizeof(policy))) {
      error = std::format("WinHttpSetOption(redirect policy) failed: {}", ::GetLastError());
      return false;
    }
  }
  if (accept) {
    const std::wstring header = std::wstring(L"Accept: ") + accept;
    ::WinHttpAddRequestHeaders(request.h, header.c_str(),
                               static_cast<DWORD>(-1),
                               WINHTTP_ADDREQ_FLAG_ADD);
  }

  if (!::WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !::WinHttpReceiveResponse(request.h, nullptr)) {
    error = std::format("request failed: {}", ::GetLastError());
    return false;
  }

  DWORD status = 0;
  DWORD statusSize = sizeof(status);
  ::WinHttpQueryHeaders(request.h,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
  headers.serverUnixSeconds = ResponseDateUnixSeconds(request.h);
  if (status != 200) {
    error = std::format("http status {}", status);
    return false;
  }

  std::uint64_t total = 0;
  std::vector<char> chunk;
  for (;;) {
    if (cancelled && cancelled()) {
      error = "cancelled";
      return false;
    }
    DWORD available = 0;
    if (!::WinHttpQueryDataAvailable(request.h, &available)) {
      error = std::format("WinHttpQueryDataAvailable failed: {}", ::GetLastError());
      return false;
    }
    if (available == 0) return true;  // the body is complete
    chunk.resize(available);
    DWORD read = 0;
    if (!::WinHttpReadData(request.h, chunk.data(), available, &read)) {
      error = std::format("WinHttpReadData failed: {}", ::GetLastError());
      return false;
    }
    total += read;
    if (total > maxBytes) {
      error = std::format("response larger than the {} byte cap", maxBytes);
      return false;
    }
    if (read && !sink(chunk.data(), read)) {
      error = "write failed";
      return false;
    }
  }
}

// ---- SHA-256 (CNG) -----------------------------------------------------------

// The MSI's hash, streamed through BCrypt, as lowercase hex — the same
// canonical form DigestHexFromAssetDigest returns, so the comparison could be
// bytewise (it is folded anyway; hex case is not worth a failure mode). Empty
// on any failure: an unreadable file must fail verification, not pass it.
std::string Sha256File(fs::path const& file) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::string hex;
  if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
    return {};
  do {
    if (::BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) break;
    std::ifstream in(file, std::ios::binary);
    if (!in) break;
    std::vector<char> buf(64 * 1024);
    bool failed = false;
    while (in) {
      in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
      const std::streamsize n = in.gcount();
      if (n <= 0) break;
      if (::BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()),
                           static_cast<ULONG>(n), 0) != 0) {
        failed = true;
        break;
      }
    }
    if (failed || in.bad()) break;
    UCHAR digest[32];
    if (::BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0) break;
    hex.reserve(64);
    for (UCHAR b : digest) hex += std::format("{:02x}", b);
  } while (false);
  if (hash) ::BCryptDestroyHash(hash);
  ::BCryptCloseAlgorithmProvider(alg, 0);
  return hex;
}

// ---- msiexec -----------------------------------------------------------------

// Start the verified MSI with the OS's own msiexec (System32 path, not PATH,
// so nothing a user installed can interpose), elevated up front with the
// "runas" verb: the package is per-machine, and asking here makes a declined
// prompt an observable ERROR_CANCELLED instead of an installer that fails
// later out of sight. /passive shows progress without questions; /norestart
// because an update must never reboot the machine on its own. The installer's
// process is recorded before this returns, and so before the app quits: until
// it ends, a launch exits with a notice instead of starting the app over the
// files it replaces (UpdateMarker.h).
bool LaunchInstaller(fs::path const& msi, std::string& error) {
  wchar_t sys[MAX_PATH];
  const UINT n = ::GetSystemDirectoryW(sys, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    error = "GetSystemDirectory failed";
    return false;
  }
  const std::wstring msiexec = std::wstring(sys, n) + L"\\msiexec.exe";
  const std::wstring params =
      L"/i \"" + msi.wstring() + L"\" /passive /norestart";
  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS;
  sei.lpVerb = L"runas";
  sei.lpFile = msiexec.c_str();
  sei.lpParameters = params.c_str();
  sei.nShow = SW_SHOWNORMAL;
  if (!::ShellExecuteExW(&sei)) {
    const DWORD code = ::GetLastError();
    error = code == ERROR_CANCELLED
                ? std::string("the elevation prompt was declined")
                : std::format("ShellExecuteEx(msiexec) failed: {}", code);
    return false;
  }
  if (sei.hProcess) {
    RecordUpdateInProgress(sei.hProcess);
    ::CloseHandle(sei.hProcess);
  } else {
    LogWarn("update: the installer's process was not returned; launches during the update "
            "are not refused");
  }
  return true;
}

}  // namespace

// ---- lifecycle ---------------------------------------------------------------

UpdateChecker::~UpdateChecker() { Stop(); }

void UpdateChecker::Start() {
  // Before the thread exists, so no lock is needed; the worker takes the
  // value from the member under the lock like every later reader.
  autoCheck_ = AutoCheckEnabled();
  if (version::kCode == 0) {
    LogInfo(
        "update: dev build (code 0) — automatic checking disabled; the "
        "developer screen's manual check still runs and reports");
  }
  worker_ = std::thread([this] { WorkerLoop(); });
}

void UpdateChecker::Stop() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

UpdateChecker::Snapshot UpdateChecker::Current() {
  std::lock_guard lock(mutex_);
  return snapshot_;
}

void UpdateChecker::SetHandler(Handler h) {
  std::lock_guard lock(handlerMutex_);
  handler_ = std::move(h);
}

void UpdateChecker::SetInstallerStartedHandler(InstallerStartedHandler h) {
  std::lock_guard lock(handlerMutex_);
  installerStarted_ = std::move(h);
}

UpdateChecker::Handler UpdateChecker::HandlerCopy() {
  std::lock_guard lock(handlerMutex_);
  return handler_;
}

UpdateChecker::InstallerStartedHandler UpdateChecker::InstallerStartedCopy() {
  std::lock_guard lock(handlerMutex_);
  return installerStarted_;
}

void UpdateChecker::CheckNow() {
  {
    std::lock_guard lock(mutex_);
    checkRequested_ = true;
  }
  cv_.notify_all();
}

void UpdateChecker::BeginApply() {
  {
    std::lock_guard lock(mutex_);
    applyRequested_ = true;
  }
  cv_.notify_all();
}

bool UpdateChecker::AutoCheckEnabled() {
  return LoadAppPrefs().value(kAutoCheckPrefKey, true);
}

void UpdateChecker::SetAutoCheckEnabled(bool on) {
  SaveAppPref(kAutoCheckPrefKey, on);
  {
    std::lock_guard lock(mutex_);
    autoCheck_ = on;
    // The user just asked for updates; answer now, not in six hours.
    if (on) nextAuto_ = steady_clock::now();
  }
  cv_.notify_all();
  LogInfo("update: automatic checking {}", on ? "enabled" : "disabled");
}

void UpdateChecker::RevealInExplorer(std::wstring const& file) {
  const std::wstring args = L"/select,\"" + file + L"\"";
  ::ShellExecuteW(nullptr, nullptr, L"explorer.exe", args.c_str(), nullptr,
                  SW_SHOWNORMAL);
}

void UpdateChecker::Mutate(std::function<void(Snapshot&)> const& fn) {
  Snapshot copy;
  {
    std::lock_guard lock(mutex_);
    fn(snapshot_);
    copy = snapshot_;
  }
  if (auto handler = HandlerCopy()) handler(copy);
}

// ---- the worker --------------------------------------------------------------

void UpdateChecker::WorkerLoop() {
  // MTA for this thread: RevealInExplorer's ShellExecuteW wants COM up when it
  // runs from an apply on this thread.
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  // Every dispatch below is wrapped: an exception escaping a std::thread is
  // std::terminate, so a single surprise (nlohmann type_error on API-shape
  // drift, bad_alloc mid-download) would otherwise take down the whole tray
  // app — and recur on the next 6-hour check. The reads are guarded
  // individually too (ReleaseJson.h); this is the backstop, not the plan.
  try {
    CleanupStaleFiles();
  } catch (std::exception const& e) {
    LogError("update: startup cleanup threw: {}", e.what());
  } catch (...) {
    LogError("update: startup cleanup threw (unknown)");
  }

  std::unique_lock lock(mutex_);
  nextAuto_ = steady_clock::now() + kLaunchDelay;
  for (;;) {
    if (stop_) break;
    if (applyRequested_) {
      applyRequested_ = false;
      lock.unlock();
      try {
        RunApply();
      } catch (std::exception const& e) {
        LogError("update: apply threw: {}", e.what());
        Mutate([](Snapshot& s) {
          s.phase = Phase::Failed;
          s.stage = Stage::Idle;
          s.failure = Failure::Download;
        });
      } catch (...) {
        LogError("update: apply threw (unknown)");
        Mutate([](Snapshot& s) {
          s.phase = Phase::Failed;
          s.stage = Stage::Idle;
          s.failure = Failure::Download;
        });
      }
      lock.lock();
      continue;
    }
    // A dev build never schedules its own checks; only CheckNow lands here.
    const bool timed = autoCheck_ && version::kCode != 0;
    if (checkRequested_ || (timed && steady_clock::now() >= nextAuto_)) {
      checkRequested_ = false;
      lock.unlock();
      try {
        RunCheck();
      } catch (std::exception const& e) {
        LogError("update: check threw: {}", e.what());
        Mutate([](Snapshot& s) { s.lastCheck = CheckOutcome::Failed; });
      } catch (...) {
        LogError("update: check threw (unknown)");
        Mutate([](Snapshot& s) { s.lastCheck = CheckOutcome::Failed; });
      }
      lock.lock();
      // Any completed check — manual or automatic — restarts the cadence; two
      // checks 30 seconds apart cannot say different things.
      nextAuto_ = steady_clock::now() + kCheckInterval;
      continue;
    }
    if (timed)
      cv_.wait_until(lock, nextAuto_);
    else
      cv_.wait(lock);
  }
  lock.unlock();
  winrt::uninit_apartment();
}

void UpdateChecker::CleanupStaleFiles() {
  // The .old files next to the exe are images an earlier portable build's
  // rename-swap updater left behind. Best-effort on purpose: whatever is still
  // held (or not deletable, as under Program Files) stays for a later launch.
  std::error_code ec;
  const fs::path exe = OwnExePath();
  if (!exe.empty()) {
    int removed = 0;
    for (auto const& entry : fs::directory_iterator(exe.parent_path(), ec)) {
      if (!entry.is_regular_file(ec)) continue;
      const std::wstring name = entry.path().filename().wstring();
      // "<stem>.old" or "<stem>.old-<digits>" at the END and nothing else
      // (UpdateFormats.h, selftest-covered): this gate is a DeleteFile in a
      // folder the user unzipped themselves, so `report.old-2024.xlsx` and
      // `URnetwork.exe.old-backup` must never match.
      if (!update::IsStaleRenamedName(Narrow(name))) continue;
      if (::DeleteFileW(entry.path().c_str())) ++removed;
    }
    if (removed) LogInfo("update: removed {} stale .old file(s)", removed);
  }

  // Download dirs whose tag no longer outranks this build are spent — either
  // this very update applied, or a newer one superseded it. A dev build
  // (kCode 0) removes nothing: every tag outranks it by definition.
  for (auto const& entry : fs::directory_iterator(UpdatesDir(), ec)) {
    if (!entry.is_directory(ec)) continue;
    const std::uint64_t code =
        version::ParseReleaseCode(Narrow(entry.path().filename().wstring()));
    if (code != 0 && code <= version::kCode) {
      std::error_code rmec;
      fs::remove_all(entry.path(), rmec);
      if (!rmec)
        LogInfo("update: removed spent download dir {}",
                Narrow(entry.path().filename().wstring()));
    }
  }
}

// ---- the check ---------------------------------------------------------------

void UpdateChecker::RunCheck() {
  Mutate([](Snapshot& s) { s.lastCheck = CheckOutcome::InFlight; });

  // The repository by its id, so no rename and no re-registered owner name
  // can move the feed, and with redirects refused for the same reason.
  const update::Feed& feed = update::kOfficialFeed;
  const std::wstring url = std::format(
      L"https://api.github.com/repositories/{}/releases?per_page=15", feed.numericRepoId);
  std::string body;
  std::string error;
  FetchHeaders headers;
  const bool fetched = FetchUrl(
      url, L"application/vnd.github+json", kMaxJsonBytes, /*followRedirects=*/false,
      [&body](const char* data, DWORD n) {
        body.append(data, n);
        return true;
      },
      [this] {
        std::lock_guard lock(mutex_);
        return stop_;
      },
      headers, error);
  if (!fetched) {
    LogWarn("update: release check failed: {}", error);
    Mutate([](Snapshot& s) { s.lastCheck = CheckOutcome::Failed; });
    return;
  }

  // The JSON is read into plain structs (ReleaseJson.h, the reader the update
  // helper uses too) and the decision made by the pure SelectRelease
  // (ReleaseSelection.h), which the tools test runs against the names the
  // release pipeline actually publishes. Two maxima, deliberately separate:
  // the newest release of this product (the honest answer to "is there
  // something newer") and the newest release this build can actually verify
  // (carrying a usable sha256 digest). When they differ, that is a broken
  // release and the log says so.
  const std::optional<std::vector<update::Release>> parsed = update::ParseReleaseList(body);
  if (!parsed) {
    LogWarn("update: release list was not a JSON array");
    Mutate([](Snapshot& s) { s.lastCheck = CheckOutcome::Failed; });
    return;
  }
  // Codes are judged against GitHub's clock, not this machine's. A response
  // without a Date header is judged against this machine's: the offer is only
  // what the banner shows, and the helper refuses a list without one.
  std::int64_t serverUnixSeconds = headers.serverUnixSeconds;
  if (serverUnixSeconds == 0) {
    LogWarn("update: the release list had no Date header; judging codes by this clock");
    serverUnixSeconds = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
  }
  const update::Selection sel = update::SelectRelease(*parsed, kArch, feed, serverUnixSeconds);
  for (auto const& skip : sel.skipped)
    LogWarn("update: release {} {} — skipped", skip.tag, skip.reason);
  const std::uint64_t newestCode = sel.newestCode;
  const std::string& newestVersion = sel.newestVersion;
  Offer offer;
  if (sel.code != 0 && !update::IsFeedAssetUrl(feed, sel.tag, sel.assetName, sel.assetUrl)) {
    // the helper would refuse it, so it is not offered
    LogWarn("update: release {} names a download outside its feed ({}) — skipped", sel.tag,
            sel.assetUrl);
  } else if (sel.code != 0) {
    LogDebug("update: release {} digest ok ({})", sel.tag, sel.digestHex);
    offer = Offer{Widen(sel.version), sel.code,  Widen(sel.tag),
                  Widen(sel.assetUrl), sel.digestHex, sel.assetName};
  }

  LogInfo("update: check complete — own code {}, newest release {} (code {})",
          static_cast<unsigned long long>(version::kCode),
          newestVersion.empty() ? "none" : newestVersion,
          static_cast<unsigned long long>(newestCode));

  Snapshot copy;
  {
    std::lock_guard lock(mutex_);
    snapshot_.newestCode = newestCode;
    snapshot_.newestVersion = Widen(newestVersion);
    if (version::kCode == 0) {
      // Dev builds never self-update (spec §1) — report, offer nothing.
      snapshot_.lastCheck =
          newestCode ? CheckOutcome::DevBuild : CheckOutcome::NoUpdate;
    } else if (offer.code > version::kCode) {
      offer_ = offer;
      snapshot_.lastCheck = CheckOutcome::UpdateFound;
      // A different (newer) release replaces whatever the banner said about
      // an older one; the SAME release keeps its standing ManualInstall/Failed
      // state — a periodic check must not wipe the outcome of a click.
      if (snapshot_.phase == Phase::None || snapshot_.code != offer.code) {
        snapshot_.phase = Phase::Available;
        snapshot_.stage = Stage::Idle;
        snapshot_.failure = Failure::None;
        snapshot_.version = offer.version;
        snapshot_.code = offer.code;
        snapshot_.installerPath.clear();
      }
    } else {
      snapshot_.lastCheck = CheckOutcome::NoUpdate;
      // A banner for a release that stopped outranking us (it was deleted, or
      // this build updated by hand) closes; a click outcome for it is moot.
      if (snapshot_.phase != Phase::None) {
        snapshot_ = Snapshot{.lastCheck = CheckOutcome::NoUpdate,
                             .newestVersion = Widen(newestVersion),
                             .newestCode = newestCode};
        offer_ = Offer{};
      }
    }
    copy = snapshot_;
  }
  if (auto handler = HandlerCopy()) handler(copy);
}

// ---- the apply ---------------------------------------------------------------

void UpdateChecker::RunApply() {
  Offer offer;
  {
    std::lock_guard lock(mutex_);
    const bool actionable = snapshot_.phase == Phase::Available ||
                            snapshot_.phase == Phase::Failed ||
                            snapshot_.phase == Phase::ManualInstall;
    if (!actionable || offer_.code == 0) return;
    offer = offer_;
  }
  Mutate([&offer](Snapshot& s) {
    s.phase = Phase::Applying;
    s.stage = Stage::Downloading;
    s.failure = Failure::None;
    s.version = offer.version;
    s.code = offer.code;
    s.installerPath.clear();
  });
  const auto fail = [this](Failure f) {
    Mutate([f](Snapshot& s) {
      s.phase = Phase::Failed;
      s.stage = Stage::Idle;
      s.failure = f;
    });
  };
  const auto cancelled = [this] {
    std::lock_guard lock(mutex_);
    return stop_;
  };
  LogInfo("update: applying v{} (code {})", Narrow(offer.version),
          static_cast<unsigned long long>(offer.code));

  // ---- (a) download the own-arch MSI ----------------------------------------
  // A fresh per-tag directory per attempt: nothing from a previous failed try
  // can leak into this one, and a completed try owns everything it verified.
  std::error_code ec;
  const fs::path dir = UpdatesDir() / offer.tag;
  fs::remove_all(dir, ec);
  ec.clear();
  fs::create_directories(dir, ec);
  if (ec) {
    LogError("update: could not create {}: {}", Narrow(dir.wstring()),
             ec.message());
    fail(Failure::Download);
    return;
  }
  const fs::path msiPath = dir / Widen(offer.msiName);
  {
    std::ofstream out(msiPath, std::ios::binary | std::ios::trunc);
    if (!out) {
      LogError("update: could not open {} for writing", offer.msiName);
      fail(Failure::Download);
      return;
    }
    std::string error;
    FetchHeaders headers;
    const bool ok = FetchUrl(
        offer.msiUrl, nullptr, kMaxMsiBytes, /*followRedirects=*/true,
        [&out](const char* data, DWORD n) {
          out.write(data, n);
          return out.good();
        },
        cancelled, headers, error);
    out.close();
    if (!ok || !out.good()) {
      LogWarn("update: download failed: {}", ok ? "file write failed" : error);
      fail(Failure::Download);
      return;
    }
  }

  // ---- (b) verify against the asset's digest ---------------------------------
  // The expected hash travelled inside the Offer since check time — GitHub's
  // per-asset SHA-256 from the very JSON object whose URL was just downloaded
  // — so verification is purely local: hash the file, compare. Folded, not
  // bytewise, because hex case is not worth a failure mode; both sides are
  // minted lowercase today.
  Mutate([](Snapshot& s) { s.stage = Stage::Verifying; });
  const std::string actual = Sha256File(msiPath);
  if (offer.digestHex.empty() || actual.empty() ||
      !update::EqualsAsciiCaseless(offer.digestHex, actual)) {
    // The unverifiable download does not stay on disk: a later "run it
    // yourself" must never be able to reach for an MSI that failed its check.
    LogError("update: checksum mismatch for {} — expected '{}', got '{}'",
             offer.msiName, offer.digestHex, actual);
    fs::remove(msiPath, ec);
    fail(Failure::Checksum);
    return;
  }
  LogInfo("update: verified {} ({})", offer.msiName, actual);

  // ---- (c) start the installer, then get out of its way ---------------------
  Mutate([](Snapshot& s) { s.stage = Stage::Installing; });
  std::string launchError;
  if (!LaunchInstaller(msiPath, launchError)) {
    // Verified and on disk; hand the finish to the user and SHOW them the
    // file rather than describing where it is.
    LogWarn("update: installer not started ({}) — downloaded, not installed",
            launchError);
    const std::wstring msiW = msiPath.wstring();
    Mutate([&msiW](Snapshot& s) {
      s.phase = Phase::ManualInstall;
      s.stage = Stage::Idle;
      s.installerPath = msiW;
    });
    RevealInExplorer(msiW);
    return;
  }
  LogInfo("update: installer started for v{}; quitting so it can replace the "
          "app", Narrow(offer.version));
  Mutate([](Snapshot& s) {
    s = Snapshot{.lastCheck = s.lastCheck,
                 .newestVersion = s.newestVersion,
                 .newestCode = s.newestCode};
  });
  if (auto started = InstallerStartedCopy()) {
    started();
  } else {
    LogWarn("update: no installer handler bound — quit the app so the "
            "installer can replace it");
  }
}

}  // namespace urnw
