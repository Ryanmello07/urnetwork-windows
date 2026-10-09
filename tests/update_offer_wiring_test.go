// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"testing"
)

// The call sites of the official feed's rules. What the rules decide runs in
// update_release_test.go, against the headers themselves
// (Common/ReleaseSelection.h, Common/UpdateSchedule.h): which repository is
// polled, how long a release must have been out and unchanged, when an offer
// is no longer fresh, what Later hides, and what becomes of a click once the
// check before it has returned. The tray app and the update helper need
// Windows, so the checks here do not run them: they read their sources, with
// every comment blanked, and pin the lines that hand those decisions their
// inputs and act on their answers. They are source pins. A pin fails when its
// line is gone or moved; it does not show that the tray app behaves, which
// only the pure tests and a run of the app do. Each check returns the
// problems it found, so the negative controls below can run the same check on
// a rewritten source and require it to fail.
//
// What is pinned: the tray app and the update helper ask GitHub for the same
// list and judge it by its own date alone; nothing the app saves takes part
// in what is offered; Later is kept in memory, and a check the user asks for
// clears it; a click on an offer that is no longer fresh checks again before
// anything else, on every path, and goes on only for the release it was on;
// the tray app keeps the download of the release it offers and of no other;
// the developer line names the release held back without promising it; and
// the README sends people to the feed the code polls.

const (
	offerRunCheck  = "bool UpdateChecker::RunCheck(std::uint64_t generation) {"
	offerRunApply  = "void UpdateChecker::RunApply(std::uint64_t generation, bool manual) {"
	offerRunReveal = "void UpdateChecker::RunReveal(std::uint64_t generation) {"
)

// The tray app's check and the helper's own fetch ask for one URL, built in
// one place (ReleaseSelection.h ReleaseListUrl): the feed's repository by its
// id, and the same page of it. Two spellings could be two pages, and the
// helper would then refuse what the tray offered.
func checkBothAskForTheSameList(checker, apply, selection string) []string {
	var problems []string
	problems = append(problems, applyOrderProblems("UpdateChecker::RunCheck", applyDefinition(checker, offerRunCheck),
		regexp.QuoteMeta("const update::Feed& feed = update::kOfficialFeed;"),
		regexp.QuoteMeta("const std::wstring url = Widen(update::ReleaseListUrl(feed));"),
		regexp.QuoteMeta("FetchUrl("),
		`url, L"application/vnd\.github\+json", kMaxJsonBytes,\s*false,`)...)
	problems = append(problems, applyOrderProblems("ApplyUpdate",
		applyDefinition(apply, "int ApplyUpdate(std::wstring_view tagArgument) {"),
		regexp.QuoteMeta("const update::Feed& feed = ChannelFeed();"),
		regexp.QuoteMeta("const std::wstring listUrl = WidenAscii(update::ReleaseListUrl(feed));"),
		regexp.QuoteMeta("HttpGet("),
		regexp.QuoteMeta(`listUrl, L"application/vnd.github+json", kMaxListBytes,`))...)
	for name, source := range map[string]string{"UpdateChecker.cpp": checker, "ApplyUpdate.cpp": apply} {
		if strings.Contains(source, "api.github.com") {
			problems = append(problems, name+" spells a GitHub API URL of its own: the release list is ReleaseListUrl's")
		}
	}
	if count := strings.Count(selection, "api.github.com"); count != 1 {
		problems = append(problems, fmt.Sprintf("ReleaseSelection.h spells %d GitHub API URLs, want the release list's alone", count))
	}
	problems = append(problems, applyOrderProblems("ReleaseListUrl",
		applyDefinition(selection, "inline std::string ReleaseListUrl(const Feed& feed) {"),
		regexp.QuoteMeta(`return "https://api.github.com/repositories/" + std::to_string(feed.numericRepoId) +`),
		regexp.QuoteMeta(`"/releases?per_page=" + std::to_string(kReleaseListPageSize);`))...)
	return problems
}

// Both judge the list by its own Date header and by nothing else: how new a
// code may be and how long a release has been out are GitHub's clock against
// GitHub's times. A list without that header offers nothing on either side.
// The soak is applied inside SelectRelease, which both call once; neither
// judges it again with a date or a time of its own.
func checkTheListIsJudgedByItsOwnDate(checker, apply string) []string {
	var problems []string
	check := applyDefinition(checker, offerRunCheck)
	problems = append(problems, applyOrderProblems("UpdateChecker::RunCheck", check,
		regexp.QuoteMeta("update::ParseReleaseList(body);"),
		regexp.QuoteMeta("if (headers.serverUnixSeconds == 0) {"),
		regexp.QuoteMeta("CheckFailed(generation);"),
		regexp.QuoteMeta("return false;"),
		regexp.QuoteMeta("SaveAppPref(kLastSuccessPrefKey, succeeded);"),
		regexp.QuoteMeta("const std::int64_t serverUnixSeconds = headers.serverUnixSeconds;"),
		regexp.QuoteMeta("update::SelectRelease(*parsed, kArch, feed, serverUnixSeconds);"))...)
	// the variable, not the .serverUnixSeconds member of the rate limit
	if assigned := regexp.MustCompile(`[^.\w]serverUnixSeconds\s*=[^=]`).FindAllString(check, -1); len(assigned) != 1 {
		problems = append(problems, fmt.Sprintf("UpdateChecker::RunCheck gives the list's date a value %d times, "+
			"want once, from the Date header: this machine's clock must not date a release", len(assigned)))
	}
	helper := applyDefinition(apply, "int ApplyUpdate(std::wstring_view tagArgument) {")
	problems = append(problems, applyOrderProblems("ApplyUpdate", helper,
		regexp.QuoteMeta("if (list.serverUnixSeconds == 0) {"),
		regexp.QuoteMeta("return refuse(Refusal::ReleaseList,"),
		regexp.QuoteMeta("update::SelectRelease(*releases, kArch, feed, list.serverUnixSeconds);"),
		regexp.QuoteMeta("if (!update::SelectionOffers(selection, tag, version::kCode)) {"),
		regexp.QuoteMeta("return refuse(Refusal::NotOffered,"))...)
	for name, source := range map[string]string{"UpdateChecker.cpp": checker, "ApplyUpdate.cpp": apply} {
		if count := strings.Count(source, "SelectRelease("); count != 1 {
			problems = append(problems, fmt.Sprintf("%s selects a release %d times, want once", name, count))
		}
		for _, own := range []string{"HasSoaked(", "SoakStartOf(", "SoakEndUnixSeconds(", "UtcDayStart(", "feed.soakSeconds",
			"publishedAt", "updatedAt"} {
			if strings.Contains(source, own) {
				problems = append(problems, name+" reads "+own+
					": how long a release has been out is SelectRelease's to judge, from the list's date")
			}
		}
	}
	return problems
}

// What is offered is decided from the release list and its date each time,
// and from nothing the app saved: the checker's preferences are the three it
// had (automatic checks, the dismissed report, the last success), and the
// check and the apply read none. So what is offered is the same after a
// restart with nothing to restore, and a preferences file a user's process
// can write decides nothing about the offer.
func checkNothingSavedDecidesTheOffer(checker string) []string {
	var problems []string
	keys := map[string]string{}
	for _, match := range regexp.MustCompile(`constexpr char (k\w+PrefKey)\[\] = "([a-z_]+)";`).FindAllStringSubmatch(checker, -1) {
		keys[match[1]] = match[2]
	}
	want := map[string]string{
		"kAutoCheckPrefKey":   "check_updates_automatically",
		"kResultSeenPrefKey":  "update_result_seen",
		"kLastSuccessPrefKey": "update_last_check_success",
	}
	var names []string
	for name := range keys {
		names = append(names, name)
	}
	sort.Strings(names)
	if len(keys) != len(want) {
		problems = append(problems, fmt.Sprintf("UpdateChecker.cpp has the preference keys %v, want the three it had: "+
			"what is offered must not come from a saved value", names))
	}
	for name, value := range want {
		if keys[name] != value {
			problems = append(problems, fmt.Sprintf("UpdateChecker.cpp's %s is %q, want %q", name, keys[name], value))
		}
	}
	saves := regexp.MustCompile(`SaveAppPref\(\s*([^,\s]+)\s*,`).FindAllStringSubmatch(checker, -1)
	if len(saves) == 0 {
		problems = append(problems, "UpdateChecker.cpp saves no preference: this check no longer reads its call sites")
	}
	for _, save := range saves {
		if _, ok := want[save[1]]; !ok {
			problems = append(problems, "UpdateChecker.cpp saves the preference "+save[1]+
				": an offer, or Later, kept across launches")
		}
	}
	for where, opener := range map[string]string{
		"UpdateChecker::RunCheck":  offerRunCheck,
		"UpdateChecker::RunApply":  offerRunApply,
		"UpdateChecker::RunReveal": offerRunReveal,
		"UpdateChecker::Later":     "void UpdateChecker::Later() {",
		"UpdateChecker::CheckNow":  "void UpdateChecker::CheckNow() {",
	} {
		body := applyDefinition(checker, opener)
		if body == "" {
			problems = append(problems, "UpdateChecker.cpp no longer defines "+where)
		}
		for _, read := range []string{"LoadAppPrefs", "AppPrefsFile"} {
			if strings.Contains(body, read) {
				problems = append(problems, where+" reads "+read+": a saved value would take part in the offer")
			}
		}
	}
	sort.Strings(problems)
	return problems
}

