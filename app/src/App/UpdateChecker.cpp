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

#include "InstallLocationWin32.h"
#include "Localization.h"
#include "Log.h"
#include "Paths.h"
#include "ReleaseJson.h"
#include "ReleaseSelection.h"
#include "SingleInstance.h"
#include "Strings.h"
#include "UpdateApply.h"
#include "UpdateChannel.h"
#include "UpdateFormats.h"
#include "UpdateResult.h"
#include "UpdateResultJson.h"
#include "UpdateSchedule.h"
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
// The finishedUtc of the helper's report the user dismissed last: a report is
// shown until then.
constexpr char kResultSeenPrefKey[] = "update_result_seen";
// When a check last succeeded (Unix seconds), or when this install first
// tried: what "Couldn't check for updates since <date>" names.
constexpr char kLastSuccessPrefKey[] = "update_last_check_success";

std::int64_t NowUnixSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// The update helper, beside this exe in an installed copy (app/src/Updater).
constexpr wchar_t kHelperName[] = L"URnetworkUpdate.exe";

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
  // What a refused request said about asking again (UpdateSchedule.h).
  std::int64_t retryAfterSeconds = 0;
  std::int64_t rateLimitResetUnixSeconds = 0;
  bool rateLimitExhausted = false;
};

// A response header by name as an integer, or `fallback` when it is absent or
// is not one.
std::int64_t NumericHeader(HINTERNET request, const wchar_t* name, std::int64_t fallback) {
  wchar_t value[32] = {};
  DWORD size = sizeof(value);
  if (!::WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, value, &size,
                             WINHTTP_NO_HEADER_INDEX)) {
    return fallback;
  }
  std::int64_t number = 0;
  std::size_t digits = 0;
  for (const wchar_t* c = value; *c; ++c, ++digits) {
    if (*c < L'0' || *c > L'9' || digits >= 18) return fallback;
    number = number * 10 + (*c - L'0');
  }
  return digits ? number : fallback;
}

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
    // GitHub says when to ask again: Retry-After for a secondary limit, the
    // reset time once the hour's requests are spent.
    headers.retryAfterSeconds = NumericHeader(request.h, L"Retry-After", 0);
    headers.rateLimitResetUnixSeconds = NumericHeader(request.h, L"X-RateLimit-Reset", 0);
    headers.rateLimitExhausted = NumericHeader(request.h, L"X-RateLimit-Remaining", -1) == 0;
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

// ---- the update helper -------------------------------------------------------

// What the elevation answers when Windows' policy "User Account Control: Only
// elevate executables that are signed and validated" is on and the program is
// not signed: "A referral was returned from the server".
constexpr DWORD kElevationRefusedUnsigned = ERROR_DS_REFERRAL;
static_assert(update::kStartCancelled == ERROR_CANCELLED &&
              update::kStartUnsigned == kElevationRefusedUnsigned);

// Starts the update helper for `tag`: the installed, admin-owned
// URnetworkUpdate.exe beside this exe, elevated up front with the "runas"
// verb, so a declined prompt is an observable ERROR_CANCELLED instead of a
// helper that fails out of sight. The tag comes from the release grammar, so
// it holds no quote or space. Nothing else is passed: the helper fetches the
// release list, the package and its digest itself, and builds msiexec's
// arguments itself. `*helper` is its process, for the caller to wait on,
// record in the update marker should this app exit first, and close; null
// when the shell returned none. `*refusal` is the Win32 error of a start that
// failed.
bool LaunchUpdateHelper(fs::path const& helperPath, std::wstring const& tag, HANDLE* helper,
                        DWORD* refusal, std::string& error) {
  *helper = nullptr;
  *refusal = ERROR_SUCCESS;
  const std::wstring params = L"--apply-update " + tag;
  const std::wstring folder = helperPath.parent_path().wstring();
  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS;
  sei.lpVerb = L"runas";
  sei.lpFile = helperPath.c_str();
  sei.lpParameters = params.c_str();
  sei.lpDirectory = folder.c_str();
  sei.nShow = SW_HIDE;
  if (!::ShellExecuteExW(&sei)) {
    *refusal = ::GetLastError();
    error = *refusal == ERROR_CANCELLED ? std::string("the elevation prompt was declined")
            : *refusal == kElevationRefusedUnsigned
                ? std::string("Windows elevates only signed programs here, and the helper is not signed")
                : std::format("ShellExecuteEx(update helper) failed: {}", *refusal);
    return false;
  }
  if (sei.hProcess) {
    *helper = sei.hProcess;
  } else {
    LogWarn("update: the update helper's process was not returned; this app cannot wait on it, "
            "and launches during the update are not refused");
  }
  return true;
}

// Starts the update helper for a change of the machine's update channel to
// `feed`, a row of the table: the installed URnetworkUpdate.exe beside this
// exe with --set-update-channel and the row's id, elevated with "runas" as
// for an update, and shown, since the developer channel asks first. Nothing
// else is passed: the helper reads and writes the channel's marker itself.
// `*helper` is its process, for the caller to wait on and close; null when
// the shell returned none. `*refusal` is the Win32 error of a start that
// failed.
bool LaunchChannelHelper(fs::path const& helperPath, update::Feed const& feed, HANDLE* helper,
                         DWORD* refusal, std::string& error) {
  *helper = nullptr;
  *refusal = ERROR_SUCCESS;
  const std::wstring params = L"--set-update-channel " + Widen(feed.id);
  const std::wstring folder = helperPath.parent_path().wstring();
  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS;
  sei.lpVerb = L"runas";
  sei.lpFile = helperPath.c_str();
  sei.lpParameters = params.c_str();
  sei.lpDirectory = folder.c_str();
  sei.nShow = SW_SHOWNORMAL;
  if (!::ShellExecuteExW(&sei)) {
    *refusal = ::GetLastError();
    error = std::format("ShellExecuteEx(update helper, channel) failed: {}", *refusal);
    return false;
  }
  if (sei.hProcess) {
    *helper = sei.hProcess;
  } else {
    LogWarn("update: the update helper's process was not returned; the channel is read again later");
  }
  return true;
}

// When Windows last started, in Unix seconds.
std::int64_t BootUnixSeconds() {
  return NowUnixSeconds() - static_cast<std::int64_t>(::GetTickCount64() / 1000);
}

// The report a last-result.json holds, read whole (it is a few hundred bytes),
// or nullopt.
std::optional<update::UpdateResult> ReadResult(fs::path const& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return std::nullopt;
  std::string text(4096, ' ');
  in.read(text.data(), static_cast<std::streamsize>(text.size()));
  text.resize(static_cast<std::size_t>(in.gcount()));
  return update::ParseUpdateResult(text);
}

// The log a report points at: msiexec's for an install that ran, the
// helper's own for a refusal.
std::wstring ResultLogPath(fs::path const& installFolder, update::UpdateResult const& result) {
  const fs::path folder = installFolder / L"updates" / Widen(result.tag);
  return (folder / (update::OutcomeOf(result.exitCode) == update::Outcome::Refused
                        ? L"update-helper.log"
                        : L"install.log"))
      .wstring();
}

// The release's v-less version, from its tag.
std::wstring VersionOfTag(std::string tag) {
  if (!tag.empty() && tag.front() == 'v') tag.erase(0, 1);
  return Widen(tag);
}

