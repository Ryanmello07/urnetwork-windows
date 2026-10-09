// The in-app update checker (beta-distribution spec §5): finds newer official
// releases, and installs one through the elevated update helper
// (app/src/Updater) after a verified download.
//
// The feed is urnetwork/build's GitHub releases (Common/ReleaseSelection.h
// kOfficialFeed, polled by the repository's numeric id): the releases the
// release pipeline publishes, each carrying one MSI per architecture. A
// release counts once it has been out for a day, judged at the start of
// GitHub's day by the release list's own Date header, so what is offered
// changes at most once a day, and the update helper, which judges the same
// way, comes to the same release (ReleaseSelection.h HasSoaked).
// Poll the release list (on launch after ~30s, then every 6 hours, and on the
// two manual triggers), pick the release with Common/ReleaseSelection.h, and when
// it outranks the build's own stamped code, offer ONE click that
//
//   downloads the own-arch MSI to %LOCALAPPDATA%\URnetwork\updates\<tag>\ and
//     checks it against the asset's own SHA-256 digest, stamped by GitHub in
//     the same releases JSON the check parsed. This copy is a pre-check: it
//     shows the banner's progress and finds a broken release before anyone is
//     asked for administrator rights. Nothing elevated ever reads it;
//   on an installed copy, one whose folder is an admin-only install location
//     (Common/InstallLocation.h), starts URnetworkUpdate.exe --apply-update
//     <tag> from that folder with "runas" and waits on it without quitting.
//     The helper fetches, checks and installs the release itself. The MSI
//     closes this app before it replaces its files (its CloseApplication
//     sends the tray's window WM_CLOSE), and as the app exits it records the
//     helper, so no launch starts the app until the helper ends
//     (Common/UpdateMarker.h). The MSI starts the new version once its files
//     are in place, and the helper starts the old one again after an install
//     that failed;
//   on a portable or dev copy, shows the checked MSI in Explorer for the user
//     to run as an installer. Nothing from a user's folder is elevated.
//
// The helper reports in last-result.json in the install folder
// (Common/UpdateResult.h). This app reads it when the helper ends while it
// still runs, and at its next launch, and the banner says how the update went
// until the user dismisses it, for as long as the report is still true of
// this build (ViewOfReport). After a release did not install, the banner can
// show its installer instead (the portable path), so a release the helper
// keeps refusing is not a dead end; and the checked copy stays until the
// report says the update took.
//
// The banner's Later hides the offered release until the next launch: it is
// kept in memory, never saved, and a newer release is shown
// (Common/UpdateSchedule.h HiddenByLater).
//
// The helper asks GitHub itself and installs only the release its own list
// offers. When GitHub's day has changed since the check behind the banner, a
// newer release may have reached the end of its day, so the check runs again
// before the download and the administrator prompt, and a different offer
// replaces the banner instead of being refused by the helper afterwards
// (ReleaseSelection.h OfferMayHaveChanged). One that changes in the minutes
// between that check and the helper's own is refused there (NotOffered), and
// a check follows.
//
// Requests are anonymous, and GitHub's answer to too many is honoured: no
// request goes before its Retry-After or X-RateLimit-Reset (at most a day
// out, Common/UpdateSchedule.h), and the helper, which asks GitHub itself, is
// not started before then either. When no check has succeeded for 72 hours,
// the banner and the developer line say since when.
//
// The feed can change under a check or an apply in flight (the opt-in
// developer channel switches it): each carries the feed generation it started
// under, a check's result from an older one is dropped, and an apply stops
// before it starts the helper.
//
// A dev build (urnw::version::kCode == 0) never self-updates: every release
// would outrank it forever. The periodic checker is fully disabled there; the
// developer screen's manual trigger still RUNS a check and reports what it
// found, because that is the only way to exercise this code path on a dev box.
//
// Threading follows SdkHost's standing-value contract (see CurrentAdvancedMode):
// one worker thread owns every check and every apply, Current() is valid at any
// time including before any view exists, the handler is an optimisation for
// changes after a view binds, and a surface built later binds then replays.
// Handlers are invoked on the WORKER thread and must marshal.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "UpdateResult.h"