// Later: the banner offers it beside its action while it offers a release and
// nothing is in flight; choosing it hides that release, and only that one,
// until the next launch or a check the user asks for. The release's code is
// kept in a member that starts at 0 and is written in three places (Later
// sets it; a check the user asks for and a change of channel clear it), and
// never saved. A timed check that finds the release again leaves the banner
// closed, and does not leave another release on it that the feed no longer
// offers; dismissing a report about the hidden release leaves it closed too.
// The two places that put a release on the banner as an offer both ask
// HiddenByLater first.
func checkTrayLater(checker, header, window, connect string) []string {
	var problems []string
	if !strings.Contains(header, "std::uint64_t laterCode_ = 0;") {
		problems = append(problems, "UpdateChecker.h no longer starts every launch with no release hidden by Later")
	}
	later := applyDefinition(checker, "void UpdateChecker::Later() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::Later", later,
		regexp.QuoteMeta("if (!OffersLater(s)) return;"),
		regexp.QuoteMeta("laterCode_ = s.code;"),
		regexp.QuoteMeta("s.phase = Phase::None;"),
		regexp.QuoteMeta("s.code = 0;"))...)
	offers := applyDefinition(checker, "bool UpdateChecker::OffersLater(Snapshot const& snapshot) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::OffersLater", offers,
		regexp.QuoteMeta("return snapshot.code != 0 &&"),
		regexp.QuoteMeta("(snapshot.phase == Phase::Available || snapshot.phase == Phase::Failed ||"),
		regexp.QuoteMeta("snapshot.phase == Phase::ManualInstall);"))...)
	for _, never := range []string{"Phase::Applying", "Phase::Result"} {
		if strings.Contains(offers, never) {
			problems = append(problems, "UpdateChecker::OffersLater offers Later in "+never+
				": an apply in flight and the helper's report are not Later's to hide")
		}
	}
	written := regexp.MustCompile(`laterCode_\s*=[^=][^;]*;`).FindAllString(checker, -1)
	if strings.Join(written, " ") != "laterCode_ = 0; laterCode_ = s.code; laterCode_ = 0;" {
		problems = append(problems, fmt.Sprintf("UpdateChecker.cpp writes laterCode_ as %q, want it cleared by a check the "+
			"user asks for, set in Later and cleared on a change of channel, and written nowhere else", written))
	}
	asked := applyDefinition(checker, "void UpdateChecker::CheckNow() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::CheckNow", asked,
		regexp.QuoteMeta("std::lock_guard lock(mutex_);"),
		regexp.QuoteMeta("checkRequested_ = true;"),
		regexp.QuoteMeta("laterCode_ = 0;"))...)
	changed := applyDefinition(checker, "void UpdateChecker::ChannelChanged() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::ChannelChanged", changed,
		regexp.QuoteMeta("offer_ = Offer{};"),
		regexp.QuoteMeta("laterCode_ = 0;"))...)
	// The timed checks are the worker's own and the ones an apply runs: none
	// of them goes through CheckNow, which would clear Later.
	for where, opener := range map[string]string{
		"UpdateChecker::WorkerLoop":          "void UpdateChecker::WorkerLoop() {",
		"UpdateChecker::RunApply":            offerRunApply,
		"UpdateChecker::RunReveal":           offerRunReveal,
		"UpdateChecker::SetAutoCheckEnabled": "void UpdateChecker::SetAutoCheckEnabled(bool on) {",
		"UpdateChecker::ChannelChanged":      "void UpdateChecker::ChannelChanged() {",
	} {
		if strings.Contains(applyDefinition(checker, opener), "CheckNow(") {
			problems = append(problems, where+" asks for its check through CheckNow, which clears Later: "+
				"only a check the user asks for shows a release Later hid")
		}
	}
	check := applyDefinition(checker, offerRunCheck)
	problems = append(problems, applyOrderProblems("UpdateChecker::RunCheck", check,
		regexp.QuoteMeta("offer_ = offer;"),
		regexp.QuoteMeta("if (update::HiddenByLater(laterCode_, offer.code)) {"),
		regexp.QuoteMeta("if (snapshot_.phase != Phase::Result && snapshot_.code != 0 &&"),
		regexp.QuoteMeta("snapshot_.code != offer.code) {"),
		regexp.QuoteMeta("snapshot_.phase = Phase::None;"),
		regexp.QuoteMeta("} else if (snapshot_.phase == Phase::None || snapshot_.code != offer.code) {"),
		regexp.QuoteMeta("snapshot_.phase = Phase::Available;"))...)
	dismiss := applyDefinition(checker, "void UpdateChecker::DismissResult() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::DismissResult", dismiss,
		regexp.QuoteMeta("if (offer_.code > version::kCode && !update::HiddenByLater(laterCode_, offer_.code)) {"),
		regexp.QuoteMeta("s.phase = Phase::Available;"))...)
	// Those two are the only places that put a release on the banner as an
	// offer: a third would be one Later does not reach. (A click in flight is
	// not an offer: StepAfterCheck drops it when Later hid its release.)
	if offered := regexp.MustCompile(`\bphase = Phase::Available;`).FindAllString(checker, -1); len(offered) != 2 {
		problems = append(problems, fmt.Sprintf("UpdateChecker.cpp puts an offer on the banner in %d places, want the two "+
			"that ask HiddenByLater first: the check and the dismissal of a report", len(offered)))
	}
	// The label is whatever the link shows; what is pinned is the link, its
	// place on the banner and what a click on it does.
	problems = append(problems, applyOrderProblems("MainWindow's update banner", window,
		regexp.QuoteMeta("UpdateBar().ActionButton(update);"),
		regexp.QuoteMeta("HyperlinkButton later;"),
		`later\.Content\(`,
		regexp.QuoteMeta("later.Visibility(Visibility::Collapsed);"),
		regexp.QuoteMeta("later.Click([](auto const&, auto const&) { urnw::pages::Updates().Later(); });"),
		regexp.QuoteMeta("UpdateBar().Content(later);"))...)
	banner := applyDefinition(connect, "void ConnectPage::ApplyUpdateChecker(urnw::UpdateChecker::Snapshot const& snap) {")
	problems = append(problems, applyOrderProblems("ConnectPage::ApplyUpdateChecker", banner,
		regexp.QuoteMeta("bar.IsClosable(urnw::UpdateChecker::OffersInstaller(snap));"),
		regexp.QuoteMeta("later.Visibility(urnw::UpdateChecker::OffersLater(snap)"),
		regexp.QuoteMeta("if (snap.phase == Phase::None) {"))...)
	return problems
}

// The body of the switch on a ClickStep that starts at `opener` in `source`,
// split by case: what each case's lines are.
func offerClickCases(source, opener string) map[string]string {
	cases := map[string]string{}
	start := strings.Index(source, opener)
	if start < 0 {
		return cases
	}
	body := source[start+len(opener):]
	// the switch closes at the first brace at its own indentation
	if end := strings.Index(body, "\n    }\n"); end >= 0 {
		body = body[:end]
	}
	const label = "case update::ClickStep::"
	for _, part := range strings.Split(body, label)[1:] {
		name, lines, found := strings.Cut(part, ":")
		if found {
			cases[name] = lines
		}
	}
	return cases
}