// A checked download that is still on disk: `msi` is there and its SHA-256
// is `digestHex`. A failed update keeps the tray's copy, so its installer can
// be shown without downloading it again.
bool IsCheckedDownload(fs::path const& msi, std::string const& digestHex) {
  std::error_code ec;
  if (digestHex.empty() || !fs::is_regular_file(msi, ec)) return false;
  const std::string actual = Sha256File(msi);
  return !actual.empty() && update::EqualsAsciiCaseless(actual, digestHex);
}

// The dismissed report's finishedUtc, or empty. Type-checked: a value of the
// wrong type in the preferences reads as none.
std::string SeenResult() {
  const nlohmann::json prefs = LoadAppPrefs();
  const auto it = prefs.find(kResultSeenPrefKey);
  return it != prefs.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

}  // namespace

// ---- lifecycle ---------------------------------------------------------------

UpdateChecker::~UpdateChecker() { Stop(); }

void UpdateChecker::Start() {
  // Before the thread exists, so no lock is needed; the worker takes the
  // value from the member under the lock like every later reader.
  autoCheck_ = AutoCheckEnabled();
  // An installed copy runs the elevated helper beside it; anything else (a
  // portable zip, a dev build, a folder a user can write) is offered the
  // installer to run, and never elevates anything.
  const fs::path exe = install::OwnExecutablePath();
  installFolder_ = exe.parent_path();
  std::string why;
  installed_ = !exe.empty() && install::AdminOnlyLocation(installFolder_ / kHelperName, why);
  if (!installed_) {
    LogInfo("update: {} is not an admin-only install ({}); an update is offered as the "
            "installer to run",
            Narrow(installFolder_.wstring()), why);
  }
  snapshot_.installed = installed_;
  // The machine's update channel, read before the worker exists: an installed
  // copy with a code follows the feed its marker selects, and any other copy
  // the official feed, reading no marker (UpdateChannel.h).
  if (installed_ && version::kCode != 0) {
    std::string channelWhy;
    const update::ChannelMarker marker = install::ReadChannelMarker(installFolder_, channelWhy);
    feed_ = &update::FeedOfMarker(marker);
    snapshot_.marker = marker;
    if (marker == update::ChannelMarker::Rejected) {
      LogWarn("update: the update channel's marker is not trusted ({}); official releases are followed",
              channelWhy);
    }
  }
  snapshot_.feed = feed_;
  LogInfo("update: this copy follows the {} feed", feed_->id);
  // When a check last succeeded; before any has, this first launch is when
  // checks began, so 72 hours of failures from now are reported too.
  const nlohmann::json prefs = LoadAppPrefs();
  const auto lastSuccess = prefs.find(kLastSuccessPrefKey);
  snapshot_.lastSuccessUnix = lastSuccess != prefs.end() && lastSuccess->is_number_integer()
                                  ? lastSuccess->get<std::int64_t>()
                                  : 0;
  if (snapshot_.lastSuccessUnix <= 0) {
    snapshot_.lastSuccessUnix = NowUnixSeconds();
    SaveAppPref(kLastSuccessPrefKey, snapshot_.lastSuccessUnix);
  }
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

UpdateChecker::Handler UpdateChecker::HandlerCopy() {
  std::lock_guard lock(handlerMutex_);
  return handler_;
}

void UpdateChecker::CheckNow() {
  {
    std::lock_guard lock(mutex_);
    checkRequested_ = true;
    // The user asked what there is, so what this check finds is shown, a
    // release Later hid included. The timed checks leave Later standing.
    laterCode_ = 0;
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

void UpdateChecker::ShowInstaller() {
  {
    std::lock_guard lock(mutex_);
    manualRequested_ = true;
  }
  cv_.notify_all();
}

void UpdateChecker::RevealInstaller() {
  {
    std::lock_guard lock(mutex_);
    revealRequested_ = true;
  }
  cv_.notify_all();
}

void UpdateChecker::DismissResult() {
  std::wstring finished;
  {
    std::lock_guard lock(mutex_);
    if (snapshot_.phase != Phase::Result) return;
    finished = snapshot_.result.finishedUtc;
  }
  // A report the helper never wrote (an early refusal) has no time to
  // remember, and nothing at a later launch shows it again.
  if (!finished.empty()) SaveAppPref(kResultSeenPrefKey, Narrow(finished));
  // a release still offered (one whose update did not install) is offered
  // again, unless Later hid it for this run
  Mutate([this](Snapshot& s) {
    if (s.phase != Phase::Result) return;
    s.result = Result{};
    if (offer_.code > version::kCode && !update::HiddenByLater(laterCode_, offer_.code)) {
      s.phase = Phase::Available;
      s.stage = Stage::Idle;
      s.failure = Failure::None;
      s.version = offer_.version;
      s.code = offer_.code;
    } else {
      s.phase = Phase::None;
      s.version.clear();
      s.code = 0;
    }
  });
}

void UpdateChecker::Later() {
  std::wstring version;
  Mutate([this, &version](Snapshot& s) {
    if (!OffersLater(s)) return;
    // In this member and nowhere else: the next launch starts without it and
    // offers the release again.
    laterCode_ = s.code;
    version = s.version;
    s.phase = Phase::None;
    s.stage = Stage::Idle;
    s.failure = Failure::None;
    s.version.clear();
    s.code = 0;
    s.installerPath.clear();
    s.replacedVersion.clear();
    s.replacedByCode = 0;
  });
  if (!version.empty()) {
    LogInfo("update: Later: v{} is not shown again until the next launch, or a check the user "
            "asks for",
            Narrow(version));
  }
}

void UpdateChecker::ChannelChanged() {
  Snapshot copy;
  {
    std::lock_guard lock(mutex_);
    ++feedGeneration_;
    offer_ = Offer{};
    // Later was about a release of the feed the user has left
    laterCode_ = 0;
    // The report of an update the helper ran is true whatever the feed, and
    // stays until the user dismisses it; every other banner was about the
    // offer.
    if (snapshot_.phase != Phase::Result) {
      snapshot_ = Snapshot{.installed = installed_,
                           .lastSuccessUnix = snapshot_.lastSuccessUnix,
                           .checkStale = snapshot_.checkStale,
                           .holdUntilUnix = snapshot_.holdUntilUnix,
                           .feed = feed_,
                           .marker = snapshot_.marker};
    }
    // a report's banner keeps its snapshot, and follows the new feed too;
    // a change seen is the end of any line about the last one asked for
    snapshot_.feed = feed_;
    snapshot_.change = update::ChangeLine::None;
    snapshot_.changeCode.clear();
    snapshot_.offeredCode = 0;
    checkRequested_ = true;
    copy = snapshot_;
  }
  cv_.notify_all();
  LogInfo("update: the update channel changed; work for the previous feed is abandoned");
  if (auto handler = HandlerCopy()) handler(copy);
}

void UpdateChecker::RequestChannel(const update::Feed& feed) {
  Snapshot copy;
  {
    std::lock_guard lock(mutex_);
    const bool ownUpdate = snapshot_.phase == Phase::Applying || applyRequested_ || manualRequested_;
    if (ownUpdate) {
      // refused now: queued, Windows' prompt would come minutes later
      const update::ChannelChange refused{.asked = &feed,
                                          .markerAfter = snapshot_.marker,
                                          .ownUpdateRunning = true};
      snapshot_.change = update::ChangeLineOf(refused);
    } else {
      channelRequested_ = &feed;
      snapshot_.change = update::ChangeLine::Asking;
    }
    snapshot_.changeCode.clear();
    copy = snapshot_;
  }
  cv_.notify_all();
  LogInfo("update: the update channel {} asked for: {}", copy.change == update::ChangeLine::Asking
                                                              ? "is"
                                                              : "was not",
          feed.id);
  if (auto handler = HandlerCopy()) handler(copy);
}

void UpdateChecker::RequestChannelSync() {
  {
    std::lock_guard lock(mutex_);
    syncRequested_ = true;
  }
  cv_.notify_all();
}

std::wstring UpdateChecker::LocalDate(std::int64_t unixSeconds) {
  ULARGE_INTEGER ticks{};
  ticks.QuadPart = static_cast<ULONGLONG>(unixSeconds + 11644473600LL) * 10000000ULL;
  const FILETIME utcFile{.dwLowDateTime = ticks.LowPart, .dwHighDateTime = ticks.HighPart};
  SYSTEMTIME utc{};
  SYSTEMTIME local{};
  wchar_t date[80] = {};
  if (!::FileTimeToSystemTime(&utcFile, &utc) ||
      !::SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local) ||
      !::GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, date, 80,
                         nullptr)) {
    return Widen(update::FormatUtcSecond(unixSeconds).substr(0, 10));
  }
  return date;
}

std::wstring UpdateChecker::LocalDateTime(std::int64_t unixSeconds) {
  ULARGE_INTEGER ticks{};
  ticks.QuadPart = static_cast<ULONGLONG>(unixSeconds + 11644473600LL) * 10000000ULL;
  const FILETIME utcFile{.dwLowDateTime = ticks.LowPart, .dwHighDateTime = ticks.HighPart};
  SYSTEMTIME utc{};
  SYSTEMTIME local{};
  wchar_t time[80] = {};
  if (!::FileTimeToSystemTime(&utcFile, &utc) ||
      !::SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local) ||
      !::GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, nullptr, time, 80)) {
    return Widen(update::FormatUtcSecond(unixSeconds));
  }
  return LocalDate(unixSeconds) + L" " + time;
}

