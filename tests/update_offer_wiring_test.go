// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"encoding/xml"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
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
	// The one renderer of both update banners, the connect screen's and the
	// sign-in screen's: what either banner says is pinned in its body.
	offerBannerRenderer = "void ConnectPage::RenderUpdateBanner(InfoBar const& bar, urnw::UpdateChecker::Snapshot const& snap) {"
)

// The tray app's check and the helper's own fetch ask for one URL, built in
// one place (ReleaseSelection.h ReleaseListUrl): the feed's repository by its
// id, and the same page of it. Two spellings could be two pages, and the
// helper would then refuse what the tray offered.
func checkBothAskForTheSameList(checker, apply, selection string) []string {
	var problems []string
	problems = append(problems, applyOrderProblems("UpdateChecker::RunCheck", applyDefinition(checker, offerRunCheck),
		regexp.QuoteMeta("const update::Feed& feed = *followed;"),
		regexp.QuoteMeta("const std::wstring url = Widen(update::ReleaseListUrl(feed));"),
		regexp.QuoteMeta("FetchUrl("),
		`url, L"application/vnd\.github\+json", kMaxJsonBytes,\s*false,`)...)
	problems = append(problems, applyOrderProblems("ApplyUpdate",
		applyDefinition(apply, "int ApplyUpdate(std::wstring_view tagArgument) {"),
		regexp.QuoteMeta("const update::Feed& feed = ChannelFeed(installFolder, log);"),
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
	// place on the banner and what a click on it does. Its place is beside the
	// banner's action, in one row that is the bar's content: in the bar's own
	// action slot the button has a row to itself and the content the next one
	// down, and at the size the window opens with (480 by 760) that row is cut
	// off at the bottom of the connect screen.
	problems = append(problems, applyOrderProblems("MainWindow's update banner", window,
		regexp.QuoteMeta("HyperlinkButton later;"),
		`later\.Content\(`,
		regexp.QuoteMeta("later.Visibility(Visibility::Collapsed);"),
		regexp.QuoteMeta("later.Click([](auto const&, auto const&) { urnw::pages::Updates().Later(); });"),
		regexp.QuoteMeta("StackPanel actions;"),
		regexp.QuoteMeta("actions.Orientation(winrt::Microsoft::UI::Xaml::Controls::Orientation::Horizontal);"),
		regexp.QuoteMeta("actions.Children().Append(update);"),
		regexp.QuoteMeta("actions.Children().Append(later);"),
		regexp.QuoteMeta("UpdateBar().Content(actions);"))...)
	if strings.Contains(window, "UpdateBar().ActionButton(") {
		problems = append(problems, "MainWindow puts a control in the update banner's own action slot: "+
			"Later would then be on a row of its own under it, which the window cuts off at the size it opens with")
	}
	banner := applyDefinition(connect, offerBannerRenderer)
	problems = append(problems, applyOrderProblems("ConnectPage::RenderUpdateBanner", banner,
		regexp.QuoteMeta("if (const auto row = bar.Content().try_as<winrt::Microsoft::UI::Xaml::Controls::Panel>()) {"),
		regexp.QuoteMeta("actionButton = controls.GetAt(0).try_as<winrt::Microsoft::UI::Xaml::Controls::Button>();"),
		regexp.QuoteMeta("later = controls.GetAt(1);"),
		regexp.QuoteMeta("bar.IsClosable(urnw::UpdateChecker::OffersInstaller(snap));"),
		regexp.QuoteMeta("later.Visibility(urnw::UpdateChecker::OffersLater(snap)"),
		regexp.QuoteMeta("if (snap.phase == Phase::None) {"))...)
	// The banner's button is written through that row. The bar's own action
	// slot is empty: a label or a state written to it reaches no control.
	if strings.Contains(banner, "bar.ActionButton()") {
		problems = append(problems, "ConnectPage::RenderUpdateBanner writes the banner's button through the bar's action slot, which is empty")
	}
	if count := strings.Count(banner, "if (auto button = actionButton) {"); count != 2 {
		problems = append(problems, fmt.Sprintf("ConnectPage::RenderUpdateBanner writes the banner's button in %d places, want its two: "+
			"the warning that checks have not worked, and every other banner", count))
	}
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
		regexp.QuoteMeta("stale = update::OfferMayHaveChanged(*offer.feed, offerServerUnix_, sinceCheck);"),
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
		regexp.QuoteMeta("update::OfferMayHaveChanged(*offer_.feed, offerServerUnix_, sinceCheck);"),
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
	banner := applyDefinition(connect, offerBannerRenderer)
	problems = append(problems, applyOrderProblems("ConnectPage::RenderUpdateBanner", banner,
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
		regexp.QuoteMeta(`Automation::AutomationProperties::SetAutomationId(later, L"acceptance.update.later");`),
		regexp.QuoteMeta("actions.Children().Append(update);"),
		regexp.QuoteMeta("actions.Children().Append(later);"),
		regexp.QuoteMeta("UpdateBar().Content(actions);"))...)
	problems = append(problems, applyOrderProblems("DeveloperPage's update check", developer,
		regexp.QuoteMeta("Automation::AutomationProperties::SetAutomationId(checkUpdates,"),
		regexp.QuoteMeta(`L"acceptance.update.check-now");`),
		regexp.QuoteMeta("urnw::pages::Updates().CheckNow();"),
		regexp.QuoteMeta("Automation::AutomationProperties::SetAutomationId(updateCheckText_,"),
		regexp.QuoteMeta(`L"acceptance.update.check-line");`))...)
	// The sign-in screen's banner and its row have ids of their own: the two
	// banners are never on the screen together, and a driver reads each by its
	// own.
	if got := xaml["SignedOutUpdateBar"]; got != "acceptance.update.signed-out-banner" {
		problems = append(problems, fmt.Sprintf("SignedOutUpdateBar: AutomationProperties.AutomationId = %q, want %q", got, "acceptance.update.signed-out-banner"))
	}
	problems = append(problems, applyOrderProblems("MainWindow's sign-in screen update banner", window,
		regexp.QuoteMeta("Automation::AutomationProperties::SetAutomationId(signedOutUpdate,"),
		regexp.QuoteMeta(`L"acceptance.update.signed-out-action");`),
		regexp.QuoteMeta("Automation::AutomationProperties::SetAutomationId(signedOutLater,"),
		regexp.QuoteMeta(`L"acceptance.update.signed-out-later");`),
		regexp.QuoteMeta("signedOutActions.Children().Append(signedOutUpdate);"),
		regexp.QuoteMeta("signedOutActions.Children().Append(signedOutLater);"),
		regexp.QuoteMeta("signedOutBar.Content(signedOutActions);"))...)
	for source, ids := range map[string][]string{
		window: {`L"acceptance.update.action"`, `L"acceptance.update.later"`,
			`L"acceptance.update.signed-out-action"`, `L"acceptance.update.signed-out-later"`},
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

// The developer screen's update check is a card of its own, in the stack
// that always shows, after the intro. In the intro's row of actions it pushed
// that row past the card's edge at the window's default width, and a gated
// card would hide it while there is no session, which the check does not
// need.
func checkUpdateCheckHasItsOwnCard(developer string) []string {
	build := applyDefinition(developer, "void DeveloperPage::Build() {")
	problems := applyOrderProblems("DeveloperPage::Build", build,
		regexp.QuoteMeta(`Border card = MakeCard(Dev("dev_developer", L"Developer"), body);`),
		regexp.QuoteMeta("body.Children().Append(actions);"),
		regexp.QuoteMeta("top.Children().Append(card);"),
		regexp.QuoteMeta(`Border card = MakeCard(Loc("dev_updates_heading"), body);`),
		regexp.QuoteMeta("body.Children().Append(checkUpdates);"),
		regexp.QuoteMeta("body.Children().Append(updateCheckText_);"),
		regexp.QuoteMeta("top.Children().Append(card);"),
		regexp.QuoteMeta("auto deviceCard = [&](Panel const& host, hstring const& heading, StackPanel& body) {"))
	if strings.Contains(build, "actions.Children().Append(checkUpdates);") {
		problems = append(problems, "the update check is in the intro's row of actions, which it pushes past the card's edge")
	}
	if start := strings.Index(build, `Border card = MakeCard(Loc("dev_updates_heading"), body);`); start >= 0 {
		card := build[start:]
		if end := strings.Index(card, "\n  }\n"); end >= 0 {
			card = card[:end]
		}
		for _, gate := range []string{"deviceCards_", "settingsCards_", "card.Visibility("} {
			if strings.Contains(card, gate) {
				problems = append(problems, "the Updates card is gated ("+gate+
					"): the update check needs no session, no device and no service")
			}
		}
	}
	return problems
}

// What each surface says about the update channel, drawn from the checker's
// snapshot through UpdateChannel.h LinesOf, and how a surface asks for a
// change. The Developer screen's Updates card holds the developer builds
// switch, built only while the feed table has the developer row, its
// channel line and, for a marker that is not trusted, the way back; the
// switch's own handler returns while the screen writes the marker's state
// into it. Its check line never says "this build is current" for a developer
// build or on the developer feed. Settings has a channel row under the
// automatic check, shown only while LinesOf says so, with its button, and
// About's "up to date" is LinesOf's to allow; the row's method is defined at
// the file's end, and its line in the window's fan-out sits between the
// connect screen's and the developer screen's. Both update banners put a
// developer offer under its own title, and say whose build it is first. The
// Developer nav item has an id, and every new id is one control's.
func checkChannelSurfaces(developer, settings, connect, window string, xaml map[string]string) []string {
	var problems []string
	build := applyDefinition(developer, "void DeveloperPage::Build() {")
	problems = append(problems, applyOrderProblems("DeveloperPage::Build", build,
		regexp.QuoteMeta(`Border card = MakeCard(Loc("dev_updates_heading"), body);`),
		regexp.QuoteMeta("if (const update::Feed* developer = update::FeedById(update::kDeveloperChannelId)) {"),
		regexp.QuoteMeta("channelSwitch_ = ToggleSwitch();"),
		regexp.QuoteMeta(`L"acceptance.update.channel-switch");`),
		regexp.QuoteMeta("self->developer().OnChannelToggled();"),
		regexp.QuoteMeta(`urnw::Format("dev_channel_switch_note", UpdateChecker::RepositoryOf(*developer))`),
		regexp.QuoteMeta(`Automation::AutomationProperties::SetAutomationId(channelLine_, L"acceptance.update.channel-line");`),
		regexp.QuoteMeta(`channelRepair_ = MakeActionButton(Loc("upd_channel_switch_official"));`),
		regexp.QuoteMeta(`L"acceptance.update.channel-repair");`),
		regexp.QuoteMeta("urnw::pages::Updates().RequestChannel(update::kOfficialFeed);"),
		regexp.QuoteMeta("body.Children().Append(checkUpdates);"))...)
	problems = append(problems, applyOrderProblems("DeveloperPage::OnChannelToggled",
		applyDefinition(developer, "void DeveloperPage::OnChannelToggled() {"),
		regexp.QuoteMeta("if (applyingChannel_ || !channelSwitch_) return;"),
		regexp.QuoteMeta("const update::Feed* developer = update::FeedById(update::kDeveloperChannelId);"),
		regexp.QuoteMeta("urnw::pages::Updates().RequestChannel(channelSwitch_.IsOn() ? *developer : update::kOfficialFeed);"))...)
	check := applyDefinition(developer, "void DeveloperPage::ApplyUpdateCheck(UpdateChecker::Snapshot const& snap) {")
	problems = append(problems, applyOrderProblems("DeveloperPage::ApplyUpdateCheck", check,
		regexp.QuoteMeta("const update::ChannelLines lines = UpdateChecker::ChannelLinesOf(snap);"),
		regexp.QuoteMeta("applyingChannel_ = true;"),
		regexp.QuoteMeta("channelSwitch_.IsOn(lines.switchOn);"),
		regexp.QuoteMeta("channelSwitch_.IsEnabled(lines.switchEnabled);"),
		regexp.QuoteMeta("applyingChannel_ = false;"),
		regexp.QuoteMeta("std::wstring channel = UpdateChecker::ChannelSentenceText(snap, lines);"),
		regexp.QuoteMeta("UpdateChecker::BuildSentenceText(lines), UpdateChecker::ChangeSentenceText(snap, lines)"),
		regexp.QuoteMeta("channelLine_.Text(hstring{channel});"),
		regexp.QuoteMeta("channelRepair_.Visibility(lines.repairButton ? Visibility::Visible : Visibility::Collapsed);"),
		regexp.QuoteMeta("channelRepair_.IsEnabled(lines.buttonsEnabled);"),
		regexp.QuoteMeta("case Outcome::NoUpdate:"),
		regexp.QuoteMeta(`lines.check == update::CheckLine::NoneListed`),
		regexp.QuoteMeta(`urnw::Localized("dev_update_none_listed")`),
		regexp.QuoteMeta(`lines.check == update::CheckLine::NoneDeveloper`),
		regexp.QuoteMeta(`urnw::Localized("dev_update_none_developer")`),
		regexp.QuoteMeta(`lines.check == update::CheckLine::NoneOfficial`),
		regexp.QuoteMeta(`urnw::Localized("dev_update_none_official")`),
		regexp.QuoteMeta(`DevW("dev_update_none", L"No update: this build is current")`))...)
	general := applyDefinition(settings, "void SettingsPage::BuildGeneralSection(Panel const& host) {")
	problems = append(problems, applyOrderProblems("SettingsPage::BuildGeneralSection", general,
		regexp.QuoteMeta("urnw::pages::Updates().SetAutoCheckEnabled(autoUpdateCheck_.IsOn());"),
		regexp.QuoteMeta("channelRow_ = StackPanel();"),
		regexp.QuoteMeta("channelRow_.Visibility(Visibility::Collapsed);"),
		regexp.QuoteMeta(`channelOfficial_ = ButtonRow(channelRow_, Loc("upd_channel_label"), hstring{},`),
		regexp.QuoteMeta(`L"acceptance.update.channel-official");`),
		regexp.QuoteMeta("urnw::pages::Updates().RequestChannel(urnw::update::kOfficialFeed);"),
		regexp.QuoteMeta("channelNote_ = Supporting(channelRow_, hstring{});"),
		regexp.QuoteMeta(`L"acceptance.update.channel-row");`),
		regexp.QuoteMeta("ApplyUpdateChannel(urnw::pages::Updates().Current());"),
		regexp.QuoteMeta("launchAtStartup_ = ToggleRow("))...)
	channel := applyDefinition(settings, "void SettingsPage::ApplyUpdateChannel(UpdateChecker::Snapshot const& snap) {")
	problems = append(problems, applyOrderProblems("SettingsPage::ApplyUpdateChannel", channel,
		regexp.QuoteMeta("const update::ChannelLines lines = UpdateChecker::ChannelLinesOf(snap);"),
		regexp.QuoteMeta("channelRow_.Visibility(lines.settingsRow ? Visibility::Visible : Visibility::Collapsed);"),
		regexp.QuoteMeta(`urnw::Format("upd_channel_developer_note", UpdateChecker::RepositoryOf(*snap.feed))`),
		regexp.QuoteMeta(`urnw::Localized("upd_channel_untrusted")`),
		regexp.QuoteMeta(`urnw::Localized("upd_channel_official_note")`),
		regexp.QuoteMeta("if (lines.settingsBuildSentence) note += L\" \" + UpdateChecker::BuildSentenceText(lines);"),
		regexp.QuoteMeta(`if (lines.settingsWhere) note += L" " + urnw::Localized("upd_channel_where");`),
		regexp.QuoteMeta("UpdateChecker::ChangeSentenceText(snap, lines)"),
		regexp.QuoteMeta("channelOfficial_.Visibility(lines.settingsButton ? Visibility::Visible : Visibility::Collapsed);"),
		regexp.QuoteMeta("channelOfficial_.IsEnabled(lines.buttonsEnabled);"))...)
	const channelOpener = "void SettingsPage::ApplyUpdateChannel("
	if at := strings.Index(settings, channelOpener); at < 0 || strings.Contains(settings[at+len(channelOpener):], "SettingsPage::") {
		problems = append(problems, "SettingsPage::ApplyUpdateChannel is not the last definition of SettingsPage.cpp, "+
			"where a merge with the fork's own update methods gains no conflict")
	}
	problems = append(problems, applyOrderProblems("SettingsPage::ApplyUpdateCheck",
		applyDefinition(settings, "void SettingsPage::ApplyUpdateCheck(UpdateChecker::Snapshot const& snap) {"),
		regexp.QuoteMeta("case Outcome::NoUpdate:"),
		regexp.QuoteMeta("if (UpdateChecker::ChannelLinesOf(snap).aboutUpToDate) {"),
		regexp.QuoteMeta(`text += L" — " + AdvW("upd_state_current", L"up to date");`))...)
	problems = append(problems, applyOrderProblems("MainWindow::ApplyUpdateChecker",
		applyDefinition(window, "void MainWindow::ApplyUpdateChecker() {"),
		regexp.QuoteMeta("if (connect_) connect_->ApplyUpdateChecker(updateSnapshot_);"),
		regexp.QuoteMeta("if (settings_) settings_->ApplyUpdateChannel(updateSnapshot_);"),
		regexp.QuoteMeta("if (developer_) developer_->ApplyUpdateCheck(updateSnapshot_);"),
		regexp.QuoteMeta("if (settings_) settings_->ApplyUpdateCheck(updateSnapshot_);"))...)
	banner := applyDefinition(connect, offerBannerRenderer)
	problems = append(problems, applyOrderProblems("ConnectPage::RenderUpdateBanner", banner,
		regexp.QuoteMeta(`winrt::hstring title{urnw::Format("upd_available_title_version", snap.version)};`),
		regexp.QuoteMeta("const bool developerOffer = snap.feed->id != urnw::update::kOfficialFeed.id;"),
		regexp.QuoteMeta(`title = winrt::hstring{urnw::Format("upd_developer_title_version", snap.version)};`),
		regexp.QuoteMeta("case Phase::Result:"),
		regexp.QuoteMeta("if (developerOffer && snap.phase != Phase::Result) {"),
		regexp.QuoteMeta(`message = urnw::Format("upd_developer_message", urnw::UpdateChecker::RepositoryOf(*snap.feed)) +`),
		regexp.QuoteMeta("bar.Title(title);"),
		regexp.QuoteMeta("bar.Message(winrt::hstring{message});"))...)
	if got := xaml["DeveloperNavItem"]; got != "acceptance.nav.developer" {
		problems = append(problems, fmt.Sprintf("DeveloperNavItem: AutomationProperties.AutomationId = %q, want %q", got, "acceptance.nav.developer"))
	}
	for source, ids := range map[string][]string{
		developer: {`L"acceptance.update.channel-switch"`, `L"acceptance.update.channel-line"`, `L"acceptance.update.channel-repair"`},
		settings:  {`L"acceptance.update.channel-official"`, `L"acceptance.update.channel-row"`},
	} {
		for _, id := range ids {
			if strings.Count(source, id) != 1 {
				problems = append(problems, "want exactly one "+id)
			}
		}
	}
	return problems
}

func TestUpdateOfferTheChannelOnEverySurface(t *testing.T) {
	xaml := xamlAutomationIds(t, filepath.Join(repositoryRoot(t), "app", "src", "App", "MainWindow.xaml"))
	reportProblems(t, checkChannelSurfaces(stripComments(readAppSource(t, "DeveloperPage.cpp")),
		stripComments(readAppSource(t, "SettingsPage.cpp")), stripComments(readAppSource(t, "ConnectPage.cpp")),
		stripComments(readAppSource(t, "MainWindow.xaml.cpp")), xaml))
}

// Each surface of the channel, taken away, is reported for its reason.
func TestUpdateOfferTheChannelOnEverySurfaceRejectsWeakerSources(t *testing.T) {
	developer := stripComments(readAppSource(t, "DeveloperPage.cpp"))
	settings := stripComments(readAppSource(t, "SettingsPage.cpp"))
	connect := stripComments(readAppSource(t, "ConnectPage.cpp"))
	window := stripComments(readAppSource(t, "MainWindow.xaml.cpp"))
	xaml := xamlAutomationIds(t, filepath.Join(repositoryRoot(t), "app", "src", "App", "MainWindow.xaml"))
	replace := func(text, old, replacement string) string {
		if strings.Count(text, old) != 1 {
			t.Fatalf("negative control: %q is not in the source exactly once", old)
		}
		return strings.Replace(text, old, replacement, 1)
	}
	surfaces := func(d, s, c, w string, x map[string]string) []string { return checkChannelSurfaces(d, s, c, w, x) }
	for _, tc := range []struct {
		name, want string
		check      func() []string
	}{
		{"the switch's revert read as a request", `DeveloperPage::OnChannelToggled is missing if \(applyingChannel_`, func() []string {
			return surfaces(replace(developer, "  if (applyingChannel_ || !channelSwitch_) return;\n", "  if (!channelSwitch_) return;\n"),
				settings, connect, window, xaml)
		}},
		{"the switch written without its guard", "DeveloperPage::ApplyUpdateCheck is missing applyingChannel_ = true;", func() []string {
			return surfaces(replace(developer, "    applyingChannel_ = true;\n", ""), settings, connect, window, xaml)
		}},
		{"a switch built whatever the table holds", `DeveloperPage::Build is missing if \(const update::Feed\* developer = update::FeedById`, func() []string {
			return surfaces(replace(developer, "if (const update::Feed* developer = update::FeedById(update::kDeveloperChannelId)) {",
				"if (const update::Feed* developer = &update::kOfficialFeed) {"), settings, connect, window, xaml)
		}},
		{"the check line saying this build is current on the developer feed", `DeveloperPage::ApplyUpdateCheck is missing lines\.check == update::CheckLine::NoneListed`, func() []string {
			return surfaces(replace(developer, "lines.check == update::CheckLine::NoneListed", "false"), settings, connect, window, xaml)
		}},
		{"About saying up to date whatever the channel", `SettingsPage::ApplyUpdateCheck is missing if \(UpdateChecker::ChannelLinesOf`, func() []string {
			return surfaces(developer, replace(settings, "      if (UpdateChecker::ChannelLinesOf(snap).aboutUpToDate) {\n", "      if (true) {\n"),
				connect, window, xaml)
		}},
		{"Settings' channel row always shown", `SettingsPage::ApplyUpdateChannel is missing channelRow_\.Visibility\(lines\.settingsRow`, func() []string {
			return surfaces(developer, replace(settings, "channelRow_.Visibility(lines.settingsRow ? Visibility::Visible : Visibility::Collapsed);",
				"channelRow_.Visibility(Visibility::Visible);"), connect, window, xaml)
		}},
		{"Settings' button on a copy that cannot change the channel", `SettingsPage::ApplyUpdateChannel is missing channelOfficial_\.Visibility\(lines\.settingsButton`, func() []string {
			return surfaces(developer, replace(settings, "channelOfficial_.Visibility(lines.settingsButton ? Visibility::Visible : Visibility::Collapsed);",
				"channelOfficial_.Visibility(Visibility::Visible);"), connect, window, xaml)
		}},
		{"the row's method next to ApplyUpdateCheck", "SettingsPage::ApplyUpdateChannel is not the last definition", func() []string {
			moved := replace(settings, "void SettingsPage::ApplyUpdateCheck(UpdateChecker::Snapshot const& snap) {",
				"void SettingsPage::ApplyUpdateChannel(UpdateChecker::Snapshot const& snap) {\n}\n\n"+
					"void SettingsPage::ApplyUpdateCheck(UpdateChecker::Snapshot const& snap) {")
			return surfaces(developer, replace(moved, "\nvoid SettingsPage::ApplyUpdateChannel(UpdateChecker::Snapshot const& snap) {\n  if (!channelRow_)",
				"\nvoid SettingsPage::ApplyUpdateChannelLater(UpdateChecker::Snapshot const& snap) {\n  if (!channelRow_)"), connect, window, xaml)
		}},
		{"the channel's row left out of the fan-out", "MainWindow::ApplyUpdateChecker is missing if \\(settings_\\) settings_->ApplyUpdateChannel", func() []string {
			return surfaces(developer, settings, connect, replace(window, "  if (settings_) settings_->ApplyUpdateChannel(updateSnapshot_);\n", ""), xaml)
		}},
		{"a developer offer under the official title", "ConnectPage::RenderUpdateBanner is missing title = winrt::hstring", func() []string {
			return surfaces(developer, settings, replace(connect,
				"    title = winrt::hstring{urnw::Format(\"upd_developer_title_version\", snap.version)};\n", ""), window, xaml)
		}},
		{"a developer offer that does not say whose build it is", `ConnectPage::RenderUpdateBanner is missing if \(developerOffer && snap\.phase != Phase::Result\)`, func() []string {
			return surfaces(developer, settings, replace(connect, "  if (developerOffer && snap.phase != Phase::Result) {\n", "  if (false) {\n"), window, xaml)
		}},
		{"the Developer nav item without its id", "DeveloperNavItem: AutomationProperties.AutomationId", func() []string {
			without := map[string]string{}
			for name, id := range xaml {
				if name != "DeveloperNavItem" {
					without[name] = id
				}
			}
			return surfaces(developer, settings, connect, window, without)
		}},
		{"one id on two controls", `want exactly one L"acceptance.update.channel-switch"`, func() []string {
			return surfaces(replace(developer, `L"acceptance.update.channel-line"`, `L"acceptance.update.channel-switch"`), settings, connect, window, xaml)
		}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			problems := strings.Join(tc.check(), "\n")
			if !strings.Contains(problems, tc.want) && !regexp.MustCompile(tc.want).MatchString(problems) {
				t.Fatalf("negative control was not detected by %q; the check reported:\n%s", tc.want, problems)
			}
		})
	}
}

// Each x:Name in a XAML document, with the x:Names of the elements around it,
// outermost first.
func xamlNamedAncestors(markup string) (map[string][]string, error) {
	decoder := xml.NewDecoder(strings.NewReader(markup))
	ancestors := map[string][]string{}
	var open []string // the x:Name of each open element, "" for none
	for {
		token, err := decoder.Token()
		if err == io.EOF {
			return ancestors, nil
		}
		if err != nil {
			return nil, err
		}
		switch element := token.(type) {
		case xml.StartElement:
			name := ""
			for _, attribute := range element.Attr {
				if attribute.Name.Local == "Name" && strings.HasSuffix(attribute.Name.Space, "/xaml") {
					name = attribute.Value
				}
			}
			if name != "" {
				var around []string
				for _, outer := range open {
					if outer != "" {
						around = append(around, outer)
					}
				}
				ancestors[name] = around
			}
			open = append(open, name)
		case xml.EndElement:
			open = open[:len(open)-1]
		}
	}
}

// A copy that is signed out is offered the update (fold-in i). The connect
// screen's banner is inside HomeNav, which is collapsed while signed out, so
// the sign-in screen has a banner of its own, SignedOutUpdateBar: in the
// initial step's column under the signed-out notice, outside HomeNav, closed
// and collapsed until it has something to say. It is fed from the window's
// update fan-out, which runs for every snapshot, signed in or out, and not
// from a screen hook: LoadCurrentDestination returns while signed out. One
// renderer writes both banners, so the same rules hold for both; the sign-in
// screen's is collapsed while closed, so the column LoginPage measures keeps
// no gap for it. Its row is the connect screen's: the same action, Later,
// and a close that dismisses a report.
func checkUpdateOfferedWhileSignedOut(markup, window, connect string) []string {
	var problems []string
	ancestors, err := xamlNamedAncestors(markup)
	if err != nil {
		return append(problems, "MainWindow.xaml does not parse: "+err.Error())
	}
	around, ok := ancestors["SignedOutUpdateBar"]
	if !ok {
		return append(problems, "MainWindow.xaml has no SignedOutUpdateBar: a signed-out copy is offered no update")
	}
	has := func(names []string, want string) bool {
		for _, name := range names {
			if name == want {
				return true
			}
		}
		return false
	}
	if !has(ancestors["UpdateBar"], "HomeNav") {
		problems = append(problems, "UpdateBar is no longer inside HomeNav: this check's reason for a second banner is gone")
	}
	if has(around, "HomeNav") {
		problems = append(problems, "SignedOutUpdateBar is inside HomeNav, which is collapsed while signed out")
	}
	if !has(around, "LoginRoot") || !has(around, "LoginPanel") {
		problems = append(problems, fmt.Sprintf("SignedOutUpdateBar is not in the sign-in screen's initial column (around it: %v)", around))
	}
	notice := strings.Index(markup, `x:Name="LoginNotice"`)
	banner := strings.Index(markup, `x:Name="SignedOutUpdateBar"`)
	carousel := strings.Index(markup, `x:Name="LoginCarouselHost"`)
	if !(notice >= 0 && notice < banner && banner < carousel) {
		problems = append(problems, "SignedOutUpdateBar is not between the signed-out notice and the carousel, at the top of the column")
	}
	if end := strings.Index(markup[banner:], "/>"); end >= 0 {
		element := markup[banner : banner+end]
		for _, want := range []string{`IsOpen="False"`, `Visibility="Collapsed"`} {
			if !strings.Contains(element, want) {
				problems = append(problems, "SignedOutUpdateBar is missing "+want+": it would hold a gap in the column while it says nothing")
			}
		}
	}
	// the fan-out renders it, at the top level of the function, beside the
	// connect screen's banner
	fan := applyDefinition(window, "void MainWindow::ApplyUpdateChecker() {")
	problems = append(problems, applyOrderProblems("MainWindow::ApplyUpdateChecker", fan,
		regexp.QuoteMeta("if (connect_) connect_->ApplyUpdateChecker(updateSnapshot_);"),
		regexp.QuoteMeta("auto signedOutBar = SignedOutUpdateBar();"),
		regexp.QuoteMeta("urnw::ConnectPage::RenderUpdateBanner(signedOutBar, updateSnapshot_);"),
		regexp.QuoteMeta("signedOutBar.Visibility(signedOutBar.IsOpen() ? Visibility::Visible : Visibility::Collapsed);"),
		regexp.QuoteMeta("if (developer_) developer_->ApplyUpdateCheck(updateSnapshot_);"))...)
	if !strings.Contains(fan, "\n  urnw::ConnectPage::RenderUpdateBanner(signedOutBar, updateSnapshot_);\n") {
		problems = append(problems, "MainWindow::ApplyUpdateChecker renders the sign-in screen's banner under a condition: it must for every snapshot")
	}
	if count := strings.Count(window, "RenderUpdateBanner("); count != 1 {
		problems = append(problems, fmt.Sprintf("MainWindow renders an update banner in %d places, want the fan-out's one", count))
	}
	if count := strings.Count(window, "SignedOutUpdateBar()"); count != 2 {
		problems = append(problems, fmt.Sprintf("MainWindow names SignedOutUpdateBar %d times, want twice: its row in the ctor and its rendering in the fan-out", count))
	}
	// one renderer for both banners
	const applyOpener = "void ConnectPage::ApplyUpdateChecker(urnw::UpdateChecker::Snapshot const& snap) {"
	// the body, with its whitespace folded: the connect screen's banner by the
	// shared renderer, then collapsed while closed (pinned under the
	// scroller, checkUpdateBannerRowStaysOnScreen), and nothing else
	body := strings.Join(strings.Fields(strings.TrimSuffix(strings.TrimPrefix(applyDefinition(connect, applyOpener), applyOpener), "\n}")), " ")
	if body != "auto bar = w_.UpdateBar(); RenderUpdateBanner(bar, snap); bar.Visibility(bar.IsOpen() ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);" {
		problems = append(problems, fmt.Sprintf("ConnectPage::ApplyUpdateChecker is %q, want the connect screen's banner by the shared renderer alone", body))
	}
	if count := strings.Count(connect, "RenderUpdateBanner("); count != 2 {
		problems = append(problems, fmt.Sprintf("ConnectPage.cpp names RenderUpdateBanner %d times, want its definition and the connect screen's call", count))
	}
	if strings.Contains(connect, "SignedOutUpdateBar") {
		problems = append(problems, "ConnectPage.cpp writes the sign-in screen's banner: the window's fan-out does")
	}
	// the row: the same action, Later, the content, and a close
	problems = append(problems, applyOrderProblems("MainWindow's sign-in screen update banner", window,
		regexp.QuoteMeta("auto signedOutBar = SignedOutUpdateBar();"),
		regexp.QuoteMeta("Button signedOutUpdate;"),
		regexp.QuoteMeta("signedOutUpdate.Click([weak = get_weak()](auto const&, auto const&) {"),
		regexp.QuoteMeta("HyperlinkButton signedOutLater;"),
		regexp.QuoteMeta("signedOutLater.Visibility(Visibility::Collapsed);"),
		regexp.QuoteMeta("signedOutLater.Click([](auto const&, auto const&) { urnw::pages::Updates().Later(); });"),
		regexp.QuoteMeta("signedOutActions.Orientation(winrt::Microsoft::UI::Xaml::Controls::Orientation::Horizontal);"),
		regexp.QuoteMeta("signedOutActions.Children().Append(signedOutUpdate);"),
		regexp.QuoteMeta("signedOutActions.Children().Append(signedOutLater);"),
		regexp.QuoteMeta("signedOutBar.Content(signedOutActions);"),
		regexp.QuoteMeta("signedOutBar.Closed("))...)
	// each handler, read alone: the action does what the connect screen's
	// does, and the close gives the column its room back and dismisses a
	// report
	handler := func(opener string) string {
		at := strings.Index(window, opener)
		if at < 0 {
			return ""
		}
		end := strings.Index(window[at:], "});")
		if end < 0 {
			return ""
		}
		return window[at : at+end]
	}
	problems = append(problems, applyOrderProblems("the sign-in screen update banner's action", handler("signedOutUpdate.Click("),
		regexp.QuoteMeta("if (auto self = weak.get()) self->OnUpdateBannerAction();"))...)
	problems = append(problems, applyOrderProblems("the sign-in screen update banner's close", handler("signedOutBar.Closed("),
		regexp.QuoteMeta("sender.Visibility(Visibility::Collapsed);"),
		regexp.QuoteMeta("if (args.Reason() != Microsoft::UI::Xaml::Controls::InfoBarCloseReason::CloseButton) return;"),
		regexp.QuoteMeta("self->updateSnapshot_.phase == urnw::UpdateChecker::Phase::Result"),
		regexp.QuoteMeta("urnw::pages::Updates().DismissResult();"))...)
	if strings.Contains(window, "signedOutBar.ActionButton(") {
		problems = append(problems, "MainWindow puts a control in the sign-in screen banner's own action slot, which the renderer never writes")
	}
	return problems
}

// An element of a XAML document: its type's local name, its x:Name, its
// attributes outside any namespace (Grid.Row, Height, Visibility), and its
// place in the tree.
type xamlElement struct {
	local    string
	name     string
	attrs    map[string]string
	parent   *xamlElement
	children []*xamlElement
}

// The elements of a XAML document that have an x:Name, by name.
func xamlNamedElements(markup string) (map[string]*xamlElement, error) {
	decoder := xml.NewDecoder(strings.NewReader(markup))
	named := map[string]*xamlElement{}
	var open []*xamlElement
	for {
		token, err := decoder.Token()
		if err == io.EOF {
			return named, nil
		}
		if err != nil {
			return nil, err
		}
		switch t := token.(type) {
		case xml.StartElement:
			element := &xamlElement{local: t.Name.Local, attrs: map[string]string{}}
			for _, attribute := range t.Attr {
				if attribute.Name.Local == "Name" && strings.HasSuffix(attribute.Name.Space, "/xaml") {
					element.name = attribute.Value
				} else if attribute.Name.Space == "" {
					element.attrs[attribute.Name.Local] = attribute.Value
				}
			}
			if len(open) > 0 {
				element.parent = open[len(open)-1]
				element.parent.children = append(element.parent.children, element)
			}
			if element.name != "" {
				named[element.name] = element
			}
			open = append(open, element)
		case xml.EndElement:
			open = open[:len(open)-1]
		}
	}
}

// The connect screen's update banner keeps its Update and Later row on the
// screen at every height the window allows (fold-in iii). In the connect
// pane's scrolling column it sat below the hero, the location, the Connect
// button and the peers line, and a window shorter than 760 put the row under
// the status strip and the pane's bottom fade: 480x728, the size the window
// opens with on a 1024x768 screen (fork run 38294750901). So the banner is in
// no ScrollViewer. It has a row of its own in the connect pane's grid, sized
// to it (Auto), under the scroller's row (*), and nothing else shares it. It
// is closed and collapsed until it has something to say, and the connect page
// collapses it again whenever it closes, so the column keeps its height then.
func checkUpdateBannerRowStaysOnScreen(markup, connect string) []string {
	var problems []string
	named, err := xamlNamedElements(markup)
	if err != nil {
		return append(problems, "MainWindow.xaml does not parse: "+err.Error())
	}
	bar := named["UpdateBar"]
	if bar == nil {
		return append(problems, "MainWindow.xaml has no UpdateBar")
	}
	for up := bar.parent; up != nil; up = up.parent {
		if up.local == "ScrollViewer" {
			problems = append(problems, fmt.Sprintf("UpdateBar is inside the ScrollViewer %q: a window short enough scrolls its row under the status strip", up.name))
		}
	}
	pane := bar.parent
	if pane == nil || pane.local != "Grid" || pane.name != "ConnectPaneA" {
		return append(problems, "UpdateBar is not in a row of the connect pane's grid (ConnectPaneA)")
	}
	var rows []string
	for _, child := range pane.children {
		if child.local != "Grid.RowDefinitions" {
			continue
		}
		for _, row := range child.children {
			if row.local == "RowDefinition" {
				height := row.attrs["Height"]
				if height == "" {
					height = "*"
				}
				rows = append(rows, height)
			}
		}
	}
	rowOf := func(element *xamlElement) int {
		row, err := strconv.Atoi(element.attrs["Grid.Row"])
		if err != nil {
			return 0
		}
		return row
	}
	barRow := rowOf(bar)
	if barRow >= len(rows) || rows[barRow] != "Auto" {
		problems = append(problems, fmt.Sprintf("UpdateBar is in row %d of ConnectPaneA's rows %v: want a row of its own, sized to it (Auto)", barRow, rows))
	}
	scroll := named["PaneAScroll"]
	if scroll == nil || scroll.parent != pane {
		problems = append(problems, "PaneAScroll is no longer a row of ConnectPaneA: this check no longer reads the pane")
	} else if scrollRow := rowOf(scroll); scrollRow >= barRow || scrollRow >= len(rows) || rows[scrollRow] != "*" {
		problems = append(problems, fmt.Sprintf("UpdateBar's row %d is not under the scroller's row %d, the pane's one that takes what is left (*), in %v", barRow, scrollRow, rows))
	}
	for _, child := range pane.children {
		if child != bar && !strings.Contains(child.local, ".") && rowOf(child) == barRow {
			problems = append(problems, fmt.Sprintf("%s %q shares the update banner's row %d", child.local, child.name, barRow))
		}
	}
	if bar.attrs["IsOpen"] != "False" || bar.attrs["Visibility"] != "Collapsed" {
		problems = append(problems, "UpdateBar is not closed and collapsed until it has something to say: the pane would lose its margins")
	}
	apply := applyDefinition(connect, "void ConnectPage::ApplyUpdateChecker(urnw::UpdateChecker::Snapshot const& snap) {")
	problems = append(problems, applyOrderProblems("ConnectPage::ApplyUpdateChecker", apply,
		regexp.QuoteMeta("auto bar = w_.UpdateBar();"),
		regexp.QuoteMeta("RenderUpdateBanner(bar, snap);"),
		`bar\.Visibility\(bar\.IsOpen\(\)\s*\?\s*winrt::Microsoft::UI::Xaml::Visibility::Visible\s*:\s*winrt::Microsoft::UI::Xaml::Visibility::Collapsed\);`)...)
	return problems
}

// The first release that updates itself, and the day it was published (UTC),
// which the README names. Both are facts about releases already published,
// pinned here as they were read. In urnetwork/build's release list,
// v2026.10.9-1067985620 was published at 2026-10-10T04:32:06Z, is not a
// pre-release, and carries both MSIs; the windows submodule at its tag is
// ebb867a6, which contains #8's merge 706ed647, the change that made the
// updater poll this feed. The MSI release before it, v2026.10.8-1066946420,
// is at ef87934a, which does not contain it; only two Android pre-releases
// are listed between them. In the wild, fork run 38294750901
// (Ryanmello07/urnetwork-windows) installed the published x64 MSI of
// v2026.10.9-1067985620, and it updated itself to v2026.10.10-1068595480,
// both from its own window and through the update helper alone.
const (
	firstSelfUpdatingRelease = "v2026.10.9-1067985620"
	firstSelfUpdatingDate    = "2026-10-10"
)

// The README's Download section sends people to the releases the updater
// polls: the feed's own repository, by the owner and name compiled in. It
// says what the updater offers without promising every release, that a hand
// install should prefer a release that has been out for a day (the newest
// one can be minutes old, and no soak covers a hand install), and that a
// release built before the updater polled this feed does not update itself
// from it. Which releases those are it says in one sentence, which names the
// first release that does and the day it was published (the two constants
// above); no other sentence of that paragraph gives a date or a tag, the
// pinned ones included. Until that release existed it gave a rule of thumb
// instead, nothing offered while a release newer than yours has been listed
// for three days, and that rule is not brought back: by its own words
// anywhere in the README, nor in other words in the Download section.
// Android pre-releases are listed beside each MSI release under later codes,
// so it can send a copy that is already current to reinstall.
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
	// the sentence that names the first release that updates itself, by its
	// tag and the day it was published
	naming := `The first release that does is ` + regexp.QuoteMeta(firstSelfUpdatingRelease) + `, published on ` + regexp.QuoteMeta(firstSelfUpdatingDate) + `\.`
	for what, pattern := range map[string]string{
		"that a hand install should prefer a release that has been out for a day": `can be minutes old\. When you install by hand, prefer the newest one that has been out for a day`,
		"that the updater offers the newest release that has been out for a day":  `It offers the newest release that has been out, unchanged, for a day`,
		"which releases do not update themselves from this feed":                  `A release built before the updater polled these releases does not look for updates here, and never offers one\.`,
		"how to tell that a copy is one of them":                                  naming + ` If yours was installed from an older release,`,
		"that one install by hand of the newest release is what it takes":         `install the newest release by hand once`,
	} {
		if !regexp.MustCompile(pattern).MatchString(prose) {
			problems = append(problems, "README.md's Download section does not say "+what)
		}
	}
	// Which releases those are is said by the sentence that names the first
	// release that does look here, by its tag and the day it was published.
	// The paragraph that says it is read on its own, with that sentence cut
	// out: any date or tag left in it, the pinned ones included, is a problem.
	first := "the first release that updates itself is " + firstSelfUpdatingRelease + ", published on " + firstSelfUpdatingDate +
		", and only the sentence that names it gives a date or a tag"
	date := regexp.MustCompile(`\b\d{4}-\d{2}-\d{2}`)
	tag := regexp.MustCompile(`\bv?\d{4}\.\d{1,2}\.\d{1,2}(-\d+)?`)
	for _, paragraph := range strings.Split(section, "\n\n") {
		text := strings.Join(strings.Fields(paragraph), " ")
		if !strings.Contains(text, "look for updates here") {
			continue
		}
		if at := regexp.MustCompile(naming).FindStringIndex(text); at != nil {
			text = text[:at[0]] + text[at[1]:]
		}
		for _, found := range date.FindAllString(text, -1) {
			problems = append(problems, "README.md's Download section dates the releases that do not update themselves ("+found+"): "+first)
		}
		for _, found := range tag.FindAllString(text, -1) {
			problems = append(problems, "README.md's Download section names a release ("+found+
				") where it says which do not update themselves: "+first)
		}
	}
	// The rule of thumb that stood in for that name is not brought back:
	// Android pre-releases are listed beside each MSI release under later
	// codes, so a copy that is already current sees "a release newer than
	// yours" listed, is offered nothing, and is sent to reinstall. Its own
	// words, "newer than yours has been listed", whatever they wait, are
	// looked for in the whole README, so a section of its own does not hide
	// them. In other words it is a sentence of the Download section that
	// says "release" and pairs "newer" with a wait, in either order and with
	// capitals or without: a number or a number word (seventy-two is found
	// whole), "several" or "a few", then, after a space or a hyphen, hours,
	// days or weeks. A wait of one day ("one day", "1 day", "24 hours",
	// "one-day") is the one-day rule, which is true, so it is not one; nor is
	// "a day". Other words are looked for in the Download section alone, so
	// true text elsewhere ("1809 or newer", "every 90 days") is no problem.
	// A sentence here runs to the next full stop, and the problem quotes what
	// it found.
	threeDay := func(found string) {
		problems = append(problems, "README.md brings back the three-day rule ("+found+
			"): Android pre-releases are listed beside each MSI release under later codes, so it sends a copy that is "+
			"already current to reinstall")
	}
	for _, found := range regexp.MustCompile(`(?i)newer than yours has been listed`).FindAllString(strings.Join(strings.Fields(readme), " "), -1) {
		threeDay(found)
	}
	newer := regexp.MustCompile(`(?i)\bnewer\b`)
	release := regexp.MustCompile(`(?i)\breleases?\b`)
	wait := regexp.MustCompile(`(?i)\b(?:(?:twenty|thirty|forty|fifty|sixty|seventy|eighty|ninety)-)?` +
		`(?:\d+|one|two|three|four|five|six|seven|eight|nine|ten|eleven|twelve|thirteen|fourteen|fifteen|sixteen|seventeen|` +
		`eighteen|nineteen|twenty|thirty|forty|fifty|sixty|seventy|eighty|ninety|several|a few)[-\s]+(?:hours?|days?|weeks?)\b`)
	oneDay := regexp.MustCompile(`(?i)^(?:(?:one|1)[-\s]+days?|24[-\s]+hours?)$`)
	for _, sentence := range regexp.MustCompile(`[^.]+`).FindAllString(prose, -1) {
		found := newer.FindAllStringIndex(sentence, -1)
		if len(found) == 0 || !release.MatchString(sentence) {
			continue
		}
		waits := 0
		for _, at := range wait.FindAllStringIndex(sentence, -1) {
			if !oneDay.MatchString(sentence[at[0]:at[1]]) {
				found = append(found, at)
				waits++
			}
		}
		if waits == 0 {
			continue
		}
		from, to := len(sentence), 0
		for _, at := range found {
			from, to = min(from, at[0]), max(to, at[1])
		}
		threeDay(sentence[from:to])
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

func TestUpdateOfferTheUpdateCheckHasItsOwnCard(t *testing.T) {
	reportProblems(t, checkUpdateCheckHasItsOwnCard(stripComments(readAppSource(t, "DeveloperPage.cpp"))))
}

func TestUpdateOfferIsMadeWhileSignedOut(t *testing.T) {
	reportProblems(t, checkUpdateOfferedWhileSignedOut(readAppSource(t, "MainWindow.xaml"),
		stripComments(readAppSource(t, "MainWindow.xaml.cpp")), stripComments(readAppSource(t, "ConnectPage.cpp"))))
}

// Each part of the signed-out offer, taken away, fails its check for the
// reason it names.
func TestUpdateOfferWhileSignedOutRejectsWeakerSources(t *testing.T) {
	markup := readAppSource(t, "MainWindow.xaml")
	window := stripComments(readAppSource(t, "MainWindow.xaml.cpp"))
	connect := stripComments(readAppSource(t, "ConnectPage.cpp"))
	developer := stripComments(readAppSource(t, "DeveloperPage.cpp"))
	xaml := xamlAutomationIds(t, filepath.Join(repositoryRoot(t), "app", "src", "App", "MainWindow.xaml"))
	replace := func(text, old, replacement string) string {
		if strings.Count(text, old) != 1 {
			t.Fatalf("negative control: %q is not in the source exactly once", old)
		}
		return strings.Replace(text, old, replacement, 1)
	}
	start := strings.Index(markup, `<muxc:InfoBar x:Name="SignedOutUpdateBar"`)
	if start < 0 {
		t.Fatal("MainWindow.xaml has no SignedOutUpdateBar element to take away")
	}
	element := markup[start : start+strings.Index(markup[start:], "/>")+2]
	signedOut := func(markup, window, connect string) func() []string {
		return func() []string { return checkUpdateOfferedWhileSignedOut(markup, window, connect) }
	}
	render := "  urnw::ConnectPage::RenderUpdateBanner(signedOutBar, updateSnapshot_);\n"
	for _, tc := range []struct {
		name, want string
		check      func() []string
	}{
		{"the sources as they are", "", signedOut(markup, window, connect)},
		{"no banner while signed out", "has no SignedOutUpdateBar", signedOut(replace(markup, element, ""), window, connect)},
		{"the banner inside HomeNav", "SignedOutUpdateBar is inside HomeNav", signedOut(replace(replace(markup, element, ""),
			`AutomationProperties.AutomationId="acceptance.update.banner" />`,
			`AutomationProperties.AutomationId="acceptance.update.banner" />`+"\n"+element), window, connect)},
		{"the banner at the foot of the sign-in column", "is not between the signed-out notice and the carousel", signedOut(replace(replace(markup, element, ""),
			"                                         Click=\"OnChangeNetworkServer\" />\n                    </StackPanel>\n",
			"                                         Click=\"OnChangeNetworkServer\" />\n                    </StackPanel>\n"+element+"\n"), window, connect)},
		{"a closed banner that keeps its gap in the column", `SignedOutUpdateBar is missing Visibility="Collapsed"`,
			signedOut(replace(markup, element, strings.Replace(element, `Visibility="Collapsed"`, `Visibility="Visible"`, 1)), window, connect)},
		{"the banner left out of the fan-out", `MainWindow::ApplyUpdateChecker is missing urnw::ConnectPage::RenderUpdateBanner`,
			signedOut(markup, replace(window, render, ""), connect)},
		{"the banner fed from a screen hook", `MainWindow::ApplyUpdateChecker is missing urnw::ConnectPage::RenderUpdateBanner`,
			signedOut(markup, replace(replace(window, render, ""), "void MainWindow::LoadCurrentDestination() {\n",
				"void MainWindow::LoadCurrentDestination() {\n  auto hooked = SignedOutUpdateBar();\n  urnw::ConnectPage::RenderUpdateBanner(hooked, updateSnapshot_);\n"), connect)},
		{"the banner rendered only while signed in", "renders the sign-in screen's banner under a condition",
			signedOut(markup, replace(window, render, "  if (HomeNav().Visibility() == Visibility::Visible)\n  "+render), connect)},
		{"a second update banner rendered elsewhere in the window", "renders an update banner in 2 places",
			signedOut(markup, replace(window, "void MainWindow::LoadCurrentDestination() {\n",
				"void MainWindow::LoadCurrentDestination() {\n  urnw::ConnectPage::RenderUpdateBanner(UpdateBar(), updateSnapshot_);\n"), connect)},
		{"a closed banner that keeps its room after the fan-out", `MainWindow::ApplyUpdateChecker is missing signedOutBar\.Visibility`,
			signedOut(markup, replace(window, "  signedOutBar.Visibility(signedOutBar.IsOpen() ? Visibility::Visible : Visibility::Collapsed);\n", ""), connect)},
		{"the connect screen's banner by a renderer of its own", "ConnectPage::ApplyUpdateChecker is",
			signedOut(markup, window, replace(connect, "  RenderUpdateBanner(bar, snap);\n",
				"  bar.IsOpen(snap.phase != urnw::UpdateChecker::Phase::None);\n"))},
		{"the sign-in screen's banner written by the connect page", "ConnectPage.cpp writes the sign-in screen's banner",
			signedOut(markup, window, replace(connect, "  RenderUpdateBanner(bar, snap);\n",
				"  RenderUpdateBanner(bar, snap);\n  RenderUpdateBanner(w_.SignedOutUpdateBar(), snap);\n"))},
		{"an action that does nothing", "the sign-in screen update banner's action is missing",
			signedOut(markup, replace(window, "      signedOutUpdate.Click([weak = get_weak()](auto const&, auto const&) {\n        if (auto self = weak.get()) self->OnUpdateBannerAction();\n      });\n",
				"      signedOutUpdate.Click([weak = get_weak()](auto const&, auto const&) {\n      });\n"), connect)},
		{"a row without Later", `MainWindow's sign-in screen update banner is missing signedOutActions\.Children\(\)\.Append\(signedOutLater\);`,
			signedOut(markup, replace(window, "      signedOutActions.Children().Append(signedOutLater);\n", ""), connect)},
		{"a Later that hides nothing", `MainWindow's sign-in screen update banner is missing signedOutLater\.Click`,
			signedOut(markup, replace(window, "signedOutLater.Click([](auto const&, auto const&) { urnw::pages::Updates().Later(); });", ""), connect)},
		{"a row that is not on the banner", `MainWindow's sign-in screen update banner is missing signedOutBar\.Content\(signedOutActions\);`,
			signedOut(markup, replace(window, "      signedOutBar.Content(signedOutActions);\n", ""), connect)},
		{"the action in the bar's own slot", "puts a control in the sign-in screen banner's own action slot",
			signedOut(markup, replace(window, "      signedOutBar.Content(signedOutActions);\n",
				"      signedOutBar.Content(signedOutActions);\n      signedOutBar.ActionButton(signedOutUpdate);\n"), connect)},
		{"a closed report not dismissed", "the sign-in screen update banner's close is missing urnw::pages::Updates",
			signedOut(markup, replace(window, "              urnw::pages::Updates().DismissResult();\n", ""), connect)},
		{"a closed banner that keeps its room in the column", "the sign-in screen update banner's close is missing sender",
			signedOut(markup, replace(window, "            sender.Visibility(Visibility::Collapsed);\n", ""), connect)},
		{"the sign-in screen's banner without its automation id", "SignedOutUpdateBar: AutomationProperties.AutomationId", func() []string {
			ids := map[string]string{}
			for name, id := range xaml {
				if name != "SignedOutUpdateBar" {
					ids[name] = id
				}
			}
			return checkUpdateBannerAutomationIds(ids, window, developer)
		}},
		{"the sign-in screen's action without its automation id", `MainWindow's sign-in screen update banner is missing L"acceptance\.update\.signed-out-action"\);`, func() []string {
			return checkUpdateBannerAutomationIds(xaml, replace(window, `L"acceptance.update.signed-out-action"`, `L"acceptance.update.signed-out-button"`), developer)
		}},
		{"one automation id on both banners' actions", `want exactly one L"acceptance.update.action"`, func() []string {
			return checkUpdateBannerAutomationIds(xaml, replace(window, `L"acceptance.update.signed-out-action"`, `L"acceptance.update.action"`), developer)
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
			t.Logf("fires: %s", tc.name)
		})
	}
}

func TestUpdateOfferBannerRowStaysOnScreen(t *testing.T) {
	reportProblems(t, checkUpdateBannerRowStaysOnScreen(readAppSource(t, "MainWindow.xaml"),
		stripComments(readAppSource(t, "ConnectPage.cpp"))))
}

// Each way the banner's row can be scrolled away, or the pane lose its room,
// fails the check for the reason it names.
func TestUpdateOfferBannerRowRejectsWeakerSources(t *testing.T) {
	markup := readAppSource(t, "MainWindow.xaml")
	connect := stripComments(readAppSource(t, "ConnectPage.cpp"))
	replace := func(text, old, replacement string) string {
		if strings.Count(text, old) != 1 {
			t.Fatalf("negative control: %q is not in the source exactly once", old)
		}
		return strings.Replace(text, old, replacement, 1)
	}
	start := strings.Index(markup, `<InfoBar x:Name="UpdateBar"`)
	if start < 0 {
		t.Fatal("MainWindow.xaml has no UpdateBar element to move")
	}
	element := markup[start : start+strings.Index(markup[start:], "/>")+2]
	column := "                                <!-- provide ....................................... -->\n"
	row := func(markup, connect string) func() []string {
		return func() []string { return checkUpdateBannerRowStaysOnScreen(markup, connect) }
	}
	for _, tc := range []struct {
		name, want string
		check      func() []string
	}{
		{"the sources as they are", "", row(markup, connect)},
		{"the banner back in the scrolling column", `UpdateBar is inside the ScrollViewer "PaneAScroll"`,
			row(replace(replace(markup, element, ""), column, strings.Replace(element, ` Grid.Row="2"`, "", 1)+"\n"+column), connect)},
		{"the banner in the scroller's row", `ScrollViewer "PaneAScroll" shares the update banner's row 1`,
			row(replace(markup, element, strings.Replace(element, `Grid.Row="2"`, `Grid.Row="1"`, 1)), connect)},
		{"the banner over the pane's header", "is not under the scroller's row",
			row(replace(markup, element, strings.Replace(element, `Grid.Row="2"`, `Grid.Row="0"`, 1)), connect)},
		{"the banner's row taking what is left", "want a row of its own, sized to it (Auto)",
			row(replace(markup, "                            <!-- the update banner, under the scroller (UpdateBar) -->\n                            <RowDefinition Height=\"Auto\" />\n",
				"                            <RowDefinition Height=\"*\" />\n"), connect)},
		{"the banner without its row", "want a row of its own, sized to it (Auto)",
			row(replace(markup, "                            <!-- the update banner, under the scroller (UpdateBar) -->\n                            <RowDefinition Height=\"Auto\" />\n", ""), connect)},
		{"a closed banner that keeps its margins", "is not closed and collapsed until it has something to say",
			row(replace(markup, element, strings.Replace(element, `Visibility="Collapsed" `, "", 1)), connect)},
		{"a banner the connect page leaves showing once closed", `ConnectPage::ApplyUpdateChecker is missing bar\\.Visibility`,
			row(markup, replace(connect, "  bar.Visibility(bar.IsOpen() ? winrt::Microsoft::UI::Xaml::Visibility::Visible\n                              : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);\n", ""))},
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
			t.Logf("fires: %s", tc.name)
		})
	}
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
		{"the tray takes its feed from app_prefs.json", `UpdateChecker::RunCheck is missing const update::Feed& feed = \*followed;`, func() []string {
			return checkBothAskForTheSameList(replace(checker, "const update::Feed& feed = *followed;",
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
		{"a Later link that is not on the banner", "MainWindow's update banner is missing actions", func() []string {
			return checkTrayLater(checker, header, replace(window, "    actions.Children().Append(later);\n", ""), connect)
		}},
		{"a row that is not on the banner", "MainWindow's update banner is missing UpdateBar", func() []string {
			return checkTrayLater(checker, header, replace(window, "    UpdateBar().Content(actions);\n", ""), connect)
		}},
		{"Later on a row of its own under the action", "puts a control in the update banner's own action slot", func() []string {
			return checkTrayLater(checker, header, replace(window, "    actions.Children().Append(update);\n",
				"    UpdateBar().ActionButton(update);\n"), connect)
		}},
		{"the banner's button written through the bar's empty action slot", "writes the banner's button through the bar's action slot", func() []string {
			return checkTrayLater(checker, header, window, replace(connect,
				"  if (auto button = actionButton) {\n    button.Content(winrt::box_value(action));\n",
				"  if (auto button = bar.ActionButton()) {\n    button.Content(winrt::box_value(action));\n"))
		}},
		{"the warning's button never written", "writes the banner's button in 1 places", func() []string {
			return checkTrayLater(checker, header, window, replace(connect,
				"    if (auto button = actionButton) {\n      button.Content(winrt::box_value(Adv(\"dev_check_updates\", L\"Check for updates\")));\n",
				"    if (false) {\n      auto button = w_.UpdateBar();\n"))
		}},
		{"the action and Later taken in the other order", "ConnectPage::RenderUpdateBanner is missing actionButton = controls", func() []string {
			return checkTrayLater(checker, header, window, replace(connect,
				"      actionButton = controls.GetAt(0).try_as<winrt::Microsoft::UI::Xaml::Controls::Button>();\n      later = controls.GetAt(1);\n",
				"      actionButton = controls.GetAt(1).try_as<winrt::Microsoft::UI::Xaml::Controls::Button>();\n      later = controls.GetAt(0);\n"))
		}},
		{"a Later link that is always shown", "ConnectPage::RenderUpdateBanner is missing later", func() []string {
			return checkTrayLater(checker, header, window, replace(connect,
				"later.Visibility(urnw::UpdateChecker::OffersLater(snap)", "later.Visibility(true"))
		}},
		{"a Later link left up when the banner closes", `ConnectPage::RenderUpdateBanner is missing if \(snap\.phase == Phase::None\)`, func() []string {
			moved := replace(connect, "  if (later) {\n"+
				"    later.Visibility(urnw::UpdateChecker::OffersLater(snap)\n"+
				"                         ? winrt::Microsoft::UI::Xaml::Visibility::Visible\n"+
				"                         : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);\n  }\n", "")
			moved = replace(moved, "  winrt::hstring title{urnw::Format(\"upd_available_title_version\", snap.version)};\n",
				"  if (later) {\n"+
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
				"    stale = update::OfferMayHaveChanged(*offer.feed, offerServerUnix_, sinceCheck);\n",
				"    stale = update::OfferMayHaveChanged(*offer.feed, offerServerUnix_, sinceCheck);\n  }\n  {\n    std::lock_guard lock(mutex_);\n"))
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
		{"the click asked about the official feed whatever the offer's", `UpdateChecker::RunApply is missing stale = update::OfferMayHaveChanged\(\*offer\.feed`, func() []string {
			return again(within(offerRunApply, "stale = update::OfferMayHaveChanged(*offer.feed,",
				"stale = update::OfferMayHaveChanged(update::kOfficialFeed,"))
		}},
		{"Show file asked about the official feed whatever the offer's", `UpdateChecker::RunReveal is missing update::OfferMayHaveChanged\(\*offer_\.feed`, func() []string {
			return again(within(offerRunReveal, "update::OfferMayHaveChanged(*offer_.feed,",
				"update::OfferMayHaveChanged(update::kOfficialFeed,"))
		}},
		{"Show file never taken for an old offer", "UpdateChecker::RunReveal is missing stale = !asked &&", func() []string {
			return again(within(offerRunReveal,
				"      stale = !asked && update::OfferMayHaveChanged(*offer_.feed, offerServerUnix_, sinceCheck);\n",
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
		{"a changed version with nothing said", `ConnectPage::RenderUpdateBanner is missing if \(urnw::UpdateChecker::SaysReplaced\(snap\)\)`, func() []string {
			return checkTrayChecksAgainBeforeItActs(checker, window,
				replace(connect, "if (urnw::UpdateChecker::SaysReplaced(snap)) {", "if (false) {"))
		}},
		{"the check shown as starting the installer", "ConnectPage::RenderUpdateBanner is missing case Stage::Checking:", func() []string {
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
		{"the update check back in the intro's row of actions", "the update check is in the intro's row of actions", func() []string {
			return checkUpdateCheckHasItsOwnCard(replace(developer,
				"    body.Children().Append(checkUpdates);\n", "    actions.Children().Append(checkUpdates);\n"))
		}},
		{"the Updates card gated by the session", "the Updates card is gated (deviceCards_)", func() []string {
			return checkUpdateCheckHasItsOwnCard(replace(developer,
				"    Border card = MakeCard(Loc(\"dev_updates_heading\"), body);\n",
				"    Border card = MakeCard(Loc(\"dev_updates_heading\"), body);\n    deviceCards_.push_back(card);\n"))
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
		{"the README dates the releases that do not update themselves", "dates the releases that do not update themselves (2026-10-08): the first release that updates itself is v2026.10.9-1067985620, published on 2026-10-10", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "A release built before the updater polled these releases does not look for\nupdates here,",
				"A release published on or before 2026-10-08 does not look for updates here,"), selection)
		}},
		{"the README names the last release that does not update itself by its tag", "names a release (v2026.10.8-1066946420) where it says which do not update themselves: the first release that updates itself is v2026.10.9-1067985620, published on 2026-10-10", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "A release built before the updater polled these releases does not look for\nupdates here,",
				"Running v2026.10.8-1066946420 or an earlier release? It does not look for updates here,"), selection)
		}},
		{"the README names another release as the first that updates itself", "names a release (v2026.10.9-1067985622) where it says which do not update themselves: the first release that updates itself is v2026.10.9-1067985620, published on 2026-10-10", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "The first release that does is\nv2026.10.9-1067985620,", "The first release that does is\nv2026.10.9-1067985622,"), selection)
		}},
		{"the README gives the first release that updates itself another date", "dates the releases that do not update themselves (2026-10-09): the first release that updates itself is v2026.10.9-1067985620, published on 2026-10-10", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "published on 2026-10-10.", "published on 2026-10-09."), selection)
		}},
		{"the README gives the first release that updates itself a later date", "dates the releases that do not update themselves (2026-10-11): the first release that updates itself is v2026.10.9-1067985620, published on 2026-10-10", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "published on 2026-10-10.", "published on 2026-10-11."), selection)
		}},
		{"the README gives the first release that updates itself another date, in words", "does not say how to tell that a copy is one of them", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "published on 2026-10-10.", "published on October 9, 2026."), selection)
		}},
		{"the README names the first release that updates itself in other words", "does not say how to tell that a copy is one of them", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "The first release that does is\n", "The earliest that does is\n"), selection)
		}},
		{"the README names the first release that updates itself with a capital V", "does not say how to tell that a copy is one of them", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "is\nv2026.10.9-1067985620,", "is\nV2026.10.9-1067985620,"), selection)
		}},
		{"the README names the first release that updates itself with dashes for dots", "does not say how to tell that a copy is one of them", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "is\nv2026.10.9-1067985620,", "is\nv2026-10-9-1067985620,"), selection)
		}},
		{"the README names the first release that updates itself without its v", "names a release (2026.10.9-1067985620) where it says which do not update themselves: the first release that updates itself is v2026.10.9-1067985620, published on 2026-10-10", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "is\nv2026.10.9-1067985620,", "is\n2026.10.9-1067985620,"), selection)
		}},
		{"the README tells an older copy in other words", "does not say how to tell that a copy is one of them", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "If yours was installed from an\nolder release,", "If yours is older than\nthat,"), selection)
		}},
		{"the README dates the releases that do not update themselves by the pinned day", "dates the releases that do not update themselves (2026-10-10): the first release that updates itself is v2026.10.9-1067985620, published on 2026-10-10, and only the sentence that names it gives a date or a tag", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "and updates itself from then on.\n",
				"and updates itself from then on. Releases published on or before 2026-10-10 never do.\n"), selection)
		}},
		{"the README names another release in the sentence after the one that names the first", "names a release (v2026.10.8-1066946420) where it says which do not update themselves", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "If yours was installed from an\nolder release, install",
				"If yours was installed from an\nolder release, such as v2026.10.8-1066946420, install"), selection)
		}},
		{"the README names the first release that updates itself a second time", "names a release (v2026.10.9-1067985620) where it says which do not update themselves: the first release that updates itself is v2026.10.9-1067985620, published on 2026-10-10, and only the sentence that names it gives a date or a tag", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "and updates itself from then on.\n",
				"and updates itself from then on. Releases up to v2026.10.9-1067985620 never do.\n"), selection)
		}},
		{"the README brings back the three-day rule", "brings back the three-day rule (newer than yours has been listed for three days): Android pre-releases are listed beside each MSI release under later codes, so it sends a copy that is already current to reinstall", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "If yours was installed from an\nolder release, install",
				"If yours was installed from an\nolder release, or if the app has offered nothing while a release newer than\nyours has been listed for three days, install"), selection)
		}},
		{"the README brings back the three-day rule in the words this check once used", "brings back the three-day rule (newer release has been listed for three days)", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "If yours was installed from an\nolder release, install",
				"If yours was installed from an\nolder release, or if there is nothing offered while a newer release has been listed for three days, install"), selection)
		}},
		{"the README brings back the three-day rule by another wait", "brings back the three-day rule (newer than yours has been out for 72 hours)", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "If yours was installed from an\nolder release, install",
				"If yours was installed from an\nolder release, or if a release newer than yours has been out for 72 hours and nothing was offered, install"), selection)
		}},
		{"the README brings back the three-day rule with the wait first", "brings back the three-day rule (Ten days after a newer)", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "and updates itself from then on.\n",
				"and updates itself from then on. Ten days after a newer release is listed, if nothing was offered, install the newest release by hand.\n"), selection)
		}},
		{"the README brings back the three-day rule by its own words, with no number", "brings back the three-day rule (newer than yours has been listed)", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "If yours was installed from an\nolder release, install",
				"If yours was installed from an\nolder release, or if a release newer than yours has been listed for a week with nothing offered, install"), selection)
		}},
		{"the README brings back the three-day rule with a hyphenated wait", "brings back the three-day rule (newer release has gone unoffered for a three-day)", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "and updates itself from then on.\n",
				"and updates itself from then on. If a newer release has gone unoffered for a three-day stretch, install the newest release by hand.\n"), selection)
		}},
		{"the README prefers a release out for one day over a newer one", "", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "the one offered.\n",
				"the one offered. So prefer a release that has been out for one day over a newer one.\n"), selection)
		}},
		{"the README says the updater offers a newer release once it has been out for one day", "", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "never offered. A release that is deleted",
				"never offered. The updater offers a newer release once it has been out for one day. A release that is deleted"), selection)
		}},
		{"the README says a newer release waits one day before it is offered", "", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "never offered. A release that is deleted",
				"never offered. A newer release waits one day before it is offered. A release that is deleted"), selection)
		}},
		{"the README says Windows 10 1809 or newer, and a certificate renewed every 90 days", "", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "\n## Build\n",
				"\nThe installer needs Windows 10 version 1809 or newer, and its signing certificate is renewed every 90 days.\n\n## Build\n"), selection)
		}},
		{"the README says Visual Studio 2022 or newer, and a clean build takes two hours", "", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "\n## Build\n",
				"\nUse Visual Studio 2022 or newer; a clean build of both architectures takes about two hours on a laptop.\n\n## Build\n"), selection)
		}},
		{"the README brings back the three-day rule in a section of its own", "README.md brings back the three-day rule (newer than yours has been listed)", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "\n## Architecture\n", "\n## Troubleshooting\n\nIf the app has offered nothing while a release newer than yours "+
				"has been listed for three days, install the newest release by hand.\n\n## Architecture\n"), selection)
		}},
		{"the README no longer names the first release that updates itself", "does not say how to tell that a copy is one of them", func() []string {
			return checkReadmeNamesTheFeed(replace(readme, "The first release that does is\nv2026.10.9-1067985620, published on 2026-10-10. ", ""), selection)
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
