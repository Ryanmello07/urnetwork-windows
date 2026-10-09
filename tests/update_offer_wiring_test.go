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
// update_release_test.go (Common/ReleaseSelection.h, Common/UpdateSchedule.h):
// which repository is polled, how long a release must have been out, that the
// offer changes at most once a day, and what Later hides. Here: the tray app
// and the update helper ask GitHub for the same list and judge it by its own
// date alone; nothing the app saves takes part in what is offered; Later is
// kept in memory; the tray checks again before it starts the helper once
// GitHub's day has changed; and the README sends people to the feed the code
// polls. The tray and the helper need Windows, so these read their sources
// with every comment blanked. Each check returns the problems it found, so
// the negative controls below can run the same check on a rewritten source
// and require it to fail.

const (
	offerRunCheck = "void UpdateChecker::RunCheck(std::uint64_t generation) {"
	offerRunApply = "void UpdateChecker::RunApply(std::uint64_t generation, bool manual) {"
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
// judges it again with a date of its own.
func checkTheListIsJudgedByItsOwnDate(checker, apply string) []string {
	var problems []string
	check := applyDefinition(checker, offerRunCheck)
	problems = append(problems, applyOrderProblems("UpdateChecker::RunCheck", check,
		regexp.QuoteMeta("update::ParseReleaseList(body);"),
		regexp.QuoteMeta("if (headers.serverUnixSeconds == 0) {"),
		regexp.QuoteMeta("CheckFailed(generation);"),
		regexp.QuoteMeta("return;"),
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
		for _, own := range []string{"HasSoaked(", "SoakEndUnixSeconds(", "UtcDayStart(", "feed.soakSeconds", "publishedAt"} {
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
// check and the apply read none. So "at most one new offer a day" holds
// across restarts with nothing to restore, and a preferences file a user's
// process can write decides nothing about the offer.
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
		"UpdateChecker::RunCheck": offerRunCheck,
		"UpdateChecker::RunApply": offerRunApply,
		"UpdateChecker::Later":    "void UpdateChecker::Later() {",
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
// until the next launch. The release's code is kept in a member that starts
// at 0 and is written in two places, Later and a change of channel, and never
// saved. A check that finds the release again leaves the banner closed, and
// so does dismissing a report about it.
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
	var written []string
	for _, match := range regexp.MustCompile(`laterCode_\s*=[^=][^;]*;`).FindAllString(checker, -1) {
		written = append(written, match)
	}
	if len(written) != 2 || written[0] != "laterCode_ = s.code;" || written[1] != "laterCode_ = 0;" {
		problems = append(problems, fmt.Sprintf("UpdateChecker.cpp writes laterCode_ as %q, want it set in Later and "+
			"cleared on a change of channel, and nowhere else", written))
	}
	changed := applyDefinition(checker, "void UpdateChecker::ChannelChanged() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::ChannelChanged", changed,
		regexp.QuoteMeta("offer_ = Offer{};"),
		regexp.QuoteMeta("laterCode_ = 0;"))...)
	check := applyDefinition(checker, offerRunCheck)
	problems = append(problems, applyOrderProblems("UpdateChecker::RunCheck", check,
		regexp.QuoteMeta("offer_ = offer;"),
		regexp.QuoteMeta("if (update::HiddenByLater(laterCode_, offer.code)) {"),
		regexp.QuoteMeta("} else if (snapshot_.phase == Phase::None || snapshot_.code != offer.code) {"),
		regexp.QuoteMeta("snapshot_.phase = Phase::Available;"))...)
	dismiss := applyDefinition(checker, "void UpdateChecker::DismissResult() {")
	problems = append(problems, applyOrderProblems("UpdateChecker::DismissResult", dismiss,
		regexp.QuoteMeta("if (offer_.code > version::kCode && !update::HiddenByLater(laterCode_, offer_.code)) {"),
		regexp.QuoteMeta("s.phase = Phase::Available;"))...)
	problems = append(problems, applyOrderProblems("MainWindow's update banner", window,
		regexp.QuoteMeta("UpdateBar().ActionButton(update);"),
		regexp.QuoteMeta(`later.Content(winrt::box_value(L"Later"));`),
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

// The helper installs only the release its own list offers, and on the
// official feed that release changes when GitHub's day does. So the offer
// carries the Date header of the check that made it and this machine's steady
// clock then, and once that day has changed, an Update on an installed copy
// runs the check again before the download and the administrator prompt. A
// different offer then stands on the banner and nothing is started; the same
// one goes on. A click made while that check ran, with the banner's button
// still up, starts nothing more. The wall clock, which the user can set,
// takes no part.
func checkTrayChecksAgainWhenTheDayChanged(checker string) []string {
	var problems []string
	check := applyDefinition(checker, offerRunCheck)
	problems = append(problems, applyOrderProblems("UpdateChecker::RunCheck", check,
		regexp.QuoteMeta("offer_ = offer;"),
		regexp.QuoteMeta("offerServerUnix_ = serverUnixSeconds;"),
		regexp.QuoteMeta("offerCheckedAt_ = steady_clock::now();"))...)
	apply := applyDefinition(checker, offerRunApply)
	problems = append(problems, applyOrderProblems("UpdateChecker::RunApply", apply,
		regexp.QuoteMeta("offer = offer_;"),
		regexp.QuoteMeta("offerServerUnix = offerServerUnix_;"),
		regexp.QuoteMeta("offerCheckedAt = offerCheckedAt_;"),
		regexp.QuoteMeta("const bool viaHelper = installed_ && !manual;"),
		regexp.QuoteMeta("std::chrono::duration_cast<std::chrono::seconds>(steady_clock::now() - offerCheckedAt).count();"),
		regexp.QuoteMeta("if (viaHelper && update::OfferMayHaveChanged(update::kOfficialFeed, offerServerUnix, sinceCheck)) {"),
		regexp.QuoteMeta("RunCheck(generation);"),
		regexp.QuoteMeta("applyRequested_ = false;"),
		regexp.QuoteMeta("manualRequested_ = false;"),
		regexp.QuoteMeta("if (feedGeneration_ != generation || offer_.code != offer.code) {"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta("held = viaHelper && steady_clock::now() < holdUntil_;"),
		regexp.QuoteMeta("s.phase = Phase::Applying;"),
		regexp.QuoteMeta("FetchUrl("),
		regexp.QuoteMeta("LaunchUpdateHelper("))...)
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
	return problems
}

// The README's Download section sends people to the releases the updater
// polls: the feed's own repository, by the owner and name compiled in.
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

func TestUpdateOfferTheTrayChecksAgainWhenTheDayChanged(t *testing.T) {
	reportProblems(t, checkTrayChecksAgainWhenTheDayChanged(stripComments(readAppSource(t, "UpdateChecker.cpp"))))
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
	apply := updaterSources(t)["ApplyUpdate.cpp"]
	selection := stripComments(readCommonSource(t, "ReleaseSelection.h"))
	readme := readRepositoryFile(t, "README.md")
	replace := func(text, old, replacement string) string {
		if strings.Count(text, old) != 1 {
			t.Fatalf("negative control: %q is not in the source exactly once", old)
		}
		return strings.Replace(text, old, replacement, 1)
	}
	within := func(opener, old, replacement string) string {
		definition := applyDefinition(checker, opener)
		return replace(checker, definition, replace(definition, old, replacement))
	}
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
				"  if (headers.serverUnixSeconds == 0) {\n    LogWarn(\"update: the release list had no Date header; nothing is offered from it\");\n    CheckFailed(generation);\n    return;\n  }\n",
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
				"  Offer offer;\n  if (!update::HasSoaked(feed, NowUnixSeconds() - 86400, NowUnixSeconds())) return;\n  if (sel.code != 0 &&"),
				apply)
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
		{"a dismissed report puts the release back on the banner", "UpdateChecker::DismissResult is missing", func() []string {
			return checkTrayLater(replace(checker,
				"if (offer_.code > version::kCode && !update::HiddenByLater(laterCode_, offer_.code)) {",
				"if (offer_.code > version::kCode) {"), header, window, connect)
		}},
		{"Later written somewhere else", "laterCode_ = offer.code;", func() []string {
			return checkTrayLater(replace(checker, "      offer_ = offer;\n      offerServerUnix_ = serverUnixSeconds;\n",
				"      offer_ = offer;\n      laterCode_ = offer.code;\n      offerServerUnix_ = serverUnixSeconds;\n"),
				header, window, connect)
		}},
		{"Later carried over a change of channel", "UpdateChecker::ChannelChanged is missing laterCode_ = 0;", func() []string {
			return checkTrayLater(replace(checker, "    laterCode_ = 0;\n", ""), header, window, connect)
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
		{"no second check when the day has changed", `UpdateChecker::RunApply is missing if \(viaHelper && update::OfferMayHaveChanged`, func() []string {
			return checkTrayChecksAgainWhenTheDayChanged(replace(checker,
				"if (viaHelper && update::OfferMayHaveChanged(update::kOfficialFeed, offerServerUnix, sinceCheck)) {",
				"if (false) {"))
		}},
		{"an offer that changed is installed anyway", `UpdateChecker::RunApply is missing if \(feedGeneration_ != generation \|\| offer_\.code != offer\.code\)`, func() []string {
			return checkTrayChecksAgainWhenTheDayChanged(replace(checker,
				"if (feedGeneration_ != generation || offer_.code != offer.code) {",
				"if (feedGeneration_ != generation) {"))
		}},
		{"a click made during the second check starts another update", "UpdateChecker::RunApply is missing applyRequested_ = false;", func() []string {
			return checkTrayChecksAgainWhenTheDayChanged(replace(checker,
				"    applyRequested_ = false;\n    manualRequested_ = false;\n    if (feedGeneration_ != generation || offer_.code != offer.code) {",
				"    if (feedGeneration_ != generation || offer_.code != offer.code) {"))
		}},
		{"the offer's age taken after the download", `UpdateChecker::RunApply is missing if \(viaHelper && update::OfferMayHaveChanged`, func() []string {
			block := "  const std::int64_t sinceCheck =\n" +
				"      std::chrono::duration_cast<std::chrono::seconds>(steady_clock::now() - offerCheckedAt).count();\n"
			moved := replace(checker, block, "")
			return checkTrayChecksAgainWhenTheDayChanged(replace(moved, "  const std::wstring msiW = msiPath.wstring();\n",
				block+"  const std::wstring msiW = msiPath.wstring();\n"))
		}},
		{"the offer's age by the wall clock", "measures the offer's age with NowUnixSeconds", func() []string {
			return checkTrayChecksAgainWhenTheDayChanged(replace(checker,
				"std::chrono::duration_cast<std::chrono::seconds>(steady_clock::now() - offerCheckedAt).count();",
				"NowUnixSeconds() - offerServerUnix;"))
		}},
		{"the offer's date never recorded", "UpdateChecker::RunCheck is missing offerServerUnix_ = serverUnixSeconds;", func() []string {
			return checkTrayChecksAgainWhenTheDayChanged(replace(checker, "      offerServerUnix_ = serverUnixSeconds;\n", ""))
		}},
		{"the offer's date taken from an older check", "UpdateChecker::RunApply is missing offerServerUnix = offerServerUnix_;", func() []string {
			return checkTrayChecksAgainWhenTheDayChanged(replace(checker, "    offerServerUnix = offerServerUnix_;\n", ""))
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
	} {
		t.Run(tc.name, func(t *testing.T) {
			problems := strings.Join(tc.check(), "\n")
			if !strings.Contains(problems, tc.want) {
				t.Fatalf("negative control was not detected by %q; the check reported:\n%s", tc.want, problems)
			}
		})
	}
}