void UpdateChecker::SetVpnConnected(bool connected) {
  std::lock_guard lock(vpnMutex_);
  vpn_.StatusPushed(connected);
}

void UpdateChecker::SetVpnAnswerProbe(std::function<std::optional<bool>()> probe) {
  vpnAnswer_ = std::move(probe);
}

update::VpnNote UpdateChecker::HandOffVpn() {
  // asked before the lock: the probe reads the sdk host's own state
  const std::optional<bool> answered = vpnAnswer_ ? vpnAnswer_() : std::nullopt;
  std::lock_guard lock(vpnMutex_);
  vpn_.HandOff(answered);
  return vpn_;
}

update::VpnAtStart UpdateChecker::NoteVpn() {
  std::lock_guard lock(vpnMutex_);
  return vpn_.Note();
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
  // "The app keeps trying" is said only while it does.
  const std::int64_t now = NowUnixSeconds();
  Mutate([now, on](Snapshot& s) {
    s.checkStale = update::CheckIsStale(now, s.lastSuccessUnix, on && version::kCode != 0);
  });
  cv_.notify_all();
  LogInfo("update: automatic checking {}", on ? "enabled" : "disabled");
}

bool UpdateChecker::OffersInstaller(Snapshot const& snapshot) {
  // a developer build installs only through the helper's own checks, so a
  // release the helper refused is never offered again as a file
  return snapshot.phase == Phase::Result &&
         snapshot.result.view == update::ReportView::NotInstalled && snapshot.offeredCode != 0 &&
         snapshot.offeredCode == snapshot.code && update::HandsOverInstaller(*snapshot.feed);
}

bool UpdateChecker::OffersLater(Snapshot const& snapshot) {
  // Not while an apply runs, and not on the helper's report, which has its
  // own dismissal.
  return snapshot.code != 0 &&
         (snapshot.phase == Phase::Available || snapshot.phase == Phase::Failed ||
          snapshot.phase == Phase::ManualInstall);
}

bool UpdateChecker::SaysReplaced(Snapshot const& snapshot) {
  return snapshot.phase == Phase::Available && snapshot.code != 0 &&
         snapshot.replacedByCode == snapshot.code && !snapshot.replacedVersion.empty();
}

bool UpdateChecker::NamesWaiting(Snapshot const& snapshot) {
  return snapshot.waitingCode != 0 && (snapshot.lastCheck == CheckOutcome::NoUpdate ||
                                       snapshot.lastCheck == CheckOutcome::UpdateFound);
}

update::ChannelLines UpdateChecker::ChannelLinesOf(Snapshot const& snapshot) {
  return update::LinesOf(
      {.installed = snapshot.installed,
       .codeZero = version::kCode == 0,
       .developerBuild = update::IsBetaVersion(version::kString),
       .marker = snapshot.marker,
       .developerRow = update::FeedById(update::kDeveloperChannelId) != nullptr,
       .change = snapshot.change,
       .foundNothingNewer = snapshot.lastCheck == CheckOutcome::NoUpdate,
       .releaseCounts = snapshot.newestCode != 0,
       .releaseWaiting = NamesWaiting(snapshot)});
}

std::wstring UpdateChecker::RepositoryOf(update::Feed const& feed) {
  return Widen(feed.owner) + L"/" + Widen(feed.repo);
}

std::wstring UpdateChecker::ChannelSentenceText(Snapshot const& snapshot,
                                                update::ChannelLines const& lines) {
  switch (lines.channel) {
    case update::ChannelSentence::DevBuild:
      return Localized("upd_channel_dev_build");
    case update::ChannelSentence::NotInstalled:
      return Localized("upd_channel_not_installed");
    case update::ChannelSentence::Untrusted:
      return Localized("upd_channel_untrusted");
    case update::ChannelSentence::Developer:
      // on the developer feed the snapshot's feed is its row
      return Format("upd_channel_developer", RepositoryOf(*snapshot.feed));
    case update::ChannelSentence::Official:
      break;
  }
  return Format("upd_channel_official", RepositoryOf(update::kOfficialFeed));
}

std::wstring UpdateChecker::BuildSentenceText(update::ChannelLines const& lines) {
  return lines.buildSentence ? Localized("upd_channel_developer_build") : std::wstring{};
}