namespace urnw {

class UpdateChecker {
 public:
  // What the banner shows. One phase, not flags: every phase names exactly one
  // banner rendering, and None is "no banner at all".
  enum class Phase {
    None,         // nothing newer is known (or the checker is disabled)
    Available,    // a newer release exists; the one click is offered
    Applying,       // the click fired; `stage` says how far it has got
    ManualInstall,  // a portable or dev copy: the MSI was downloaded, checked
                    // against GitHub's SHA-256 and shown in Explorer, and the
                    // user runs it
    Failed,         // the last apply attempt failed before the helper ran;
                    // `failure` says where. Nothing was installed, and the
                    // click retries from scratch.
    Result,         // how the last update the helper ran went (`result`),
                    // until the user dismisses it or a newer release replaces
                    // it
  };
  // Installing: the elevation prompt is up. Helper: the helper runs, with
  // its own download and check, then msiexec.
  enum class Stage { Idle, Downloading, Verifying, Installing, Helper };
  enum class Failure {
    None,
    Download,
    Checksum,
    // The elevation prompt was declined, or the helper could not be started.
    Elevation,
    // GitHub asked this network to wait (holdUntilUnix) before asking it
    // again, and the helper must ask it: nothing was started.
    Held,
    // Windows elevates only signed programs here, and the helper is not
    // signed: the installer is offered instead.
    Unsigned,
  };

  // What the last CHECK concluded — the developer screen's line, separate from
  // the banner phase because "checked and found nothing" must be reportable
  // without putting anything on the connect screen.
  enum class CheckOutcome {
    NeverRan,
    InFlight,
    NoUpdate,     // newest parsed release does not outrank this build
    UpdateFound,  // it does, and the banner phase says so too
    DevBuild,     // a release exists but kCode==0 — dev builds never self-update
    Failed,       // the HTTP fetch or the JSON parse failed; details in the log
  };

  // An update the helper ran, as its last-result.json reported it.
  struct Result {
    std::wstring version;  // the release's, v-less
    // msiexec's exit code, or the helper's refusal (Common/UpdateResult.h)
    std::int64_t exitCode = 0;
    // install.log for msiexec's outcome, update-helper.log for a refusal;
    // empty when the helper refused before it could write a report
    std::wstring logPath;
    std::wstring finishedUtc;
    // What the banner says about it for this build (never Hidden here: a
    // hidden report is no Result).
    update::ReportView view = update::ReportView::Hidden;
  };

  struct Snapshot {
    Phase phase = Phase::None;
    Stage stage = Stage::Idle;        // meaningful while Applying
    Failure failure = Failure::None;  // meaningful while Failed
    // The offered release (v-less grammar, e.g. "2026.8.9-101076420-beta").
    // Empty when phase == None.
    std::wstring version;
    std::uint64_t code = 0;
    // ManualInstall: where the checked MSI sits, for the banner's wording and
    // its re-reveal action.
    std::wstring installerPath;
    // This copy runs from an admin-only install location, so Update runs the
    // helper; otherwise the banner offers the installer to download.
    bool installed = false;
    // Meaningful while phase == Result.
    Result result;
    // The release a check offers now, whatever the banner shows; 0 when none.
    // A Result about this release can offer its installer (ShowInstaller).
    std::uint64_t offeredCode = 0;
    // When a check last succeeded, in Unix seconds, or, before any has, when
    // this install first tried; 0 before either.
    std::int64_t lastSuccessUnix = 0;
    // No check has succeeded for 72 hours while automatic checks are on: the
    // app says so, with lastSuccessUnix.
    bool checkStale = false;
    // GitHub asked for no request before this Unix second (Retry-After,
    // X-RateLimit-Reset); 0 when it has not, or that time has passed.
    std::int64_t holdUntilUnix = 0;
    CheckOutcome lastCheck = CheckOutcome::NeverRan;
    // The newest release tag the last completed check parsed, whether or not
    // it outranks this build — the developer line names it either way.
    std::wstring newestVersion;
    std::uint64_t newestCode = 0;
    // A release newer than this build and than the offer that the feed holds
    // back until it has been out for a day, and the Unix second it is offered
    // from; code 0 when there is none. The developer line names it.
    std::wstring waitingVersion;
    std::uint64_t waitingCode = 0;
    std::int64_t waitingFromUnix = 0;
  };