// A banner can be hours old when it is clicked, and its release withdrawn or
// overtaken since. So the click is taken under the lock that judged it, with
// the banner moved to a stage that offers no action and no Later, and when
// the offer is no longer fresh (ReleaseSelection.h OfferMayHaveChanged, from
// the Date header of the check behind it and this machine's steady clock, not
// its wall clock), the check runs again before anything else: on an
// installed copy before the download and the administrator prompt, and on
// every other path before an installer is shown, where nothing else asks
// GitHub. What stands once the check has returned is read under one lock and
// handed to StepAfterCheck (UpdateSchedule.h, run by update_release_test.go),
// and only its Proceed goes on. "Show file" asks first too. After the helper
// refuses a release as no longer offered, the check runs once more, and a
// release that replaced it takes the banner instead of the refusal's report.
func checkTrayChecksAgainBeforeItActs(checker, window, connect string) []string {
	var problems []string
	check := applyDefinition(checker, offerRunCheck)
	problems = append(problems, applyOrderProblems("UpdateChecker::RunCheck", check,
		regexp.QuoteMeta("offer_ = offer;"),
		regexp.QuoteMeta("offerServerUnix_ = serverUnixSeconds;"),
		regexp.QuoteMeta("offerCheckedAt_ = steady_clock::now();"),
		regexp.QuoteMeta("return true;"))...)
	apply := applyDefinition(checker, offerRunApply)
	problems = append(problems, applyOrderProblems("UpdateChecker::RunApply", apply,
		regexp.QuoteMeta("std::lock_guard lock(mutex_);"),
		regexp.QuoteMeta("if (!actionable || offer_.code == 0 || feedGeneration_ != generation) return;"),
		regexp.QuoteMeta("if (snapshot_.code != offer_.code) return;"),
		regexp.QuoteMeta("offer = offer_;"),
		regexp.QuoteMeta("std::chrono::duration_cast<std::chrono::seconds>(steady_clock::now() - offerCheckedAt_)"),
		regexp.QuoteMeta("stale = update::OfferMayHaveChanged(update::kOfficialFeed, offerServerUnix_, sinceCheck);"),
		regexp.QuoteMeta("snapshot_.phase = Phase::Applying;"),
		regexp.QuoteMeta("snapshot_.stage = stale ? Stage::Checking : Stage::Downloading;"),
		regexp.QuoteMeta("claimed = snapshot_;"),
		regexp.QuoteMeta("const bool viaHelper = installed_ && !manual;"),
		regexp.QuoteMeta("if (stale) {"),
		regexp.QuoteMeta("held = viaHelper && steady_clock::now() < holdUntil_;"),
		regexp.QuoteMeta("FetchUrl("),
		regexp.QuoteMeta("RevealInExplorer(msiW);"),
		regexp.QuoteMeta("LaunchUpdateHelper("))...)
	// The check of an offer that is no longer fresh, read on its own: the
	// refusal's check further down hands StepAfterCheck the same fields.
	stale := applyDefinitionUntil(apply, "  if (stale) {\n", "\n  }\n")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunApply's check of an old offer", stale,
		regexp.QuoteMeta("const bool checked = RunCheck(generation);"),
		regexp.QuoteMeta("std::lock_guard lock(mutex_);"),
		regexp.QuoteMeta("applyRequested_ = false;"),
		regexp.QuoteMeta("manualRequested_ = false;"),
		regexp.QuoteMeta("offered = offer_;"),
		regexp.QuoteMeta("step = update::StepAfterCheck("),
		regexp.QuoteMeta("{.clickedCode = offer.code,"),
		regexp.QuoteMeta(".sameFeed = feedGeneration_ == generation,"),
		regexp.QuoteMeta(".checked = checked,"),
		regexp.QuoteMeta(".offeredCode = offer_.code,"),
		regexp.QuoteMeta(".bannerHolds = snapshot_.phase == Phase::Applying && snapshot_.code == offer.code,"),
		regexp.QuoteMeta(".laterCode = laterCode_,"),
		regexp.QuoteMeta(".viaHelper = viaHelper});"),
		regexp.QuoteMeta("switch (step) {"))...)
	// The click is judged and taken under one lock: between the test of the
	// banner and the stage that takes its action away, the lock is not let go.
	if from := strings.Index(apply, "const bool actionable ="); from < 0 {
		problems = append(problems, "UpdateChecker::RunApply no longer judges whether the banner offers its action")
	} else if to := strings.Index(apply[from:], "claimed = snapshot_;"); to >= 0 {
		taken := apply[from : from+to]
		if strings.Contains(taken, "\n  }\n") || strings.Contains(taken, "lock_guard") {
			problems = append(problems, "UpdateChecker::RunApply lets go of the lock between judging the click and taking "+
				"the banner's action away: Later chosen in between would be undone by the click")
		}
	}
	// Whether to check again does not depend on who installs: the question
	// is asked before the path is chosen.
	if strings.Contains(apply, "viaHelper && update::OfferMayHaveChanged") ||
		strings.Contains(apply, "installed_ && update::OfferMayHaveChanged") {
		problems = append(problems, "UpdateChecker::RunApply checks again on the helper's path only: an installer shown "+
			"for the user to run is the path nothing else asks GitHub on")
	}
	if from := strings.Index(apply, "const std::int64_t sinceCheck ="); from < 0 {
		problems = append(problems, "UpdateChecker::RunApply no longer measures the time since the offer's check")
	} else if to := strings.Index(apply[from:], "RunCheck(generation);"); to >= 0 {
		for _, wall := range []string{"NowUnixSeconds", "system_clock", "GetSystemTime", "time("} {
			if strings.Contains(apply[from:from+to], wall) {
				problems = append(problems, "UpdateChecker::RunApply measures the offer's age with "+wall+
					": the day is GitHub's, carried by the steady clock, not this machine's date")
			}
		}
	}
	// Only Proceed goes on, for the release the list gives now; every other
	// answer returns.
	cases := offerClickCases(apply, "switch (step) {")
	for _, name := range []string{"Proceed", "Replaced", "Withdrawn", "Unconfirmed", "Dropped"} {
		lines, ok := cases[name]
		switch {
		case !ok:
			problems = append(problems, "UpdateChecker::RunApply's switch on the click has no case for "+name)
		case name == "Proceed":
			if !strings.Contains(lines, "offer = offered;") || !strings.Contains(lines, "break;") ||
				strings.Contains(lines, "return;") {
				problems = append(problems, "UpdateChecker::RunApply's Proceed does not go on with the release as the list gives it now")
			}
		default:
			if !strings.Contains(lines, "return;") || strings.Contains(lines, "break;") ||
				strings.Contains(lines, "offer = ") {
				problems = append(problems, "UpdateChecker::RunApply goes on after "+name+
					": only a click on the release the feed still offers starts anything")
			}
		}
	}
	if len(cases) != 5 {
		problems = append(problems, fmt.Sprintf("UpdateChecker::RunApply's switch on the click has %d cases, want the five of ClickStep", len(cases)))
	}
	if !strings.Contains(cases["Replaced"], "NoteReplaced(generation, offer.version);") {
		problems = append(problems, "UpdateChecker::RunApply does not say on the banner which release the new offer replaced")
	}
	if !strings.Contains(cases["Unconfirmed"], "fail(Failure::Unconfirmed);") {
		problems = append(problems, "UpdateChecker::RunApply does not say that GitHub could not be asked")
	}
	if count := strings.Count(apply, "offer = offered;"); count != 1 {
		problems = append(problems, fmt.Sprintf("UpdateChecker::RunApply takes the offer as it stands after the check %d times, want once, under Proceed", count))
	}
	// The offer is read from the checker once, under the lock that judges the
	// click: read again after the check, it could be another release's, and
	// that release would be installed on a click the user made on this one.
	if count := strings.Count(apply, "offer = offer_;"); count != 1 {
		problems = append(problems, fmt.Sprintf("UpdateChecker::RunApply reads the checker's offer %d times, want once, as the click is taken", count))
	}

	// "Show file" shows an installer again: it asks first too.
	reveal := applyDefinition(checker, offerRunReveal)
	problems = append(problems, applyOrderProblems("UpdateChecker::RunReveal", reveal,
		regexp.QuoteMeta("std::chrono::duration_cast<std::chrono::seconds>(steady_clock::now() - offerCheckedAt_)"),
		regexp.QuoteMeta("stale = !asked &&"),
		regexp.QuoteMeta("update::OfferMayHaveChanged(update::kOfficialFeed, offerServerUnix_, sinceCheck);"),
		regexp.QuoteMeta("if (!stale) break;"),
		regexp.QuoteMeta("if (!RunCheck(generation)) {"),
		regexp.QuoteMeta("s.failure = Failure::Unconfirmed;"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta("const std::string actual = Sha256File(installer);"),
		regexp.QuoteMeta("RevealInExplorer(installer);"))...)
	// the banner is read again after the check: the loop comes back to the
	// test it began with
	if !regexp.MustCompile(`for \(bool asked = false;; asked = true\) \{\s*bool stale = false;\s*\{\s*std::lock_guard lock\(mutex_\);\s*if \(snapshot_\.phase != Phase::ManualInstall`).MatchString(reveal) {
		problems = append(problems, "UpdateChecker::RunReveal does not read the banner again after its check")
	}

	// After the helper refused the release as no longer offered.
	problems = append(problems, applyOrderProblems("UpdateChecker::RunApply", apply,
		regexp.QuoteMeta("LaunchUpdateHelper("),
		regexp.QuoteMeta("if (ended == static_cast<std::int64_t>(update::Refusal::NotOffered)) {"),
		regexp.QuoteMeta("Mutate([this, &result](Snapshot& s) {"))...)
	refused := applyDefinitionUntil(apply, "  if (ended == static_cast<std::int64_t>(update::Refusal::NotOffered)) {\n", "\n  }\n")
	problems = append(problems, applyOrderProblems("UpdateChecker::RunApply's check after a refusal", refused,
		regexp.QuoteMeta("const bool checked = RunCheck(generation);"),
		regexp.QuoteMeta("std::lock_guard lock(mutex_);"),
		regexp.QuoteMeta("step = update::StepAfterCheck("),
		regexp.QuoteMeta("{.clickedCode = offer.code,"),
		regexp.QuoteMeta(".sameFeed = feedGeneration_ == generation,"),
		regexp.QuoteMeta(".checked = checked,"),
		regexp.QuoteMeta(".offeredCode = offer_.code,"),
		regexp.QuoteMeta(".viaHelper = true});"),
		regexp.QuoteMeta("if (step == update::ClickStep::Replaced) {"),
		regexp.QuoteMeta("SaveAppPref(kResultSeenPrefKey, Narrow(result.finishedUtc));"),
		regexp.QuoteMeta("NoteReplaced(generation, offer.version);"),
		regexp.QuoteMeta("return;"))...)
	note := applyDefinition(checker, "void UpdateChecker::NoteReplaced(std::uint64_t generation, std::wstring const& replaced) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::NoteReplaced", note,
		regexp.QuoteMeta("MutateFor(generation, [this, &replaced](Snapshot& s) {"),
		regexp.QuoteMeta("if (s.phase != Phase::Available || s.code == 0 || s.code != offer_.code) return;"),
		regexp.QuoteMeta("s.replacedVersion = replaced;"),
		regexp.QuoteMeta("s.replacedByCode = s.code;"))...)
	says := applyDefinition(checker, "bool UpdateChecker::SaysReplaced(Snapshot const& snapshot) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::SaysReplaced", says,
		regexp.QuoteMeta("return snapshot.phase == Phase::Available && snapshot.code != 0 &&"),
		regexp.QuoteMeta("snapshot.replacedByCode == snapshot.code && !snapshot.replacedVersion.empty();"))...)

	// What the banner and its button do with the new stage and the new failure.
	banner := applyDefinition(connect, "void ConnectPage::ApplyUpdateChecker(urnw::UpdateChecker::Snapshot const& snap) {")
	problems = append(problems, applyOrderProblems("ConnectPage::ApplyUpdateChecker", banner,
		regexp.QuoteMeta("case Phase::Available:"),
		regexp.QuoteMeta("if (urnw::UpdateChecker::SaysReplaced(snap)) {"),
		regexp.QuoteMeta("snap.replacedVersion) +"),
		regexp.QuoteMeta("case Phase::Applying:"),
		regexp.QuoteMeta("enabled = false;"),
		regexp.QuoteMeta("case Stage::Checking:"),
		regexp.QuoteMeta("case Stage::Downloading:"),
		regexp.QuoteMeta("case Failure::Unconfirmed:"),
		regexp.QuoteMeta(`action = winrt::hstring{L"Download the installer"};`))...)
	action := applyDefinition(window, "void MainWindow::OnUpdateBannerAction() {")
	problems = append(problems, applyOrderProblems("MainWindow::OnUpdateBannerAction", action,
		regexp.QuoteMeta("case Phase::Failed:"),
		regexp.QuoteMeta("if (updateSnapshot_.phase == Phase::Failed &&"),
		regexp.QuoteMeta("updateSnapshot_.failure == Failure::Unconfirmed) {"),
		regexp.QuoteMeta("urnw::pages::Updates().ShowInstaller();"),
		regexp.QuoteMeta("break;"),
		regexp.QuoteMeta("urnw::pages::Updates().BeginApply();"))...)
	return problems
}