std::wstring UpdateChecker::ChangeSentenceText(Snapshot const& snapshot,
                                               update::ChannelLines const& lines) {
  switch (lines.change) {
    case update::ChangeLine::Asking:
      return Localized("upd_channel_asking");
    case update::ChangeLine::Unchanged:
      return Localized("upd_channel_unchanged");
    case update::ChangeLine::Unsigned:
      return Localized("upd_channel_unsigned");
    case update::ChangeLine::Busy:
      return Localized("upd_channel_busy");
    case update::ChangeLine::Failed:
      return Format("upd_channel_failed", snapshot.changeCode);
    case update::ChangeLine::None:
      break;
  }
  return {};
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

bool UpdateChecker::MutateFor(std::uint64_t generation,
                              std::function<void(Snapshot&)> const& fn) {
  Snapshot copy;
  {
    std::lock_guard lock(mutex_);
    if (feedGeneration_ != generation) return false;
    fn(snapshot_);
    copy = snapshot_;
  }
  if (auto handler = HandlerCopy()) handler(copy);
  return true;
}

void UpdateChecker::CheckFailed(std::uint64_t generation) {
  const std::int64_t now = NowUnixSeconds();
  MutateFor(generation, [this, now](Snapshot& s) {
    s.lastCheck = CheckOutcome::Failed;
    s.checkStale = update::CheckIsStale(now, s.lastSuccessUnix, autoCheck_ && version::kCode != 0);
  });
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
    ShowLastResult();
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
    if (applyRequested_ || manualRequested_) {
      // the click the user made last wins; both start the same pipeline
      const bool manual = manualRequested_ && !applyRequested_;
      applyRequested_ = false;
      manualRequested_ = false;
      const std::uint64_t generation = feedGeneration_;
      lock.unlock();
      const auto failed = [this, generation] {
        MutateFor(generation, [](Snapshot& s) {
          s.phase = Phase::Failed;
          s.stage = Stage::Idle;
          s.failure = Failure::Download;
        });
      };
      try {
        RunApply(generation, manual);
      } catch (std::exception const& e) {
        LogError("update: apply threw: {}", e.what());
        failed();
      } catch (...) {
        LogError("update: apply threw (unknown)");
        failed();
      }
      lock.lock();
      continue;
    }
    if (revealRequested_) {
      revealRequested_ = false;
      const std::uint64_t generation = feedGeneration_;
      lock.unlock();
      try {
        RunReveal(generation);
      } catch (std::exception const& e) {
        LogError("update: showing the installer threw: {}", e.what());
      } catch (...) {
        LogError("update: showing the installer threw (unknown)");
      }
      lock.lock();
      continue;
    }
    if (channelRequested_) {
      const update::Feed* asked = channelRequested_;
      channelRequested_ = nullptr;
      lock.unlock();
      try {
        RunChannelChange(*asked);
      } catch (std::exception const& e) {
        LogError("update: the change of channel threw: {}", e.what());
      } catch (...) {
        LogError("update: the change of channel threw (unknown)");
      }
      lock.lock();
      continue;
    }
    if (syncRequested_) {
      syncRequested_ = false;
      lock.unlock();
      try {
        SyncChannel();
      } catch (std::exception const& e) {
        LogError("update: reading the update channel threw: {}", e.what());
      } catch (...) {
        LogError("update: reading the update channel threw (unknown)");
      }
      lock.lock();
      continue;
    }
    // A dev build never schedules its own checks; only CheckNow lands here.
    const bool timed = autoCheck_ && version::kCode != 0;
    if (checkRequested_ || (timed && steady_clock::now() >= nextAuto_)) {
      checkRequested_ = false;
      const std::uint64_t generation = feedGeneration_;
      lock.unlock();
      try {
        RunCheck(generation);
      } catch (std::exception const& e) {
        LogError("update: check threw: {}", e.what());
        CheckFailed(generation);
      } catch (...) {
        LogError("update: check threw (unknown)");
        CheckFailed(generation);
      }
      lock.lock();
      // Any completed check — manual or automatic — restarts the cadence; two
      // checks 30 seconds apart cannot say different things. Never before
      // GitHub's own Retry-After or rate-limit reset.
      nextAuto_ = std::max(steady_clock::now() + kCheckInterval, holdUntil_);
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

void UpdateChecker::ShowLastResult() {
  if (!installed_) return;
  const std::optional<update::UpdateResult> result =
      ReadResult(installFolder_ / L"updates" / L"last-result.json");
  if (!result) return;
  if (SeenResult() == result->finishedUtc) return;
  // Shown only while it is still true of this build: Windows has restarted
  // since a 3010, a newer build runs, or an older one does after a success.
  const std::optional<std::int64_t> finished = update::ParseUtcSecond(result->finishedUtc);
  const bool restartedSince = finished && BootUnixSeconds() > *finished;
  const update::ReportView view =
      update::ViewOfReport(*result, version::kCode, /*live=*/false, restartedSince);
  if (view == update::ReportView::Hidden) {
    LogInfo("update: the update helper's last report ({} ended with {} at {}) is not about this "
            "build any more; not shown",
            result->tag, result->exitCode, result->finishedUtc);
    return;
  }
  LogInfo("update: the update helper's last report: {} ended with {} at {}", result->tag,
          result->exitCode, result->finishedUtc);
  // Whether a VPN was connected when that update began: the record of the
  // update this launch found over (SingleInstance.cpp TakeEndedUpdate), when
  // it is of this report's update. It is written as the installer closes the
  // app, before the helper ends, so a report that finished before it was
  // written is another update's, and Unknown, as is a record an older build
  // wrote. The record is taken once, by the first launch after its update: a
  // launch that took none shows the report again, and leaves the line out
  // (UpdateMarker.h VpnOfReport, SaysVpnDisconnected).
  const std::optional<update::UpdateMarker> began = TakeEndedUpdate();
  const std::optional<update::VpnAtStart> vpn = update::VpnOfReport(began, finished);
  LogInfo("update: {}; the report {} the line about the VPN",
          !began ? "this launch took no record of the update that ended"
          : vpn == update::VpnAtStart::Connected
              ? "the record of the update that ended says a VPN was connected when it began"
          : vpn == update::VpnAtStart::NotConnected
              ? "the record of the update that ended says a VPN was not connected when it began"
              : "the record of the update that ended does not say whether a VPN was connected, "
                "or is another update's",
          update::SaysVpnDisconnected(vpn) ? "keeps" : "leaves out");
  const std::wstring version = VersionOfTag(result->tag);
  Mutate([&](Snapshot& s) {
    s.phase = Phase::Result;
    s.stage = Stage::Idle;
    s.failure = Failure::None;
    s.version = version;
    s.code = result->code;
    s.installerPath.clear();
    s.result = Result{.version = version,
                      .exitCode = result->exitCode,
                      .logPath = ResultLogPath(installFolder_, *result),
                      .finishedUtc = Widen(result->finishedUtc),
                      .view = view};
    s.result.vpn = vpn;
  });
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

// ---- the update channel ------------------------------------------------------

void UpdateChecker::SyncChannel() {
  // an installed copy with a code is the only one that has a channel; any
  // other copy follows the official feed and reads nothing (Start)
  if (!installed_ || version::kCode == 0) return;
  std::string why;
  const update::ChannelMarker marker = install::ReadChannelMarker(installFolder_, why);
  const update::Feed& feed = update::FeedOfMarker(marker);
  bool changed = false;
  bool newlyRejected = false;
  Snapshot copy;
  {
    std::lock_guard lock(mutex_);
    newlyRejected = marker == update::ChannelMarker::Rejected && snapshot_.marker != marker;
    snapshot_.marker = marker;
    changed = feed.id != feed_->id;
    if (changed) feed_ = &feed;
    copy = snapshot_;
  }
  if (newlyRejected) {
    LogWarn("update: the update channel's marker is not trusted ({}); official releases are followed", why);
  }
  if (changed) {
    LogInfo("update: the update channel is now the {} feed", feed.id);
    ChannelChanged();
    return;
  }
  if (auto handler = HandlerCopy()) handler(copy);
}

void UpdateChecker::RunChannelChange(const update::Feed& asked) {
  // As for an update, a copy that is not an installed one with a code asks
  // for no elevation: it has no channel to change.
  if (!installed_ || version::kCode == 0) {
    LogWarn("update: this copy has no update channel; the helper is not started");
    Mutate([](Snapshot& s) {
      s.change = update::ChangeLine::None;
      s.changeCode.clear();
    });
    return;
  }
  LogInfo("update: asking the update helper for the {} channel", asked.id);
  HANDLE helper = nullptr;
  DWORD refusal = ERROR_SUCCESS;
  std::string launchError;
  const bool started =
      LaunchChannelHelper(installFolder_ / kHelperName, asked, &helper, &refusal, launchError);
  std::int64_t exitCode = 0;
  if (!started) {
    LogWarn("update: the update helper did not start ({}); the update channel is as it was",
            launchError);
  } else if (!helper) {
    // nothing to wait on: no line is said, and the channel's own line
    // shows what the marker says
    SyncChannel();
    Mutate([](Snapshot& s) {
      s.change = update::ChangeLine::None;
      s.changeCode.clear();
    });
    return;
  } else {
    const auto cancelled = [this] {
      std::lock_guard lock(mutex_);
      return stop_;
    };
    const update::HelperWait waited = update::AwaitHelper(
        [helper] { return ::WaitForSingleObject(helper, 250) == WAIT_OBJECT_0; }, cancelled);
    if (waited == update::HelperWait::AppExiting) {
      // nothing is being installed: no update is recorded, and the helper
      // ends on its own
      ::CloseHandle(helper);
      return;
    }
    DWORD code = 0;
    exitCode = ::GetExitCodeProcess(helper, &code) ? static_cast<std::int64_t>(code)
                                                   : std::int64_t{0xFFFFFFFF};
    ::CloseHandle(helper);
    LogInfo("update: the update helper ended the change of channel with {}", exitCode);
  }
  // Whatever the helper answered, the marker says what happened: read again,
  // it decides the line (ChangeLineOf), and this copy follows it.
  std::string why;
  const update::ChannelChange change{.asked = &asked,
                                     .markerAfter = install::ReadChannelMarker(installFolder_, why),
                                     .startError = started ? 0u : static_cast<std::uint32_t>(refusal),
                                     .exitCode = exitCode};
  const update::ChangeLine line = update::ChangeLineOf(change);
  const std::wstring code =
      line == update::ChangeLine::Failed ? Widen(update::ChangeCodeText(change)) : std::wstring{};
  SyncChannel();
  Mutate([line, &code](Snapshot& s) {
    s.change = line;
    s.changeCode = code;
  });
  LogInfo("update: the change of channel to {} {}", asked.id,
          line == update::ChangeLine::None ? "is what the marker says" : "did not happen");
}

// ---- the check ---------------------------------------------------------------

bool UpdateChecker::RunCheck(std::uint64_t generation) {
  // The machine's channel first: a change since this check was asked for
  // drops it, and ChannelChanged has queued one of the new feed.
  SyncChannel();
  bool held = false;
  const update::Feed* followed = nullptr;
  {
    std::lock_guard lock(mutex_);
    if (feedGeneration_ != generation) return false;
    followed = feed_;
    held = steady_clock::now() < holdUntil_;
  }
  if (held) {
    // GitHub asked for no request before then; a manual check waits too.
    LogWarn("update: GitHub asked for no request yet; the check is not sent");
    CheckFailed(generation);
    return false;
  }
  MutateFor(generation, [](Snapshot& s) { s.lastCheck = CheckOutcome::InFlight; });

  // The repository by its id, so no rename and no re-registered owner name
  // can move the feed, and with redirects refused for the same reason. The
  // same request the update helper makes (ReleaseListUrl), so the two judge
  // the same page of releases.
  const update::Feed& feed = *followed;
  const std::wstring url = Widen(update::ReleaseListUrl(feed));
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
    const update::RateLimit limit{.retryAfterSeconds = headers.retryAfterSeconds,
                                  .resetUnixSeconds = headers.rateLimitResetUnixSeconds,
                                  .exhausted = headers.rateLimitExhausted,
                                  .serverUnixSeconds = headers.serverUnixSeconds};
    const std::int64_t wait = update::NextCheckDelaySeconds(0, limit);
    if (wait > 0) {
      LogWarn("update: GitHub asked for no request for {} s", wait);
      std::lock_guard lock(mutex_);
      holdUntil_ = steady_clock::now() + std::chrono::seconds(wait);
      holdUntilUnix_ = NowUnixSeconds() + wait;
      snapshot_.holdUntilUnix = holdUntilUnix_;
    }
    CheckFailed(generation);
    return false;
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
    CheckFailed(generation);
    return false;
  }
  // Releases are judged against GitHub's clock, never this machine's: how far
  // ahead a code may be, and how long a release has been out. A list without
  // a Date header cannot be judged, and the helper refuses one too, so
  // nothing is offered from it and the check has not succeeded.
  if (headers.serverUnixSeconds == 0) {
    LogWarn("update: the release list had no Date header; nothing is offered from it");
    CheckFailed(generation);
    return false;
  }
  const std::int64_t succeeded = NowUnixSeconds();
  SaveAppPref(kLastSuccessPrefKey, succeeded);
  const std::int64_t serverUnixSeconds = headers.serverUnixSeconds;
  const update::Selection sel = update::SelectRelease(*parsed, kArch, feed, serverUnixSeconds);
  for (auto const& skip : sel.skipped) {
    // A release still inside its day is how this feed looks on most days:
    // nothing is wrong with it, and it is no warning.
    if (skip.soaking) {
      LogInfo("update: release {} {} — skipped", skip.tag, skip.reason);
    } else {
      LogWarn("update: release {} {} — skipped", skip.tag, skip.reason);
    }
  }
  const std::uint64_t newestCode = sel.newestCode;
  const std::string& newestVersion = sel.newestVersion;
  Offer offer;
  if (sel.code != 0 && !update::IsFeedAssetUrl(feed, sel.tag, sel.assetName, sel.assetUrl)) {
    // the helper would refuse it, so it is not offered
    LogWarn("update: release {} names a download outside its feed ({}) — skipped", sel.tag,
            sel.assetUrl);
  } else if (sel.code != 0) {
    LogDebug("update: release {} digest ok ({})", sel.tag, sel.digestHex);
    offer = Offer{Widen(sel.version), sel.code,     Widen(sel.tag),
                  Widen(sel.assetUrl),  sel.digestHex, sel.assetName, &feed};
  }

  LogInfo("update: check of the {} feed complete — own code {}, newest release {} (code {}){}",
          feed.id, static_cast<unsigned long long>(version::kCode),
          newestVersion.empty() ? "none" : newestVersion,
          static_cast<unsigned long long>(newestCode),
          sel.waitingCode == 0
              ? std::string{}
              : std::format("; {} counts from {}", sel.waitingVersion,
                            update::FormatUtcSecond(sel.waitingFromUnixSeconds)));

  Snapshot copy;
  // The report a new offer takes off the banner, by its finishedUtc.
  std::wstring replacedReport;
  // The release whose download the tray app keeps: the one it offers.
  std::wstring keepTag;
  {
    std::lock_guard lock(mutex_);
    // Reaching GitHub is what "Couldn't check for updates" is about, whichever
    // feed was asked (ChannelChanged carries it over too).
    snapshot_.lastSuccessUnix = succeeded;
    snapshot_.checkStale = false;
    snapshot_.holdUntilUnix = 0;
    // a check of a feed the user has since left says nothing about this one
    if (feedGeneration_ != generation) {
      LogInfo("update: a check of the previous feed finished; its result is dropped");
      return false;
    }
    snapshot_.newestCode = newestCode;
    snapshot_.newestVersion = Widen(newestVersion);
    if (version::kCode == 0) {
      // Dev builds never self-update (spec §1) — report, offer nothing.
      snapshot_.lastCheck =
          newestCode ? CheckOutcome::DevBuild : CheckOutcome::NoUpdate;
    } else if (offer.code > version::kCode) {
      offer_ = offer;
      offerServerUnix_ = serverUnixSeconds;
      offerCheckedAt_ = steady_clock::now();
      snapshot_.lastCheck = CheckOutcome::UpdateFound;
      // A different (newer) release replaces whatever the banner said about
      // an older one; the SAME release keeps its standing ManualInstall/Failed
      // state — a periodic check must not wipe the outcome of a click. A
      // release the user chose Later on stays off the banner for this run.
      if (update::HiddenByLater(laterCode_, offer.code)) {
        LogInfo("update: v{} is offered, and Later keeps it off the banner until the next launch",
                Narrow(offer.version));
        // The banner does not go on offering another release, which the feed
        // no longer offers. The helper's report stays.
        if (snapshot_.phase != Phase::Result && snapshot_.code != 0 &&
            snapshot_.code != offer.code) {
          snapshot_.phase = Phase::None;
          snapshot_.stage = Stage::Idle;
          snapshot_.failure = Failure::None;
          snapshot_.version.clear();
          snapshot_.code = 0;
          snapshot_.installerPath.clear();
        }
      } else if (snapshot_.phase == Phase::None || snapshot_.code != offer.code) {
        // A report this offer replaces is read: it is not shown again at a
        // later launch, in the seconds before that launch's own check.
        if (snapshot_.phase == Phase::Result) replacedReport = snapshot_.result.finishedUtc;
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
      // The helper's report stays until the user dismisses it.
      if (snapshot_.phase != Phase::None && snapshot_.phase != Phase::Result) {
        snapshot_ = Snapshot{.installed = installed_,
                             .lastSuccessUnix = succeeded,
                             .lastCheck = CheckOutcome::NoUpdate,
                             .newestVersion = Widen(newestVersion),
                             .newestCode = newestCode,
                             .feed = feed_,
                             .marker = snapshot_.marker,
                             .change = snapshot_.change,
                             .changeCode = snapshot_.changeCode};
      }
      offer_ = Offer{};
    }
    snapshot_.offeredCode = offer_.code;
    // What the feed holds back until it has been out for a day is named only
    // when it is newer than this build and than what is offered, and never on
    // a dev build, which is offered nothing.
    const bool waiting = version::kCode != 0 && sel.waitingCode > version::kCode &&
                         sel.waitingCode > offer_.code;
    snapshot_.waitingCode = waiting ? sel.waitingCode : 0;
    snapshot_.waitingVersion = waiting ? Widen(sel.waitingVersion) : std::wstring{};
    snapshot_.waitingFromUnix = waiting ? sel.waitingFromUnixSeconds : 0;
    keepTag = offer_.tag;
    copy = snapshot_;
  }
  if (auto handler = HandlerCopy()) handler(copy);
  if (!replacedReport.empty()) SaveAppPref(kResultSeenPrefKey, Narrow(replacedReport));
  // A dev build is offered nothing and removes nothing: an installed copy of
  // the same user keeps its downloads in the same folder.
  if (version::kCode != 0) RemoveOtherDownloads(keepTag);
  return true;
}

void UpdateChecker::RemoveOtherDownloads(std::wstring const& keepTag) {
  // The tray app's downloads sit in the user's folder, one folder per release
  // tag. The one worth keeping is the offered release's: a checked copy an
  // attempt left, or the installer the banner shows. The installer of a
  // release that is no longer offered (withdrawn, or overtaken) goes, so the
  // folder never holds one this app would not hand over now.
  try {
    std::error_code ec;
    for (auto const& entry : fs::directory_iterator(UpdatesDir(), ec)) {
      if (!entry.is_directory(ec)) continue;
      const std::wstring name = entry.path().filename().wstring();
      // only what this app put there: a folder named as a release tag
      if (name == keepTag || version::ParseReleaseCode(Narrow(name)) == 0) continue;
      std::error_code removeError;
      fs::remove_all(entry.path(), removeError);
      if (!removeError) {
        LogInfo("update: removed the download of {}, which is not the release offered",
                Narrow(name));
      }
    }
  } catch (std::exception const& e) {
    LogWarn("update: the downloads of other releases could not be removed: {}", e.what());
  }
}

void UpdateChecker::NoteReplaced(std::uint64_t generation, std::wstring const& replaced) {
  MutateFor(generation, [this, &replaced](Snapshot& s) {
    // only on the banner that offers the release the feed offers now
    if (s.phase != Phase::Available || s.code == 0 || s.code != offer_.code) return;
    s.replacedVersion = replaced;
    s.replacedByCode = s.code;
  });
}

// ---- the apply ---------------------------------------------------------------

void UpdateChecker::RunApply(std::uint64_t generation, bool manual) {
  // The machine's channel first: a change since the click drops it below.
  SyncChannel();
  Offer offer;
  bool stale = false;
  Snapshot claimed;
  {
    std::lock_guard lock(mutex_);
    // Update answers what offers the release; ShowInstaller also answers the
    // report of an update that did not install.
    const bool actionable = snapshot_.phase == Phase::Available ||
                            snapshot_.phase == Phase::Failed ||
                            snapshot_.phase == Phase::ManualInstall ||
                            (manual && snapshot_.phase == Phase::Result);
    if (!actionable || offer_.code == 0 || feedGeneration_ != generation) return;
    // The click is on the release the banner shows, and that is the release
    // offered.
    if (snapshot_.code != offer_.code) return;
    offer = offer_;
    // How old the offer is: from its list's Date header by the steady clock,
    // not by this machine's date.
    const std::int64_t sinceCheck =
        std::chrono::duration_cast<std::chrono::seconds>(steady_clock::now() - offerCheckedAt_)
            .count();
    stale = update::OfferMayHaveChanged(*offer.feed, offerServerUnix_, sinceCheck);
    // The click is taken under the lock that judged it. From here the banner
    // shows its action disabled and no Later, so nothing the user does
    // crosses it.
    snapshot_.phase = Phase::Applying;
    snapshot_.stage = stale ? Stage::Checking : Stage::Downloading;
    snapshot_.failure = Failure::None;
    snapshot_.version = offer.version;
    snapshot_.code = offer.code;
    snapshot_.installerPath.clear();
    snapshot_.replacedVersion.clear();
    snapshot_.replacedByCode = 0;
    claimed = snapshot_;
  }
  if (auto handler = HandlerCopy()) handler(claimed);
  // On an installed copy the helper installs; on any other copy, and
  // whenever the user asked for the installer, the user runs it.
  const bool viaHelper = installed_ && !manual;
  // Every stage below starts only if the feed is still the one the offer came
  // from: a channel change stops the apply at the next stage, and the latest
  // it can is the hand-off to the helper.
  const auto abandoned = [&offer](fs::path const& dir) {
    LogInfo("update: the update channel changed; v{} is not installed", Narrow(offer.version));
    std::error_code ignored;
    fs::remove_all(dir, ignored);
  };
  const auto fail = [this, generation](Failure f) {
    MutateFor(generation, [f](Snapshot& s) {
      s.phase = Phase::Failed;
      s.stage = Stage::Idle;
      s.failure = f;
    });
  };
  const auto cancelled = [this] {
    std::lock_guard lock(mutex_);
    return stop_;
  };
  // The banner may have been up for hours, and the release behind it may have
  // been withdrawn or overtaken since. When its offer is no longer fresh, the
  // check runs again before anything else, on every path. The helper would
  // refuse a release its own list does not offer, but only after the download
  // and the administrator prompt; and when the user is handed the installer to
  // run, nothing else asks GitHub at all. What becomes of the click is
  // StepAfterCheck's to say (UpdateSchedule.h), from what stands under one
  // lock once the check has returned.
  if (stale) {
    LogInfo("update: the offer of v{} is no longer fresh; asking GitHub whether it stands",
            Narrow(offer.version));
    const bool checked = RunCheck(generation);
    update::ClickStep step = update::ClickStep::Dropped;
    Offer offered;
    {
      std::lock_guard lock(mutex_);
      // A click made while that check ran was on the banner as it stood
      // before it, and starts nothing more.
      applyRequested_ = false;
      manualRequested_ = false;
      offered = offer_;
      step = update::StepAfterCheck(
          {.clickedCode = offer.code,
           .sameFeed = feedGeneration_ == generation,
           .checked = checked,
           .offeredCode = offer_.code,
           .bannerHolds = snapshot_.phase == Phase::Applying && snapshot_.code == offer.code,
           .laterCode = laterCode_,
           .viaHelper = viaHelper});
    }
    switch (step) {
      case update::ClickStep::Proceed:
        // the same release, as the list gives it now
        offer = offered;
        break;
      case update::ClickStep::Replaced:
        LogInfo("update: the feed offers v{} now, not v{}; nothing was started",
                Narrow(offered.version), Narrow(offer.version));
        NoteReplaced(generation, offer.version);
        return;
      case update::ClickStep::Withdrawn:
        LogInfo("update: v{} is no longer offered; nothing was started", Narrow(offer.version));
        return;
      case update::ClickStep::Unconfirmed:
        LogWarn("update: GitHub could not be asked whether v{} is still offered; its installer "
                "is not shown",
                Narrow(offer.version));
        fail(Failure::Unconfirmed);
        return;
      case update::ClickStep::Dropped:
        LogInfo("update: the click on v{} no longer stands; nothing was started",
                Narrow(offer.version));
        // a banner still held for this click closes
        MutateFor(generation, [&offer](Snapshot& s) {
          if (s.phase != Phase::Applying || s.code != offer.code) return;
          s.phase = Phase::None;
          s.stage = Stage::Idle;
          s.version.clear();
          s.code = 0;
        });
        return;
    }
  }
  // The helper asks GitHub for the release list itself: while GitHub holds
  // this network's requests, it would only be refused, after the prompt and
  // the download.
  bool held = false;
  {
    std::lock_guard lock(mutex_);
    held = viaHelper && steady_clock::now() < holdUntil_;
  }
  if (held) {
    LogWarn("update: GitHub asked for no request yet; the update helper is not started");
    fail(Failure::Held);
    return;
  }
  MutateFor(generation, [&offer](Snapshot& s) {
    s.phase = Phase::Applying;
    s.stage = Stage::Downloading;
    s.failure = Failure::None;
    s.version = offer.version;
    s.code = offer.code;
    s.installerPath.clear();
  });
  LogInfo("update: applying v{} (code {}){}", Narrow(offer.version),
          static_cast<unsigned long long>(offer.code), viaHelper ? "" : ", as an installer to run");

  // ---- (a) download the own-arch MSI ----------------------------------------
  // A checked copy an earlier attempt kept is used again; otherwise a fresh
  // per-tag directory, so nothing from a failed try can leak into this one.
  std::error_code ec;
  const fs::path dir = UpdatesDir() / offer.tag;
  const fs::path msiPath = dir / Widen(offer.msiName);
  if (IsCheckedDownload(msiPath, offer.digestHex)) {
    LogInfo("update: {} is downloaded and checked already", offer.msiName);
  } else {
    fs::remove_all(dir, ec);
    ec.clear();
    fs::create_directories(dir, ec);
    if (ec) {
      LogError("update: could not create {}: {}", Narrow(dir.wstring()),
               ec.message());
      fail(Failure::Download);
      return;
    }
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
  if (!MutateFor(generation, [](Snapshot& s) { s.stage = Stage::Verifying; })) {
    abandoned(dir);
    return;
  }
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

  // ---- (c) the install: the helper on an installed copy, the user elsewhere ---
  const std::wstring msiW = msiPath.wstring();
  if (!viaHelper) {
    // A developer build installs only through the helper's own checks: its
    // installer is never handed over to run.
    if (!update::HandsOverInstaller(*offer.feed)) {
      LogWarn("update: v{} is a release of the {} feed; its installer is not handed over",
              Narrow(offer.version), offer.feed->id);
      fs::remove_all(dir, ec);
      fail(Failure::Elevation);
      return;
    }
    // A portable or dev copy elevates nothing, and neither does a user who
    // asked for the installer. The checked MSI is the user's to run as an
    // installer, and the banner says what it was checked against.
    LogInfo("update: showing the installer for v{} to run", Narrow(offer.version));
    if (!MutateFor(generation, [&msiW](Snapshot& s) {
          s.phase = Phase::ManualInstall;
          s.stage = Stage::Idle;
          s.installerPath = msiW;
        })) {
      abandoned(dir);
      return;
    }
    RevealInExplorer(msiW);
    return;
  }
  // The hand-off, checked and marked under one lock: a release of a feed the
  // user has since left is never handed to the helper. (The helper decides
  // its feed itself, so a change during the elevation prompt cannot make it
  // install from the old one either.)
  if (!MutateFor(generation, [](Snapshot& s) { s.stage = Stage::Installing; })) {
    abandoned(dir);
    return;
  }
  HANDLE helper = nullptr;
  DWORD refusal = ERROR_SUCCESS;
  std::string launchError;
  const std::int64_t started = NowUnixSeconds();
  // The VPN from here on (UpdateMarker.h VpnNote, the lead's G10 and G10a).
  // The installer stops the VPN service and the app does not connect by
  // itself, so the report says to connect again when there was a VPN to lose.
  // Its state here is the last status the service pushed, else the state the
  // service answered, else not known; and a VPN up at any moment from here
  // until the note goes into the record or the report counts.
  const update::VpnNote handOff = HandOffVpn();
  if (handOff.source == update::VpnSource::Unknown) {
    LogInfo("update: whether a VPN is connected as the update begins is not known: the "
            "service has told this app nothing");
  } else {
    LogInfo("update: a VPN is {}connected as the update begins, as the service {}",
            handOff.upSinceHandOff ? "" : "not ",
            handOff.source == update::VpnSource::Pushed ? "last pushed"
                                                        : "answered (it has pushed no status)");
  }
  if (!LaunchUpdateHelper(installFolder_ / kHelperName, offer.tag, &helper, &refusal,
                          launchError)) {
    LogWarn("update: the update helper did not start ({}); nothing was installed", launchError);
    // the installer is offered instead only for an official release
    fail(refusal == kElevationRefusedUnsigned && update::HandsOverInstaller(*offer.feed)
             ? Failure::Unsigned
             : Failure::Elevation);
    return;
  }
  LogInfo("update: the update helper is installing v{}; the installer closes this app before "
          "it replaces it",
          Narrow(offer.version));
  MutateFor(generation, [](Snapshot& s) { s.stage = Stage::Helper; });
  if (!helper) return;

  // Wait without quitting: the installer's CloseApplication closes this app
  // (WM_CLOSE, a close request), and the app's teardown stops this worker,
  // which leaves the helper running on its own. A helper that ends while this
  // app still runs refused, failed, or installed only up to a restart.
  const update::HelperWait waited = update::AwaitHelper(
      [helper] { return ::WaitForSingleObject(helper, 250) == WAIT_OBJECT_0; }, cancelled);
  if (waited == update::HelperWait::AppExiting) {
    // This app is exiting while the helper runs, the installer closing it:
    // from now until the helper ends, a launch is refused rather than started
    // over the files the installer replaces (UpdateMarker.h). Recorded before
    // this thread ends, and so before the process does (AppController's
    // Shutdown joins it); until now every launch reached this app.
    RecordUpdateInProgress(helper, NoteVpn());
    ::CloseHandle(helper);
    return;
  }
  DWORD exitCode = 0;
  const bool read = ::GetExitCodeProcess(helper, &exitCode) != 0;
  ::CloseHandle(helper);
  LogInfo("update: the update helper ended with {}", exitCode);

  // Its report, when it wrote one for this run; after a refusal before it
  // could, its exit code alone.
  const std::int64_t ended = read ? static_cast<std::int64_t>(exitCode) : 1603;
  update::UpdateResult outcome{.tag = Narrow(offer.tag), .code = offer.code, .exitCode = ended};
  Result result{.version = offer.version, .exitCode = ended};
  result.vpn = NoteVpn();
  const std::optional<update::UpdateResult> report =
      ReadResult(installFolder_ / L"updates" / L"last-result.json");
  if (report && update::IsReportOfRun(*report, outcome.tag, ended, started)) {
    outcome = *report;
    result.logPath = ResultLogPath(installFolder_, *report);
    result.finishedUtc = Widen(report->finishedUtc);
  }
  result.view = update::ViewOfReport(outcome, version::kCode, /*live=*/true,
                                     /*restartedSince=*/false);
  // The checked copy stays while the release has not installed, for its
  // installer to be shown; once it has, it has done its job.
  if (update::KeepsPackage(ended)) fs::remove_all(dir, ec);
  // The helper's own list offers another release, or none. The check runs
  // here, and when the feed does offer another release, that one takes the
  // banner and says what it replaced. The refusal's report would have stood
  // for the moment before a queued check replaced it, so it is not shown, now
  // or at a later launch.
  if (ended == static_cast<std::int64_t>(update::Refusal::NotOffered)) {
    const bool checked = RunCheck(generation);
    update::ClickStep step = update::ClickStep::Dropped;
    {
      std::lock_guard lock(mutex_);
      step = update::StepAfterCheck(
          {.clickedCode = offer.code,
           .sameFeed = feedGeneration_ == generation,
           .checked = checked,
           .offeredCode = offer_.code,
           .bannerHolds = snapshot_.phase == Phase::Applying && snapshot_.code == offer.code,
           .laterCode = laterCode_,
           .viaHelper = true});
    }
    if (step == update::ClickStep::Replaced) {
      if (!result.finishedUtc.empty()) {
        SaveAppPref(kResultSeenPrefKey, Narrow(result.finishedUtc));
      }
      NoteReplaced(generation, offer.version);
      return;
    }
  }
  // Not generation-gated: the helper ran, and its report is true whatever the
  // feed is now.
  Mutate([this, &result](Snapshot& s) {
    s.stage = Stage::Idle;
    s.failure = Failure::None;
    s.offeredCode = offer_.code;
    if (result.view == update::ReportView::Hidden) {
      s.phase = Phase::None;
      s.result = Result{};
      return;
    }
    s.phase = Phase::Result;
    s.version = result.version;
    s.result = result;
  });
}

void UpdateChecker::RunReveal(std::uint64_t generation) {
  std::wstring installer;
  std::string digest;
  // Twice at most: once more after the check an offer that is no longer fresh
  // gets first. Nothing but that check asks GitHub before the user is shown
  // the installer again, and it may have moved the banner on.
  for (bool asked = false;; asked = true) {
    bool stale = false;
    {
      std::lock_guard lock(mutex_);
      if (snapshot_.phase != Phase::ManualInstall || snapshot_.installerPath.empty() ||
          feedGeneration_ != generation || offer_.code != snapshot_.code ||
          !update::HandsOverInstaller(*offer_.feed)) {
        return;
      }
      installer = snapshot_.installerPath;
      digest = offer_.digestHex;
      const std::int64_t sinceCheck =
          std::chrono::duration_cast<std::chrono::seconds>(steady_clock::now() - offerCheckedAt_)
              .count();
      stale = !asked && update::OfferMayHaveChanged(*offer_.feed, offerServerUnix_, sinceCheck);
    }
    if (!stale) break;
    if (!RunCheck(generation)) {
      LogWarn("update: GitHub could not be asked whether the release is still offered; its "
              "installer is not shown again");
      MutateFor(generation, [](Snapshot& s) {
        if (s.phase != Phase::ManualInstall) return;
        s.phase = Phase::Failed;
        s.stage = Stage::Idle;
        s.failure = Failure::Unconfirmed;
        s.installerPath.clear();
      });
      return;
    }
  }
  // The installer sits in the user's folder, where any of the user's
  // processes can write: it is shown again only while it still is what
  // GitHub's SHA-256 says, and it is never called verified.
  const std::string actual = Sha256File(installer);
  if (digest.empty() || actual.empty() || !update::EqualsAsciiCaseless(actual, digest)) {
    LogError("update: {} no longer matches GitHub's SHA-256 (got '{}', want '{}'); it is deleted",
             Narrow(installer), actual, digest);
    std::error_code ec;
    fs::remove(installer, ec);
    MutateFor(generation, [](Snapshot& s) {
      s.phase = Phase::Failed;
      s.stage = Stage::Idle;
      s.failure = Failure::Checksum;
      s.installerPath.clear();
    });
    return;
  }
  RevealInExplorer(installer);
}

}  // namespace urnw