  using Handler = std::function<void(Snapshot const&)>;

  UpdateChecker() = default;
  ~UpdateChecker();
  UpdateChecker(UpdateChecker const&) = delete;
  UpdateChecker& operator=(UpdateChecker const&) = delete;

  // Spawn the worker: the helper's last report and stale-file cleanup first,
  // then the launch-delay check and the 6h cadence.
  void Start();
  // Signal and JOIN the worker. A download in flight notices within one read
  // (the fetch loop polls the stop flag), and the wait on the helper within a
  // fraction of a second, so this is bounded, not "until the whole MSI
  // finishes" or "until the helper ends". The helper carries on without this
  // app.
  void Stop();

  Snapshot Current();
  // Store only — never invokes. Bind, then replay Current() yourself: the main
  // window is built on the first tray click, which can be minutes after the
  // launch check already ran.
  void SetHandler(Handler h);

  // Queue a check now (the developer screen's trigger). Coalesces with a check
  // already queued; ignored only after Stop().
  void CheckNow();
  // Queue the download/verify/install for the currently offered release.
  // Ignored when nothing is offered or an apply is already running.
  void BeginApply();
  // Queue the portable path for the offered release on any copy: download
  // (or reuse the checked copy), check it, and show the installer for the
  // user to run. The way out of a release the helper did not install, and of
  // a Windows that elevates only signed programs.
  void ShowInstaller();
  // The ManualInstall banner's "Show file": checks the shown installer
  // against GitHub's SHA-256 again before it shows it, since it sits in the
  // user's folder. Safe from the UI thread.
  void RevealInstaller();
  // The Result banner's dismissal: the report is not shown again, and the
  // banner closes. Safe from the UI thread.
  void DismissResult();
  // The banner's Later: the release it offers is not shown again until the
  // next launch. Nothing is saved, and a newer release is shown. Ignored
  // unless the banner offers Later (OffersLater). Safe from the UI thread.
  void Later();
  // The update channel changed (the opt-in developer channel calls this): a
  // check or an apply still running under the old feed is abandoned, the
  // offer and its banner are dropped (the helper's report stays), and a check
  // is queued.
  void ChannelChanged();

  // The "Check for updates automatically" preference (Settings): persisted in
  // app_prefs.json beside Advanced Mode, default ON. The static read exists so
  // the Settings row can seed itself without reaching the instance.
  static bool AutoCheckEnabled();
  // Persist + apply. Turning it ON schedules a check right away — the user
  // just asked for updates, so "in six hours" would be a strange answer.
  void SetAutoCheckEnabled(bool on);

  // Whether the Result banner offers the release's installer (ShowInstaller)
  // rather than only its dismissal: the release did not install, and a check
  // still offers it.
  static bool OffersInstaller(Snapshot const& snapshot);
  // Whether the banner offers Later beside its action: it offers a release,
  // and nothing is in flight for it.
  static bool OffersLater(Snapshot const& snapshot);
  // Open an Explorer window with `file` selected.
  static void RevealInExplorer(std::wstring const& file);
  // `unixSeconds` as the user's short local date, for "Couldn't check for
  // updates since <date>".
  static std::wstring LocalDate(std::int64_t unixSeconds);
  // `unixSeconds` as the user's short local date and time, for "GitHub asked
  // to wait until <time>".
  static std::wstring LocalDateTime(std::int64_t unixSeconds);