// The tray app's downloads sit in the user's folder, one folder per release
// tag, and the only one worth keeping is the offered release's. A check that
// reached GitHub removes the rest, so the installer of a release that was
// withdrawn or overtaken is not left there to be run. A dev build, which is
// offered nothing, removes nothing: an installed copy of the same user keeps
// its downloads in the same folder. Only folders named as a release tag go,
// and never anything under the install folder, which is the helper's.
func checkTrayKeepsOnlyTheOfferedDownload(checker string) []string {
	var problems []string
	check := applyDefinition(checker, offerRunCheck)
	problems = append(problems, applyOrderProblems("UpdateChecker::RunCheck", check,
		regexp.QuoteMeta("if (feedGeneration_ != generation) {"),
		regexp.QuoteMeta("return false;"),
		regexp.QuoteMeta("offer_ = offer;"),
		regexp.QuoteMeta("offer_ = Offer{};"),
		regexp.QuoteMeta("keepTag = offer_.tag;"),
		regexp.QuoteMeta("copy = snapshot_;"),
		regexp.QuoteMeta("if (version::kCode != 0) RemoveOtherDownloads(keepTag);"),
		regexp.QuoteMeta("return true;"))...)
	if count := strings.Count(checker, "RemoveOtherDownloads("); count != 2 {
		problems = append(problems, fmt.Sprintf("UpdateChecker.cpp names RemoveOtherDownloads %d times, want its "+
			"definition and the one call, after a check that reached GitHub", count))
	}
	remove := applyDefinition(checker, "void UpdateChecker::RemoveOtherDownloads(std::wstring const& keepTag) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::RemoveOtherDownloads", remove,
		regexp.QuoteMeta("for (auto const& entry : fs::directory_iterator(UpdatesDir(), ec)) {"),
		regexp.QuoteMeta("if (!entry.is_directory(ec)) continue;"),
		regexp.QuoteMeta("if (name == keepTag || version::ParseReleaseCode(Narrow(name)) == 0) continue;"),
		regexp.QuoteMeta("fs::remove_all(entry.path(), removeError);"))...)
	for _, other := range []string{"installFolder_", "OwnExePath", "remove_all(UpdatesDir()"} {
		if strings.Contains(remove, other) {
			problems = append(problems, "UpdateChecker::RemoveOtherDownloads reaches "+other+
				": it removes release folders from the tray app's own updates folder and nothing else")
		}
	}
	return problems
}

// A report that a new offer takes off the banner is read: marked as seen, so
// it does not come back at every launch for the seconds before that launch's
// own check.
func checkTrayMarksAReplacedReportRead(checker string) []string {
	check := applyDefinition(checker, offerRunCheck)
	return applyOrderProblems("UpdateChecker::RunCheck", check,
		regexp.QuoteMeta("std::wstring replacedReport;"),
		regexp.QuoteMeta("} else if (snapshot_.phase == Phase::None || snapshot_.code != offer.code) {"),
		regexp.QuoteMeta("if (snapshot_.phase == Phase::Result) replacedReport = snapshot_.result.finishedUtc;"),
		regexp.QuoteMeta("snapshot_.phase = Phase::Available;"),
		regexp.QuoteMeta("copy = snapshot_;"),
		regexp.QuoteMeta("if (!replacedReport.empty()) SaveAppPref(kResultSeenPrefKey, Narrow(replacedReport));"))
}

// The developer line names the newest release the feed holds back, with the
// earliest time it can be offered. It promises nothing: a later build that
// reaches its day at the same midnight is offered instead. It is one format
// string with two placeholders, so a store key can take its place as written.
// It shows after a check that reached GitHub, and not on a dev build, which
// is offered nothing, nor after a failed check, when the time would be an
// earlier check's.
func checkDeveloperLineNamesTheReleaseHeldBack(checker, developer string) []string {
	var problems []string
	check := applyDefinition(checker, offerRunCheck)
	problems = append(problems, applyOrderProblems("UpdateChecker::RunCheck", check,
		regexp.QuoteMeta("snapshot_.offeredCode = offer_.code;"),
		regexp.QuoteMeta("const bool waiting = version::kCode != 0 && sel.waitingCode > version::kCode &&"),
		regexp.QuoteMeta("sel.waitingCode > offer_.code;"),
		regexp.QuoteMeta("snapshot_.waitingCode = waiting ? sel.waitingCode : 0;"),
		regexp.QuoteMeta("snapshot_.waitingFromUnix = waiting ? sel.waitingFromUnixSeconds : 0;"))...)
	names := applyDefinition(checker, "bool UpdateChecker::NamesWaiting(Snapshot const& snapshot) {")
	problems = append(problems, applyOrderProblems("UpdateChecker::NamesWaiting", names,
		regexp.QuoteMeta("return snapshot.waitingCode != 0 && (snapshot.lastCheck == CheckOutcome::NoUpdate ||"),
		regexp.QuoteMeta("snapshot.lastCheck == CheckOutcome::UpdateFound);"))...)
	for _, never := range []string{"CheckOutcome::Failed", "CheckOutcome::InFlight", "CheckOutcome::DevBuild", "CheckOutcome::NeverRan"} {
		if strings.Contains(names, never) {
			problems = append(problems, "UpdateChecker::NamesWaiting names the release held back after "+never)
		}
	}
	line := applyDefinition(developer, "void DeveloperPage::ApplyUpdateCheck(UpdateChecker::Snapshot const& snap) {")
	problems = append(problems, applyOrderProblems("DeveloperPage::ApplyUpdateCheck", line,
		regexp.QuoteMeta("if (UpdateChecker::NamesWaiting(snap)) {"),
		regexp.QuoteMeta(`text += std::format(L"v{0} is published and is offered no earlier than {1}",`),
		regexp.QuoteMeta("snap.waitingVersion, UpdateChecker::LocalDateTime(snap.waitingFromUnix));"),
		regexp.QuoteMeta("updateCheckText_.Text(hstring{text});"))...)
	if strings.Contains(line, "is offered from") {
		problems = append(problems, "DeveloperPage::ApplyUpdateCheck says when the release held back is offered: "+
			"that is a promise the feed does not keep when a later build overtakes it")
	}
	if strings.Contains(line, "snap.waitingCode != 0") {
		problems = append(problems, "DeveloperPage::ApplyUpdateCheck decides for itself when to name the release held back: "+
			"NamesWaiting does, from the last check's outcome")
	}
	return problems
}

// A UI Automation driver finds the update banner, its action, its Later link
// and the developer screen's check by these ids, as the insufficient-balance
// driver finds its controls (acceptance_automation_ids_test.go). A rename
// here must move any such driver with it.
func checkUpdateBannerAutomationIds(xaml map[string]string, window, developer string) []string {
	var problems []string
	if got := xaml["UpdateBar"]; got != "acceptance.update.banner" {
		problems = append(problems, fmt.Sprintf("UpdateBar: AutomationProperties.AutomationId = %q, want %q", got, "acceptance.update.banner"))
	}
	problems = append(problems, applyOrderProblems("MainWindow's update banner", window,
		regexp.QuoteMeta(`Automation::AutomationProperties::SetAutomationId(update, L"acceptance.update.action");`),
		regexp.QuoteMeta("UpdateBar().ActionButton(update);"),
		regexp.QuoteMeta(`Automation::AutomationProperties::SetAutomationId(later, L"acceptance.update.later");`),
		regexp.QuoteMeta("UpdateBar().Content(later);"))...)
	problems = append(problems, applyOrderProblems("DeveloperPage's update check", developer,
		regexp.QuoteMeta("Automation::AutomationProperties::SetAutomationId(checkUpdates,"),
		regexp.QuoteMeta(`L"acceptance.update.check-now");`),
		regexp.QuoteMeta("urnw::pages::Updates().CheckNow();"),
		regexp.QuoteMeta("Automation::AutomationProperties::SetAutomationId(updateCheckText_,"),
		regexp.QuoteMeta(`L"acceptance.update.check-line");`))...)
	for source, ids := range map[string][]string{
		window:    {`L"acceptance.update.action"`, `L"acceptance.update.later"`},
		developer: {`L"acceptance.update.check-now"`, `L"acceptance.update.check-line"`},
	} {
		for _, id := range ids {
			if strings.Count(source, id) != 1 {
				problems = append(problems, "want exactly one "+id)
			}
		}
	}
	return problems
}

// The README's Download section sends people to the releases the updater
// polls: the feed's own repository, by the owner and name compiled in. It
// says what the updater offers without promising every release, that a hand
// install should prefer a release that has been out for a day (the newest
// one can be minutes old, and no soak covers a hand install), and which
// releases do not update themselves from this feed, by a date and not by a
// tag that the next release would make wrong.
func checkReadmeNamesTheFeed(readme, selection string) []string {
	var problems []string
	feed := applyDefinitionUntil(selection, "inline constexpr Feed kOfficialFeed{", "\n};\n")
	owner := regexp.MustCompile(`\.owner = "([^"]+)",`).FindStringSubmatch(feed)
	repo := regexp.MustCompile(`\.repo = "([^"]+)",`).FindStringSubmatch(feed)
	if owner == nil || repo == nil {
		return append(problems, "ReleaseSelection.h's kOfficialFeed no longer names its owner and repo")
	}
	start := strings.Index(readme, "\n## Download\n")
	if start < 0 {
		return append(problems, "README.md has no Download section")
	}
	section := readme[start+1:]
	if end := strings.Index(section[1:], "\n## "); end >= 0 {
		section = section[:end+1]
	}
	latest := "https://github.com/" + owner[1] + "/" + repo[1] + "/releases/latest"
	if !strings.Contains(section, latest) {
		problems = append(problems, "README.md's Download section does not link "+latest+", the feed the updater polls")
	}
	if !strings.Contains(section, "kOfficialFeed") {
		problems = append(problems, "README.md's Download section no longer names kOfficialFeed")
	}
	for _, link := range regexp.MustCompile(`github\.com/([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)/releases`).FindAllStringSubmatch(readme, -1) {
		if link[1] != owner[1]+"/"+repo[1] {
			problems = append(problems, "README.md sends people to "+link[1]+"'s releases, which the updater does not poll")
		}
	}
	// read as one line, so a sentence may wrap anywhere
	prose := strings.Join(strings.Fields(section), " ")
	for what, pattern := range map[string]string{
		"that a hand install should prefer a release that has been out for a day": `can be minutes old\. When you install by hand, prefer the newest one that has been out for a day`,
		"that the updater offers the newest release that has been out for a day":  `It offers the newest release that has been out, unchanged, for a day`,
		"which releases do not update themselves, by a date":                      `A release published on or before \d{4}-\d{2}-\d{2} does not look for updates here`,
		"that one install by hand of a later release is what it takes":            `Install one published after that by hand once`,
	} {
		if !regexp.MustCompile(pattern).MatchString(prose) {
			problems = append(problems, "README.md's Download section does not say "+what)
		}
	}
	if regexp.MustCompile(`Running v\d{4}\.\d+\.\d+-\d+ or an earlier release`).MatchString(prose) {
		problems = append(problems, "README.md's Download section names the last release that does not update itself by its "+
			"tag: a release cut before this is merged makes it wrong")
	}
	if strings.Contains(prose, "at most once a day") {
		problems = append(problems, "README.md's Download section says the offer changes at most once a day: "+
			"it moves on when the UTC day does, and a withdrawn release leaves it at once")
	}
	sort.Strings(problems)
	return problems
}

// The text from `opener` through the next `terminator`, or "" when absent.
func applyDefinitionUntil(source, opener, terminator string) string {
	start := strings.Index(source, opener)
	if start < 0 {
		return ""
	}
	end := strings.Index(source[start:], terminator)
	if end < 0 {
		return ""
	}
	return source[start : start+end+len(terminator)]
}

func readRepositoryFile(t *testing.T, name string) string {
	t.Helper()
	data, err := os.ReadFile(filepath.Join(repositoryRoot(t), name))
	if err != nil {
		t.Fatal(err)
	}
	return string(data)
}

func TestUpdateOfferBothSidesAskForTheSameList(t *testing.T) {
	reportProblems(t, checkBothAskForTheSameList(stripComments(readAppSource(t, "UpdateChecker.cpp")),
		updaterSources(t)["ApplyUpdate.cpp"], stripComments(readCommonSource(t, "ReleaseSelection.h"))))
}

func TestUpdateOfferTheListIsJudgedByItsOwnDate(t *testing.T) {
	reportProblems(t, checkTheListIsJudgedByItsOwnDate(stripComments(readAppSource(t, "UpdateChecker.cpp")),
		updaterSources(t)["ApplyUpdate.cpp"]))
}

func TestUpdateOfferNothingSavedDecidesIt(t *testing.T) {
	reportProblems(t, checkNothingSavedDecidesTheOffer(stripComments(readAppSource(t, "UpdateChecker.cpp"))))
}

func TestUpdateOfferLaterLastsUntilTheNextLaunch(t *testing.T) {
	reportProblems(t, checkTrayLater(stripComments(readAppSource(t, "UpdateChecker.cpp")),
		stripComments(readAppSource(t, "UpdateChecker.h")), stripComments(readAppSource(t, "MainWindow.xaml.cpp")),
		stripComments(readAppSource(t, "ConnectPage.cpp"))))
}

func TestUpdateOfferTheTrayChecksAgainBeforeItActs(t *testing.T) {
	reportProblems(t, checkTrayChecksAgainBeforeItActs(stripComments(readAppSource(t, "UpdateChecker.cpp")),
		stripComments(readAppSource(t, "MainWindow.xaml.cpp")), stripComments(readAppSource(t, "ConnectPage.cpp"))))
}

func TestUpdateOfferTheTrayKeepsOnlyTheOfferedDownload(t *testing.T) {
	reportProblems(t, checkTrayKeepsOnlyTheOfferedDownload(stripComments(readAppSource(t, "UpdateChecker.cpp"))))
}

func TestUpdateOfferAReplacedReportIsRead(t *testing.T) {
	reportProblems(t, checkTrayMarksAReplacedReportRead(stripComments(readAppSource(t, "UpdateChecker.cpp"))))
}

func TestUpdateOfferTheDeveloperLineNamesTheReleaseHeldBack(t *testing.T) {
	reportProblems(t, checkDeveloperLineNamesTheReleaseHeldBack(stripComments(readAppSource(t, "UpdateChecker.cpp")),
		stripComments(readAppSource(t, "DeveloperPage.cpp"))))
}

func TestUpdateOfferBannerAutomationIds(t *testing.T) {
	xaml := xamlAutomationIds(t, filepath.Join(repositoryRoot(t), "app", "src", "App", "MainWindow.xaml"))
	reportProblems(t, checkUpdateBannerAutomationIds(xaml, stripComments(readAppSource(t, "MainWindow.xaml.cpp")),
		stripComments(readAppSource(t, "DeveloperPage.cpp"))))
}

func TestUpdateOfferTheReadmeNamesTheFeed(t *testing.T) {
	reportProblems(t, checkReadmeNamesTheFeed(readRepositoryFile(t, "README.md"),
		stripComments(readCommonSource(t, "ReleaseSelection.h"))))
}