 private:
  // The release a check decided to offer: everything the apply needs, captured
  // at check time so a repo that changes mid-flight cannot redirect an apply
  // the user already clicked.
  struct Offer {
    std::wstring version;  // v-less
    std::uint64_t code = 0;
    std::wstring tag;      // as minted, with the v — names the download dir
    std::wstring msiUrl;   // browser_download_url of the own-arch MSI
    // The MSI asset's expected SHA-256 (lowercase hex), parsed out of the SAME
    // asset object the msiUrl came from — never re-looked-up later, so a repo
    // that changes mid-flight cannot pair this hash with a different download.
    std::string digestHex;
    std::string msiName;   // the exact asset filename, names the file on disk
  };

  void WorkerLoop();
  // Each runs for the feed generation the worker read when it started it.
  // `manual`: the portable path, whatever this copy is (ShowInstaller).
  void RunCheck(std::uint64_t generation);
  void RunApply(std::uint64_t generation, bool manual);
  void RunReveal(std::uint64_t generation);
  // Best-effort startup hygiene: drop <name>.old / <name>.old-<code> leftovers
  // next to the exe (renamed images from the portable builds' old rename-swap
  // updater) and download dirs whose tag no longer outranks this build.
  void CleanupStaleFiles();
  // The helper's last-result.json, shown as the Result phase unless it was
  // dismissed already. Installed copies only: elsewhere no file there can be
  // trusted to be the helper's.
  void ShowLastResult();

  // Copy the snapshot under the lock, mutate, publish to the handler outside
  // it — the handler is never invoked with mutex_ held.
  void Mutate(std::function<void(Snapshot&)> const& fn);
  // Mutate, unless the feed generation is no longer `generation`: the work
  // that asks was started for a feed the user has since left. Says whether it
  // did, decided under the same lock as the change.
  bool MutateFor(std::uint64_t generation, std::function<void(Snapshot&)> const& fn);
  // A check failed: the snapshot says so, and whether checks have been
  // failing long enough to tell the user.
  void CheckFailed(std::uint64_t generation);
  Handler HandlerCopy();

  // Written by Start before the worker exists, and only read after.
  bool installed_ = false;
  std::filesystem::path installFolder_;

  std::mutex mutex_;
  std::condition_variable cv_;
  std::thread worker_;
  bool stop_ = false;
  bool checkRequested_ = false;
  bool applyRequested_ = false;
  bool manualRequested_ = false;
  bool revealRequested_ = false;
  bool autoCheck_ = true;
  std::chrono::steady_clock::time_point nextAuto_{};
  // No request before this: GitHub's Retry-After or rate-limit reset, and
  // the same instant on the wall clock for what the banner says.
  std::chrono::steady_clock::time_point holdUntil_{};
  std::int64_t holdUntilUnix_ = 0;
  // Bumped by ChannelChanged; see the header comment.
  std::uint64_t feedGeneration_ = 0;
  Snapshot snapshot_;
  Offer offer_;
  // GitHub's clock at the check that made offer_ (the list's Date header),
  // and this machine's steady clock then. Together they say whether GitHub's
  // day has changed since, without this machine's wall clock.
  std::int64_t offerServerUnix_ = 0;
  std::chrono::steady_clock::time_point offerCheckedAt_{};
  // The release the banner's Later hid, 0 when none: not shown again in this
  // run. A member and nothing else, so the next launch offers it again.
  std::uint64_t laterCode_ = 0;

  // The handler's own lock, on SdkHost's advancedMutex_ reasoning: never held
  // across an invocation, never taken together with mutex_.
  std::mutex handlerMutex_;
  Handler handler_;
};

}  // namespace urnw