// Each check above fails on a source that drops what it pins, and for that
// reason: every control names the problem it must be reported by.
func TestUpdateOfferWiringRejectsWeakerSources(t *testing.T) {
	checker := stripComments(readAppSource(t, "UpdateChecker.cpp"))
	header := stripComments(readAppSource(t, "UpdateChecker.h"))
	window := stripComments(readAppSource(t, "MainWindow.xaml.cpp"))
	connect := stripComments(readAppSource(t, "ConnectPage.cpp"))
	developer := stripComments(readAppSource(t, "DeveloperPage.cpp"))
	apply := updaterSources(t)["ApplyUpdate.cpp"]
	selection := stripComments(readCommonSource(t, "ReleaseSelection.h"))
	readme := readRepositoryFile(t, "README.md")
	xaml := xamlAutomationIds(t, filepath.Join(repositoryRoot(t), "app", "src", "App", "MainWindow.xaml"))
	replace := func(text, old, replacement string) string {
		if strings.Count(text, old) != 1 {
			t.Fatalf("negative control: %q is not in the source exactly once", old)
		}
		return strings.Replace(text, old, replacement, 1)
	}
	// Rewrites `old` inside the definition that starts at `opener`.
	inside := func(source, opener, old, replacement string) string {
		definition := applyDefinition(source, opener)
		if definition == "" {
			t.Fatalf("negative control: %q is not defined", opener)
		}
		return replace(source, definition, replace(definition, old, replacement))
	}
	within := func(opener, old, replacement string) string {
		return inside(checker, opener, old, replacement)
	}
	again := func(source string) []string { return checkTrayChecksAgainBeforeItActs(source, window, connect) }
	for _, tc := range []struct {
		name, want string
		check      func() []string
	}{
		{"the tray asks for a page of its own", "UpdateChecker.cpp spells a GitHub API URL of its own", func() []string {
			return checkBothAskForTheSameList(replace(checker, "const std::wstring url = Widen(update::ReleaseListUrl(feed));",
				`const std::wstring url = std::format(L"https://api.github.com/repositories/{}/releases?per_page=15", feed.numericRepoId);`),
				apply, selection)
		}},
		{"the helper asks for a page of its own", "ApplyUpdate.cpp spells a GitHub API URL of its own", func() []string {
			return checkBothAskForTheSameList(checker, replace(apply,
				"const std::wstring listUrl = WidenAscii(update::ReleaseListUrl(feed));",
				`const std::wstring listUrl = std::format(L"https://api.github.com/repositories/{}/releases?per_page=100", feed.numericRepoId);`),
				selection)
		}},
		{"the list asked for by the repository's name", "ReleaseListUrl is missing", func() []string {
			return checkBothAskForTheSameList(checker, apply, replace(selection,
				`return "https://api.github.com/repositories/" + std::to_string(feed.numericRepoId) +`,
				`return "https://api.github.com/repos/" + std::string(feed.owner) + "/" + std::string(feed.repo) +`))
		}},
		{"the tray polls another feed than the official one", "UpdateChecker::RunCheck is missing const update::Feed& feed = update::kOfficialFeed;", func() []string {
			return checkBothAskForTheSameList(replace(checker, "const update::Feed& feed = update::kOfficialFeed;",
				"const update::Feed& feed = *update::FeedById(Narrow(LoadAppPrefs().value(\"update_feed\", std::string{\"official\"})));"),
				apply, selection)
		}},
		{"a list without a Date header judged by this machine's clock", `UpdateChecker::RunCheck is missing if \(headers\.serverUnixSeconds == 0\)`, func() []string {
			return checkTheListIsJudgedByItsOwnDate(replace(checker,
				"  if (headers.serverUnixSeconds == 0) {\n    LogWarn(\"update: the release list had no Date header; nothing is offered from it\");\n    CheckFailed(generation);\n    return false;\n  }\n",
				""), apply)
		}},
		{"the release list dated by this machine's clock", "UpdateChecker::RunCheck is missing const std::int64_t serverUnixSeconds = headers", func() []string {
			return checkTheListIsJudgedByItsOwnDate(replace(checker,
				"const std::int64_t serverUnixSeconds = headers.serverUnixSeconds;",
				"const std::int64_t serverUnixSeconds = succeeded;"), apply)
		}},
		{"the list's date replaced after it is read", "gives the list's date a value 2 times", func() []string {
			return checkTheListIsJudgedByItsOwnDate(replace(checker,
				"const std::int64_t serverUnixSeconds = headers.serverUnixSeconds;",
				"std::int64_t serverUnixSeconds = headers.serverUnixSeconds;\n  if (serverUnixSeconds < succeeded) serverUnixSeconds = succeeded;"),
				apply)
		}},
		{"the helper dates the list by its own clock", "ApplyUpdate is missing update::SelectRelease", func() []string {
			return checkTheListIsJudgedByItsOwnDate(checker, replace(apply,
				"update::SelectRelease(*releases, kArch, feed, list.serverUnixSeconds);",
				"update::SelectRelease(*releases, kArch, feed, NowUnixSeconds());"))
		}},
		{"the helper takes a list without a date", `ApplyUpdate is missing if \(list\.serverUnixSeconds == 0\)`, func() []string {
			return checkTheListIsJudgedByItsOwnDate(checker, replace(apply, "if (list.serverUnixSeconds == 0) {", "if (false) {"))
		}},
		{"the helper installs whatever tag it is given", `ApplyUpdate is missing if \(!update::SelectionOffers`, func() []string {
			return checkTheListIsJudgedByItsOwnDate(checker, replace(apply,
				"if (!update::SelectionOffers(selection, tag, version::kCode)) {", "if (selection.code == 0) {"))
		}},
		{"the tray judges the soak again with its own date", "UpdateChecker.cpp reads HasSoaked(", func() []string {
			return checkTheListIsJudgedByItsOwnDate(replace(checker, "  Offer offer;\n  if (sel.code != 0 &&",
				"  Offer offer;\n  if (!update::HasSoaked(feed, NowUnixSeconds() - 86400, NowUnixSeconds())) return false;\n  if (sel.code != 0 &&"),
				apply)
		}},
		{"the helper works out a release's day for itself", "ApplyUpdate.cpp reads SoakStartOf(", func() []string {
			return checkTheListIsJudgedByItsOwnDate(checker, replace(apply,
				"  if (!update::SelectionOffers(selection, tag, version::kCode)) {",
				"  (void)update::SoakStartOf(code, {}, {}, {});\n  if (!update::SelectionOffers(selection, tag, version::kCode)) {"))
		}},
		{"the offer kept across launches", "update_offer_code", func() []string {
			return checkNothingSavedDecidesTheOffer(replace(checker, "      offer_ = offer;\n      offerServerUnix_ = serverUnixSeconds;\n",
				"      offer_ = offer;\n      SaveAppPref(\"update_offer_code\", offer.code);\n      offerServerUnix_ = serverUnixSeconds;\n"))
		}},
		{"Later kept across launches", "saves the preference kLaterPrefKey", func() []string {
			return checkNothingSavedDecidesTheOffer(replace(checker, "    laterCode_ = s.code;\n",
				"    laterCode_ = s.code;\n    SaveAppPref(kLaterPrefKey, laterCode_);\n"))
		}},
		{"a fourth preference", "kLastOfferPrefKey", func() []string {
			return checkNothingSavedDecidesTheOffer(replace(checker,
				"constexpr char kLastSuccessPrefKey[] = \"update_last_check_success\";",
				"constexpr char kLastSuccessPrefKey[] = \"update_last_check_success\";\nconstexpr char kLastOfferPrefKey[] = \"update_last_offer\";"))
		}},
		{"a saved value read by the check", "UpdateChecker::RunCheck reads LoadAppPrefs", func() []string {
			return checkNothingSavedDecidesTheOffer(within(offerRunCheck, "  bool held = false;\n",
				"  bool held = false;\n  const nlohmann::json saved = LoadAppPrefs();\n"))
		}},
		{"a saved value read before an installer is shown again", "UpdateChecker::RunReveal reads LoadAppPrefs", func() []string {
			return checkNothingSavedDecidesTheOffer(within(offerRunReveal, "  std::string digest;\n",
				"  std::string digest;\n  const nlohmann::json saved = LoadAppPrefs();\n"))
		}},
		{"Later restored at launch", "no longer starts every launch with no release hidden by Later", func() []string {
			return checkTrayLater(checker, replace(header, "std::uint64_t laterCode_ = 0;", "std::uint64_t laterCode_ = SavedLater();"),
				window, connect)
		}},
		{"Later that hides nothing", "UpdateChecker::Later is missing laterCode_ = s", func() []string {
			return checkTrayLater(replace(checker, "    laterCode_ = s.code;\n", ""), header, window, connect)
		}},
		{"Later offered while an update runs", "offers Later in Phase::Applying", func() []string {
			return checkTrayLater(replace(checker, "snapshot.phase == Phase::ManualInstall);",
				"snapshot.phase == Phase::ManualInstall || snapshot.phase == Phase::Applying);"), header, window, connect)
		}},
		{"Later on the helper's report", "offers Later in Phase::Result", func() []string {
			return checkTrayLater(replace(checker, "(snapshot.phase == Phase::Available || snapshot.phase == Phase::Failed ||",
				"(snapshot.phase == Phase::Available || snapshot.phase == Phase::Result ||"), header, window, connect)
		}},
		{"the next check puts the release back on the banner", `UpdateChecker::RunCheck is missing if \(update::HiddenByLater`, func() []string {
			return checkTrayLater(replace(checker, "if (update::HiddenByLater(laterCode_, offer.code)) {", "if (false) {"),
				header, window, connect)
		}},
		{"a release the feed no longer offers left on the banner behind Later", `UpdateChecker::RunCheck is missing snapshot_\.phase = Phase::None;`, func() []string {
			return checkTrayLater(within(offerRunCheck, "          snapshot_.phase = Phase::None;\n", ""), header, window, connect)
		}},
		{"a dismissed report puts the release back on the banner", "UpdateChecker::DismissResult is missing", func() []string {
			return checkTrayLater(replace(checker,
				"if (offer_.code > version::kCode && !update::HiddenByLater(laterCode_, offer_.code)) {",
				"if (offer_.code > version::kCode) {"), header, window, connect)
		}},
		{"a third place puts the offer back on the banner", "puts an offer on the banner in 3 places", func() []string {
			return checkTrayLater(replace(checker, "  if (update::KeepsPackage(ended)) fs::remove_all(dir, ec);\n",
				"  if (update::KeepsPackage(ended)) fs::remove_all(dir, ec);\n"+
					"  Mutate([](Snapshot& s) { s.phase = Phase::Available; });\n"), header, window, connect)
		}},
		{"Later written somewhere else", "laterCode_ = offer.code;", func() []string {
			return checkTrayLater(replace(checker, "      offer_ = offer;\n      offerServerUnix_ = serverUnixSeconds;\n",
				"      offer_ = offer;\n      laterCode_ = offer.code;\n      offerServerUnix_ = serverUnixSeconds;\n"),
				header, window, connect)
		}},
		{"Later carried over a change of channel", "UpdateChecker::ChannelChanged is missing laterCode_ = 0;", func() []string {
			return checkTrayLater(within("void UpdateChecker::ChannelChanged() {", "    laterCode_ = 0;\n", ""), header, window, connect)
		}},
		{"a check the user asks for that leaves the release hidden", "UpdateChecker::CheckNow is missing laterCode_ = 0;", func() []string {
			return checkTrayLater(within("void UpdateChecker::CheckNow() {", "    laterCode_ = 0;\n", ""), header, window, connect)
		}},
		{"a timed check that shows the release Later hid", "UpdateChecker::WorkerLoop asks for its check through CheckNow", func() []string {
			return checkTrayLater(within("void UpdateChecker::WorkerLoop() {", "        RunCheck(generation);\n",
				"        CheckNow();\n        RunCheck(generation);\n"), header, window, connect)
		}},
		{"a Later link that does nothing", "MainWindow's update banner is missing later", func() []string {
			return checkTrayLater(checker, header, replace(window,
				"later.Click([](auto const&, auto const&) { urnw::pages::Updates().Later(); });", ""), connect)
		}},
		{"a Later link that is not on the banner", "MainWindow's update banner is missing UpdateBar", func() []string {
			return checkTrayLater(checker, header, replace(window, "    UpdateBar().Content(later);\n", ""), connect)
		}},
		{"a Later link that is always shown", "ConnectPage::ApplyUpdateChecker is missing later", func() []string {
			return checkTrayLater(checker, header, window, replace(connect,
				"later.Visibility(urnw::UpdateChecker::OffersLater(snap)", "later.Visibility(true"))
		}},
		{"a Later link left up when the banner closes", `ConnectPage::ApplyUpdateChecker is missing if \(snap\.phase == Phase::None\)`, func() []string {
			moved := replace(connect, "  if (const auto later = bar.Content().try_as<winrt::Microsoft::UI::Xaml::UIElement>()) {\n"+
				"    later.Visibility(urnw::UpdateChecker::OffersLater(snap)\n"+
				"                         ? winrt::Microsoft::UI::Xaml::Visibility::Visible\n"+
				"                         : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);\n  }\n", "")
			moved = replace(moved, "  winrt::hstring title{urnw::Format(\"upd_available_title_version\", snap.version)};\n",
				"  if (const auto later = bar.Content().try_as<winrt::Microsoft::UI::Xaml::UIElement>()) {\n"+
					"    later.Visibility(urnw::UpdateChecker::OffersLater(snap)\n"+
					"                         ? winrt::Microsoft::UI::Xaml::Visibility::Visible\n"+
					"                         : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);\n  }\n"+
					"  winrt::hstring title{urnw::Format(\"upd_available_title_version\", snap.version)};\n")
			return checkTrayLater(checker, header, window, moved)
		}},
		{"a Later link whose label is a store key", "", func() []string {
			// not a weaker source: the pin must not hold the label's wording
			return checkTrayLater(checker, header, replace(window, `later.Content(winrt::box_value(L"Later"));`,
				`later.Content(LocBox("upd_later"));`), connect)
		}},
		{"no check before an old offer is acted on", `UpdateChecker::RunApply is missing if \(stale\)`, func() []string {
			return again(within(offerRunApply, "  if (stale) {\n", "  if (false) {\n"))
		}},
		{"a check on the helper's path only", "checks again on the helper's path only", func() []string {
			return again(within(offerRunApply, "stale = update::OfferMayHaveChanged(",
				"stale = installed_ && update::OfferMayHaveChanged("))
		}},
		{"Later can be chosen between the click and its claim", "lets go of the lock between judging the click", func() []string {
			return again(within(offerRunApply,
				"    stale = update::OfferMayHaveChanged(update::kOfficialFeed, offerServerUnix_, sinceCheck);\n",
				"    stale = update::OfferMayHaveChanged(update::kOfficialFeed, offerServerUnix_, sinceCheck);\n  }\n  {\n    std::lock_guard lock(mutex_);\n"))
		}},
		{"the banner keeps its button while the check runs", `UpdateChecker::RunApply is missing snapshot_\.stage = stale \? Stage::Checking`, func() []string {
			return again(within(offerRunApply, "    snapshot_.stage = stale ? Stage::Checking : Stage::Downloading;\n", ""))
		}},
		{"a click on a banner that shows another release", `UpdateChecker::RunApply is missing if \(snapshot_\.code != offer_\.code\) return;`, func() []string {
			return again(within(offerRunApply, "    if (snapshot_.code != offer_.code) return;\n", ""))
		}},
		{"an offer that changed is installed anyway", "goes on after Replaced", func() []string {
			return again(within(offerRunApply,
				"        NoteReplaced(generation, offer.version);\n        return;\n",
				"        NoteReplaced(generation, offer.version);\n        offer = offered;\n        break;\n"))
		}},
		{"the changed offer taken whatever the check said", "takes the offer as it stands after the check 2 times", func() []string {
			return again(within(offerRunApply, "    switch (step) {\n", "    offer = offered;\n    switch (step) {\n"))
		}},
		{"the offer read again once the check has returned", "reads the checker's offer 2 times", func() []string {
			return again(within(offerRunApply, "      offered = offer_;\n", "      offered = offer_;\n      offer = offer_;\n"))
		}},
		{"a withdrawn release goes on", "goes on after Withdrawn", func() []string {
			return again(within(offerRunApply,
				"        LogInfo(\"update: v{} is no longer offered; nothing was started\", Narrow(offer.version));\n        return;\n",
				"        LogInfo(\"update: v{} is no longer offered; nothing was started\", Narrow(offer.version));\n        break;\n"))
		}},
		{"an installer shown when GitHub could not be asked", "goes on after Unconfirmed", func() []string {
			return again(within(offerRunApply, "        fail(Failure::Unconfirmed);\n        return;\n", "        break;\n"))
		}},
		{"the click's own judgement instead of the tested one", "check of an old offer is missing step = update::StepAfterCheck", func() []string {
			return again(within(offerRunApply, "      offered = offer_;\n      step = update::StepAfterCheck(",
				"      offered = offer_;\n      step = offer_.code == offer.code ? update::ClickStep::Proceed : update::ClickStep::Replaced;\n      (void)update::ClickAfterCheck("))
		}},
		{"Later not handed to the click's judgement", `check of an old offer is missing \.laterCode = laterCode_,`, func() []string {
			return again(within(offerRunApply, "           .laterCode = laterCode_,\n           .viaHelper = viaHelper});",
				"           .laterCode = 0,\n           .viaHelper = viaHelper});"))
		}},
		{"a failed check taken for one that reached GitHub", `check of an old offer is missing \.checked = checked,`, func() []string {
			return again(within(offerRunApply, "           .checked = checked,\n           .offeredCode = offer_.code,\n           .bannerHolds = snapshot_.phase == Phase::Applying && snapshot_.code == offer.code,\n           .laterCode = laterCode_,\n           .viaHelper = viaHelper});",
				"           .checked = true,\n           .offeredCode = offer_.code,\n           .bannerHolds = snapshot_.phase == Phase::Applying && snapshot_.code == offer.code,\n           .laterCode = laterCode_,\n           .viaHelper = viaHelper});"))
		}},
		{"a click made during the check starts another update", "check of an old offer is missing applyRequested_ = false;", func() []string {
			return again(within(offerRunApply, "      applyRequested_ = false;\n      manualRequested_ = false;\n", ""))
		}},
		{"the offer's age taken after the download", "UpdateChecker::RunApply is missing held = viaHelper", func() []string {
			moved := within(offerRunApply, "  if (stale) {\n", "  if (false) {\n")
			return again(replace(moved, "  const std::wstring msiW = msiPath.wstring();\n",
				"  if (stale) {\n  }\n  const std::wstring msiW = msiPath.wstring();\n"))
		}},
		{"the offer's age by the wall clock", "measures the offer's age with NowUnixSeconds", func() []string {
			return again(within(offerRunApply,
				"        std::chrono::duration_cast<std::chrono::seconds>(steady_clock::now() - offerCheckedAt_)\n            .count();",
				"        NowUnixSeconds() - offerServerUnix_;"))
		}},
		{"the offer's date never recorded", "UpdateChecker::RunCheck is missing offerServerUnix_ = serverUnixSeconds;", func() []string {
			return again(replace(checker, "      offerServerUnix_ = serverUnixSeconds;\n", ""))
		}},
		{"a check that failed said to have reached GitHub", "UpdateChecker::RunCheck is missing return true;", func() []string {
			return again(within(offerRunCheck, "  if (version::kCode != 0) RemoveOtherDownloads(keepTag);\n  return true;\n",
				"  if (version::kCode != 0) RemoveOtherDownloads(keepTag);\n  return false;\n"))
		}},
		{"Show file without asking GitHub", `UpdateChecker::RunReveal is missing if \(!RunCheck\(generation\)\)`, func() []string {
			return again(within(offerRunReveal, "    if (!RunCheck(generation)) {", "    if (false) {"))
		}},
		{"Show file never taken for an old offer", "UpdateChecker::RunReveal is missing stale = !asked &&", func() []string {
			return again(within(offerRunReveal,
				"      stale = !asked &&\n              update::OfferMayHaveChanged(update::kOfficialFeed, offerServerUnix_, sinceCheck);\n",
				"      stale = false;\n"))
		}},
		{"Show file after a check, without reading the banner again", "does not read the banner again after its check", func() []string {
			return again(within(offerRunReveal, "  for (bool asked = false;; asked = true) {\n    bool stale = false;\n    {\n      std::lock_guard lock(mutex_);\n",
				"  for (bool asked = false;; asked = true) {\n    bool stale = false;\n    if (asked) break;\n    {\n      std::lock_guard lock(mutex_);\n"))
		}},
		{"the refusal's report shown over the release that replaced it", `check after a refusal is missing if \(step == update::ClickStep::Replaced\)`, func() []string {
			return again(within(offerRunApply, "    if (step == update::ClickStep::Replaced) {", "    if (false) {"))
		}},
		{"the refusal's report shown again at the next launch", `check after a refusal is missing SaveAppPref\(kResultSeenPrefKey, Narrow\(result\.finishedUtc\)\);`, func() []string {
			return again(within(offerRunApply, "        SaveAppPref(kResultSeenPrefKey, Narrow(result.finishedUtc));\n", ""))
		}},
		{"no check after the helper says the release is no longer offered", `check after a refusal is missing const bool checked = RunCheck\(generation\);`, func() []string {
			return again(within(offerRunApply,
				"  if (ended == static_cast<std::int64_t>(update::Refusal::NotOffered)) {\n    const bool checked = RunCheck(generation);\n",
				"  if (ended == static_cast<std::int64_t>(update::Refusal::NotOffered)) {\n    const bool checked = false;\n"))
		}},
		{"the note about a replaced release on any banner", "UpdateChecker::NoteReplaced is missing", func() []string {
			return again(replace(checker, "    if (s.phase != Phase::Available || s.code == 0 || s.code != offer_.code) return;\n", ""))
		}},
		{"the note kept for another release", "UpdateChecker::SaysReplaced is missing", func() []string {
			return again(replace(checker, "snapshot.replacedByCode == snapshot.code && !snapshot.replacedVersion.empty();",
				"!snapshot.replacedVersion.empty();"))
		}},
		{"a changed version with nothing said", `ConnectPage::ApplyUpdateChecker is missing if \(urnw::UpdateChecker::SaysReplaced\(snap\)\)`, func() []string {
			return checkTrayChecksAgainBeforeItActs(checker, window,
				replace(connect, "if (urnw::UpdateChecker::SaysReplaced(snap)) {", "if (false) {"))
		}},
		{"the check shown as starting the installer", "ConnectPage::ApplyUpdateChecker is missing case Stage::Checking:", func() []string {
			return checkTrayChecksAgainBeforeItActs(checker, window, replace(connect, "        case Stage::Checking:\n", ""))
		}},
		{"an installer that could not be confirmed retried through the helper", `MainWindow::OnUpdateBannerAction is missing updateSnapshot_\.failure == Failure::Unconfirmed\)`, func() []string {
			return checkTrayChecksAgainBeforeItActs(checker, replace(window,
				"          updateSnapshot_.failure == Failure::Unconfirmed) {", "          false) {"), connect)
		}},
		{"the offered release's download removed too", `UpdateChecker::RemoveOtherDownloads is missing if \(name == keepTag`, func() []string {
			return checkTrayKeepsOnlyTheOfferedDownload(replace(checker,
				"if (name == keepTag || version::ParseReleaseCode(Narrow(name)) == 0) continue;",
				"if (version::ParseReleaseCode(Narrow(name)) == 0) continue;"))
		}},
		{"any folder in the updates folder removed", `UpdateChecker::RemoveOtherDownloads is missing if \(name == keepTag`, func() []string {
			return checkTrayKeepsOnlyTheOfferedDownload(replace(checker,
				"if (name == keepTag || version::ParseReleaseCode(Narrow(name)) == 0) continue;",
				"if (name == keepTag) continue;"))
		}},
		{"downloads removed by a dev build", `UpdateChecker::RunCheck is missing if \(version::kCode != 0\) RemoveOtherDownloads`, func() []string {
			return checkTrayKeepsOnlyTheOfferedDownload(replace(checker,
				"if (version::kCode != 0) RemoveOtherDownloads(keepTag);", "RemoveOtherDownloads(keepTag);"))
		}},
		{"the download of a withdrawn release kept", `UpdateChecker::RunCheck is missing if \(version::kCode != 0\) RemoveOtherDownloads`, func() []string {
			return checkTrayKeepsOnlyTheOfferedDownload(replace(checker,
				"  if (version::kCode != 0) RemoveOtherDownloads(keepTag);\n", ""))
		}},
		{"downloads removed by a check that did not reach GitHub", "names RemoveOtherDownloads 3 times", func() []string {
			return checkTrayKeepsOnlyTheOfferedDownload(replace(checker,
				"    LogWarn(\"update: release list was not a JSON array\");\n",
				"    LogWarn(\"update: release list was not a JSON array\");\n    RemoveOtherDownloads({});\n"))
		}},
		{"the helper's own folder swept", "UpdateChecker::RemoveOtherDownloads reaches installFolder_", func() []string {
			return checkTrayKeepsOnlyTheOfferedDownload(within(
				"void UpdateChecker::RemoveOtherDownloads(std::wstring const& keepTag) {",
				"for (auto const& entry : fs::directory_iterator(UpdatesDir(), ec)) {",
				"for (auto const& entry : fs::directory_iterator(installFolder_ / L\"updates\", ec)) {"))
		}},
		{"a replaced report shown again at every launch", `UpdateChecker::RunCheck is missing if \(!replacedReport\.empty\(\)\) SaveAppPref`, func() []string {
			return checkTrayMarksAReplacedReportRead(replace(checker,
				"  if (!replacedReport.empty()) SaveAppPref(kResultSeenPrefKey, Narrow(replacedReport));\n", ""))
		}},
		{"a replaced report never noticed", `UpdateChecker::RunCheck is missing if \(snapshot_\.phase == Phase::Result\) replacedReport`, func() []string {
			return checkTrayMarksAReplacedReportRead(replace(checker,
				"        if (snapshot_.phase == Phase::Result) replacedReport = snapshot_.result.finishedUtc;\n", ""))
		}},
		{"the developer line's sentence deleted", `DeveloperPage::ApplyUpdateCheck is missing if \(UpdateChecker::NamesWaiting\(snap\)\)`, func() []string {
			block := applyDefinitionUntil(developer, "  if (UpdateChecker::NamesWaiting(snap)) {", "\n  }\n")
			return checkDeveloperLineNamesTheReleaseHeldBack(checker, replace(developer, block, ""))
		}},
		{"the developer line promising a time", "is offered no earlier than", func() []string {
			return checkDeveloperLineNamesTheReleaseHeldBack(checker, replace(developer,
				`L"v{0} is published and is offered no earlier than {1}"`, `L"v{0} is published and is offered from {1}"`))
		}},
		{"the developer line glued from pieces", `DeveloperPage::ApplyUpdateCheck is missing text \+= std::format`, func() []string {
			return checkDeveloperLineNamesTheReleaseHeldBack(checker, replace(developer,
				"    text += std::format(L\"v{0} is published and is offered no earlier than {1}\",\n"+
					"                        snap.waitingVersion, UpdateChecker::LocalDateTime(snap.waitingFromUnix));\n",
				"    text += L\"v\" + snap.waitingVersion + L\" is published and is offered no earlier than \" +\n"+
					"            UpdateChecker::LocalDateTime(snap.waitingFromUnix);\n"))
		}},
		{"the developer line deciding for itself", "decides for itself when to name the release held back", func() []string {
			return checkDeveloperLineNamesTheReleaseHeldBack(checker, replace(developer,
				"if (UpdateChecker::NamesWaiting(snap)) {", "if (UpdateChecker::NamesWaiting(snap) || snap.waitingCode != 0) {"))
		}},
		{"any held release named, on a dev build too", `UpdateChecker::RunCheck is missing const bool waiting = version::kCode != 0`, func() []string {
			return checkDeveloperLineNamesTheReleaseHeldBack(replace(checker,
				"const bool waiting = version::kCode != 0 && sel.waitingCode > version::kCode &&\n                         sel.waitingCode > offer_.code;",
				"const bool waiting = sel.waitingCode != 0;"), developer)
		}},
		{"a release older than the offer named as held back", `UpdateChecker::RunCheck is missing sel\.waitingCode > offer_\.code;`, func() []string {
			return checkDeveloperLineNamesTheReleaseHeldBack(replace(checker,
				"const bool waiting = version::kCode != 0 && sel.waitingCode > version::kCode &&\n                         sel.waitingCode > offer_.code;",
				"const bool waiting = version::kCode != 0 && sel.waitingCode > version::kCode &&\n                         sel.waitingCode != 0;"), developer)
		}},
		{"the release held back named after a failed check", "UpdateChecker::NamesWaiting is missing", func() []string {
			return checkDeveloperLineNamesTheReleaseHeldBack(replace(checker,
				"  return snapshot.waitingCode != 0 && (snapshot.lastCheck == CheckOutcome::NoUpdate ||\n"+
					"                                       snapshot.lastCheck == CheckOutcome::UpdateFound);",
				"  return snapshot.waitingCode != 0;"), developer)
		}},
		{"the release held back named unless a check failed", "names the release held back after CheckOutcome::Failed", func() []string {
			return checkDeveloperLineNamesTheReleaseHeldBack(replace(checker,
				"  return snapshot.waitingCode != 0 && (snapshot.lastCheck == CheckOutcome::NoUpdate ||\n"+
					"                                       snapshot.lastCheck == CheckOutcome::UpdateFound);",
				"  return snapshot.waitingCode != 0 && (snapshot.lastCheck == CheckOutcome::NoUpdate ||\n"+
					"                                       snapshot.lastCheck == CheckOutcome::UpdateFound) &&\n"+
					"         snapshot.lastCheck != CheckOutcome::Failed;"), developer)
		}},
		{"the banner without its automation id", "UpdateBar: AutomationProperties.AutomationId", func() []string {
			return checkUpdateBannerAutomationIds(map[string]string{}, window, developer)
		}},
		{"the Later link without its automation id", `MainWindow's update banner is missing Automation::AutomationProperties::SetAutomationId\(later`, func() []string {
			return checkUpdateBannerAutomationIds(xaml, replace(window,
				"    Automation::AutomationProperties::SetAutomationId(later, L\"acceptance.update.later\");\n", ""), developer)
		}},
		{"the banner's action without its automation id", `MainWindow's update banner is missing Automation::AutomationProperties::SetAutomationId\(update`, func() []string {
			return checkUpdateBannerAutomationIds(xaml, replace(window, `L"acceptance.update.action"`, `L"acceptance.update.button"`), developer)
		}},
		{"the developer screen's check without its automation id", "DeveloperPage's update check is missing", func() []string {
			return checkUpdateBannerAutomationIds(xaml, window, replace(developer, `L"acceptance.update.check-now"`, `L"acceptance.update.check"`))
		}},
		{"one automation id on two controls", `want exactly one L"acceptance.update.later"`, func() []string {
			return checkUpdateBannerAutomationIds(xaml, replace(window, `L"acceptance.update.action"`, `L"acceptance.update.later"`), developer)
		}},
		{"the README sends people to the app's own repository", "README.md sends people to urnetwork/windows's releases", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "https://github.com/urnetwork/build/releases/latest",
				"https://github.com/urnetwork/windows/releases"), selection)
		}},
		{"the README keeps a second releases page", "README.md sends people to example-user/urnetwork-windows's releases", func() []string {
			return checkReadmeNamesTheFeed(readme+"\nOlder builds: https://github.com/example-user/urnetwork-windows/releases\n", selection)
		}},
		{"the feed moves and the README stays", "does not link https://github.com/urnetwork/windows/releases/latest", func() []string {
			return checkReadmeNamesTheFeed(readme, replace(selection, `.repo = "build",`, `.repo = "windows",`))
		}},
		{"the README sends a hand install to the newest release", "does not say that a hand install should prefer a release that has been out for a day", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "prefer the\nnewest one that has been out for a day", "take the\nnewest one"), selection)
		}},
		{"the README promises every release", "does not say that the updater offers the newest release", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "It offers the newest\nrelease that has been out, unchanged, for a day",
				"It offers a release\nonce it has been out for a day"), selection)
		}},
		{"the README names the last release by its tag", "names the last release that does not update itself by its tag", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "A release published on or before 2026-10-08 does not look for updates here,",
				"Running v2026.10.8-1066946420 or an earlier release? It does not look for updates here,"), selection)
		}},
		{"the README says the offer changes at most once a day", "says the offer changes at most once a day", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "and moves on when the\nUTC day does", "and changes at most once a day"), selection)
		}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			problems := strings.Join(tc.check(), "\n")
			if tc.want == "" {
				if problems != "" {
					t.Fatalf("a source that is no weaker was reported:\n%s", problems)
				}
				return
			}
			if !strings.Contains(problems, tc.want) && !regexp.MustCompile(tc.want).MatchString(problems) {
				t.Fatalf("negative control was not detected by %q; the check reported:\n%s", tc.want, problems)
			}
		})
	}
}
