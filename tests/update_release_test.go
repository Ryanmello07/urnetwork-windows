// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
	"time"
)

// The update's pure decisions, compiled and run by
// app/tools/update-release-tests.cpp against the same headers the tray app and
// the update helper compile: the feed, how long a release must have been out
// and unchanged, and the release it offers (Common/ReleaseSelection.h), what
// becomes of a click on the banner (Common/UpdateSchedule.h), the install locations
// an elevated process may run from (Common/InstallLocation.h), and what the
// helper's exit code means (Common/UpdateResult.h). The MSI ProductVersion a
// release must carry
// (UrMsiVersion) is also checked against UrVersion.ps1's Go oracle
// (ur_version_test.go) on every day boundary the layout can reach.

// The Common headers the program includes.
var updateReleaseHeaders = []string{
	"InstallLocation.h", "ReleaseSelection.h", "UpdateApply.h", "UpdateChannel.h", "UpdateFormats.h",
	"UpdateResult.h", "UpdateSchedule.h", "VersionGrammar.h",
}

// The program, built from the repository's headers, or, when `mutate` is set,
// from a copy of them with `header` rewritten by it, for a negative control.
func updateReleaseTestProgram(t *testing.T, header string, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("update release tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	fixtureDir := t.TempDir()
	common := filepath.Join(root, "app", "src", "Common")
	if mutate != nil {
		copied := filepath.Join(fixtureDir, "Common")
		if err := os.Mkdir(copied, 0700); err != nil {
			t.Fatal(err)
		}
		mutated := false
		for _, name := range updateReleaseHeaders {
			data, err := os.ReadFile(filepath.Join(common, name))
			if err != nil {
				t.Fatal(err)
			}
			source := string(data)
			if name == header {
				changed := mutate(source)
				if changed == source {
					t.Fatalf("negative control did not change %s", header)
				}
				source = changed
				mutated = true
			}
			if err := os.WriteFile(filepath.Join(copied, name), []byte(source), 0600); err != nil {
				t.Fatal(err)
			}
		}
		if !mutated {
			t.Fatalf("%s is not one of the program's headers", header)
		}
		common = copied
	}
	program := filepath.Join(fixtureDir, "update-release-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I"+common,
		filepath.Join(root, "app", "tools", "update-release-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build update release tests: %v\n%s", err, output)
	}
	return program
}

// A file of the oracle's MSI versions, "<code> <version>" per line, or
// "<code> -" for a code past the layout: every fixed vector, the first and
// the last tenth of every day from the founding through the first day the
// layout cannot hold, and every second of one day. The count is returned too.
func updateReleaseOracleVectors(t *testing.T) (string, int) {
	t.Helper()
	var lines strings.Builder
	count := 0
	add := func(code int64) {
		version, err := deriveUrNumbers(code, urPinnedClock)
		switch {
		case err == nil:
			fmt.Fprintf(&lines, "%d %s\n", code, version.Msi)
		case urRefusalClass(err) == urRefuseRange:
			fmt.Fprintf(&lines, "%d -\n", code)
		default:
			t.Fatalf("the oracle refused code %d: %v", code, err)
		}
		count++
	}
	for _, vector := range urFixedVectors {
		fields := strings.SplitN(vector.input, "-", 3)
		code, err := strconv.ParseInt(fields[1], 10, 64)
		if err != nil {
			t.Fatalf("fixed vector %q: %v", vector.input, err)
		}
		add(code)
	}
	days := 0
	for day := urFounded.AddDate(0, 0, 1); !day.After(urPinnedClock); day = day.AddDate(0, 0, 1) {
		code := (day.Unix() - urFounded.Unix()) * 10
		add(code - 1)
		add(code)
		days++
	}
	// 2023-05-24 through 2256-01-01, inclusive
	if days != 84959 {
		t.Fatalf("swept %d day boundaries, want 84959", days)
	}
	start := (time.Date(2026, 10, 1, 0, 0, 0, 0, time.UTC).Unix() - urFounded.Unix()) * 10
	for second := int64(0); second < 86400; second++ {
		add(start + second*10 + second%10)
	}
	path := filepath.Join(t.TempDir(), "msi-versions.txt")
	if err := os.WriteFile(path, []byte(lines.String()), 0600); err != nil {
		t.Fatal(err)
	}
	return path, count
}

func TestUpdateReleaseSelection(t *testing.T) {
	program := updateReleaseTestProgram(t, "", nil)
	vectors, count := updateReleaseOracleVectors(t)
	output, err := exec.Command(program, vectors).CombinedOutput()
	if err != nil {
		t.Fatalf("update release selection: %v\n%s", err, output)
	}
	if !regexp.MustCompile(fmt.Sprintf(`(?m)^  %d oracle vectors\r?$`, count)).Match(output) {
		t.Fatalf("the program did not read all %d oracle vectors:\n%s", count, output)
	}
	t.Logf("%s", output)
}

// The rewritten program must fail, naming the check that caught it. Every
// control builds the program from its own copy of the headers, in its own
// temporary folder, so the controls of one table run side by side.
func requireUpdateReleaseFailure(t *testing.T, header string, mutate func(string) string, want string) {
	t.Helper()
	program := updateReleaseTestProgram(t, header, mutate)
	vectors, _ := updateReleaseOracleVectors(t)
	output, err := exec.Command(program, vectors).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want a failure naming %q): %v\n%s", want, err, output)
	}
}

// Replaces exactly one occurrence of `old`.
func updateReleaseReplace(old, replacement string) func(string) string {
	return func(source string) string {
		if strings.Count(source, old) != 1 {
			return source
		}
		return strings.Replace(source, old, replacement, 1)
	}
}

// The official feed must be urnetwork/build's published releases, addressed
// by that repository's own id, immutable, without prereleases, and out for a
// day: pointing it at a personal fork or at any other repository, or
// loosening it, fails the suite, not just a review.
func TestUpdateReleaseRejectsOtherFeeds(t *testing.T) {
	for _, tc := range []struct {
		name, old, replacement, want string
	}{
		{"a personal fork", `.owner = "urnetwork",
    .repo = "build",`, `.owner = "example-user",
    .repo = "urnetwork-windows",`, "official urnetwork repo"},
		{"a personal fork, by its id too", `.numericRepoId = 936244679,
    .owner = "urnetwork",
    .repo = "build",`, `.numericRepoId = 1,
    .owner = "example-user",
    .repo = "urnetwork-windows",`, "official urnetwork repo"},
		{"a personal fork behind the organisation's names", ".numericRepoId = 936244679,", ".numericRepoId = 1,",
			"urnetwork/build's own id"},
		{"another repository of the organisation", `.repo = "build",`, `.repo = "windows",`,
			"polls urnetwork/build's published releases"},
		{"mutable releases", ".requireImmutable = true,\n    .soakSeconds = 24 * 60 * 60,",
			".requireImmutable = false,\n    .soakSeconds = 24 * 60 * 60,", "requires immutable releases"},
		{"prereleases", `.id = "official",
    .numericRepoId = 936244679,
    .owner = "urnetwork",
    .repo = "build",
    .acceptBetaPrereleases = false,`, `.id = "official",
    .numericRepoId = 936244679,
    .owner = "urnetwork",
    .repo = "build",
    .acceptBetaPrereleases = true,`, "takes no prerelease"},
		{"a release offered the moment it is published", ".soakSeconds = 24 * 60 * 60,", ".soakSeconds = 0,",
			"soaks a release for 24 hours"},
		{"an hour's soak", ".soakSeconds = 24 * 60 * 60,", ".soakSeconds = 60 * 60,",
			"soaks a release for 24 hours"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			requireUpdateReleaseFailure(t, "ReleaseSelection.h", updateReleaseReplace(tc.old, tc.replacement), tc.want)
		})
	}
}

// The developer feed must be Ryanmello07/urnetwork-windows's -beta releases,
// addressed by that repository's own id and spelled as GitHub spells it,
// immutable, with no other prerelease, no prefix and no soak, and the second
// row of a table of two: pointing it elsewhere, loosening it or adding a row
// fails the suite.
func TestUpdateReleaseRejectsAnotherDeveloperFeed(t *testing.T) {
	for _, tc := range []struct {
		name, header, old, replacement, want string
	}{
		{"a third row", "ReleaseSelection.h", "inline constexpr Feed kFeeds[] = {kOfficialFeed, kDeveloperFeed};",
			`inline constexpr Feed kFeeds[] = {kOfficialFeed, kDeveloperFeed, Feed{.id = "third", .numericRepoId = 4, ` +
				`.owner = "example", .repo = "example", .acceptBetaPrereleases = false, .requireImmutable = true, ` +
				`.soakSeconds = 0, .tagPrefix = "", .acceptAnyPrerelease = false}};`,
			"the table holds the official and the developer feed alone"},
		{"the row under another id", "ReleaseSelection.h", `.id = "developer",`, `.id = "developer-builds",`,
			"the developer feed is in the table"},
		{"another repository's id", "ReleaseSelection.h", ".numericRepoId = 1324127805,", ".numericRepoId = 1324127806,",
			"the developer feed is repository 1324127805"},
		{"urnetwork/build", "ReleaseSelection.h", `.numericRepoId = 1324127805,
    .owner = "Ryanmello07",
    .repo = "urnetwork-windows",`, `.numericRepoId = 936244679,
    .owner = "urnetwork",
    .repo = "build",`, "the developer feed is repository 1324127805"},
		{"the owner in lower case", "ReleaseSelection.h", `.owner = "Ryanmello07",`, `.owner = "ryanmello07",`,
			"Ryanmello07/urnetwork-windows's, spelled as GitHub spells it"},
		{"no -beta prereleases", "ReleaseSelection.h", `.repo = "urnetwork-windows",
    .acceptBetaPrereleases = true,`, `.repo = "urnetwork-windows",
    .acceptBetaPrereleases = false,`, "the developer feed takes -beta prereleases"},
		{"any prerelease", "ReleaseSelection.h", `.soakSeconds = 0,
    .tagPrefix = "",
    .acceptAnyPrerelease = false,`, `.soakSeconds = 0,
    .tagPrefix = "",
    .acceptAnyPrerelease = true,`, "the developer feed takes no other prerelease"},
		{"a tag prefix", "ReleaseSelection.h", `.soakSeconds = 0,
    .tagPrefix = "",`, `.soakSeconds = 0,
    .tagPrefix = "beta-",`, "the developer feed's tags carry no prefix"},
		{"mutable releases", "ReleaseSelection.h", ".requireImmutable = true,\n    .soakSeconds = 0,",
			".requireImmutable = false,\n    .soakSeconds = 0,", "the developer feed requires immutable releases"},
		{"a day's soak", "ReleaseSelection.h", ".requireImmutable = true,\n    .soakSeconds = 0,",
			".requireImmutable = true,\n    .soakSeconds = 24 * 60 * 60,", "the developer feed does not soak"},
		{"any tag read", "ReleaseSelection.h",
			"    if (feed.acceptBetaPrereleases && !selection_detail::EndsWith(rel.tag, \"-beta\")) continue;\n", "",
			"the developer feed reads only tags that end -beta"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			requireUpdateReleaseFailure(t, tc.header, updateReleaseReplace(tc.old, tc.replacement), tc.want)
		})
	}
}

// ReleaseSelection.h without the developer feed, as the revert of the commit
// that added it leaves it: no row, no comment above it, and no entry in kFeeds.
func withoutDeveloperFeed(t *testing.T) func(string) string {
	return func(source string) string {
		row := regexp.MustCompile(`(?s)// The opt-in developer channel's feed: .*?\ninline constexpr Feed kDeveloperFeed\{\n.*?\n\};\n\n`)
		if found := len(row.FindAllStringIndex(source, -1)); found != 1 {
			t.Fatalf("ReleaseSelection.h has %d kDeveloperFeed rows to take out, want one", found)
		}
		source = row.ReplaceAllString(source, "")
		const entry = "inline constexpr Feed kFeeds[] = {kOfficialFeed, kDeveloperFeed};"
		if strings.Count(source, entry) != 1 {
			t.Fatalf("ReleaseSelection.h's kFeeds is not %q", entry)
		}
		source = strings.Replace(source, entry, "inline constexpr Feed kFeeds[] = {kOfficialFeed};", 1)
		if strings.Contains(source, "kDeveloperFeed") {
			t.Fatal("ReleaseSelection.h names kDeveloperFeed outside its row and its entry in kFeeds")
		}
		return source
	}
}

// The developer feed comes out in one change, the revert of the commit that
// added it: its row and its entry in kFeeds (SPEC-A2c 3.1). Nothing else names
// the row, so the program built from a ReleaseSelection.h without them, and
// run with --feed-taken-out, which leaves out the row's own checks, passes.
// It names each check it made of what is left: a trusted marker is the
// official channel, "developer" is no channel argument, and "official" still
// is one and still deletes a marker. Both other ways round fail: that program
// run as if the table had the row, and the table as it is run as if the row
// were out.
func TestUpdateReleaseTheFeedTakenOut(t *testing.T) {
	takenOut := updateReleaseTestProgram(t, "ReleaseSelection.h", withoutDeveloperFeed(t))
	output, err := exec.Command(takenOut, "--feed-taken-out").CombinedOutput()
	if err != nil {
		t.Fatalf("the program without the developer feed failed: %v\n%s", err, output)
	}
	for _, checked := range []string{
		"the developer row is taken out of the table, and the official row is left",
		"a trusted marker is the official channel while the table has no developer row",
		"official is a channel argument",
		"developer is a channel argument exactly while the table has its row",
		"official deletes a trusted marker",
	} {
		if !regexp.MustCompile(`(?m)^  taken out: ` + regexp.QuoteMeta(checked) + `\r?$`).Match(output) {
			t.Errorf("the program without the developer feed did not check that %s:\n%s", checked, output)
		}
	}
	t.Logf("%s", output)
	if output, err := exec.Command(takenOut).CombinedOutput(); err == nil ||
		!strings.Contains(string(output), "FAIL the developer feed is in the table") {
		t.Errorf("the program without the developer feed passed as if the table had the row, "+
			"want a failure naming the developer feed: %v\n%s", err, output)
	}
	full := updateReleaseTestProgram(t, "", nil)
	if output, err := exec.Command(full, "--feed-taken-out").CombinedOutput(); err == nil ||
		!strings.Contains(string(output), "FAIL the developer row is taken out of the table") {
		t.Errorf("the program with the developer feed passed as if it were taken out, "+
			"want a failure naming the row: %v\n%s", err, output)
	}
}

// Each rule the elevated side relies on fails a named check when it is taken
// out or loosened.
func TestUpdateReleaseRejectsWeakerDecisions(t *testing.T) {
	for _, tc := range []struct {
		name, header, old, replacement, want string
	}{
		{"mutable releases offered", "ReleaseSelection.h",
			"if (feed.requireImmutable && !rel.immutable) {", "if (false) {",
			"is not offered on the official feed"},
		{"no future-code cap", "ReleaseSelection.h",
			"if (CodeUnixSeconds(tag->code) > serverUnixSeconds + kFutureCodeLimitSeconds) {",
			"if (CodeUnixSeconds(tag->code) > serverUnixSeconds + kFutureCodeLimitSeconds * 1000000) {",
			"more than 48 h past the server's date is not offered"},
		{"the cap against a day", "ReleaseSelection.h",
			"inline constexpr std::int64_t kFutureCodeLimitSeconds = 48 * 60 * 60;",
			"inline constexpr std::int64_t kFutureCodeLimitSeconds = 24 * 60 * 60;",
			"exactly 48 h past the server's date is offered"},
		{"newest counts every release", "ReleaseSelection.h",
			"    if (!match || match->url.empty()) {\n      s.skipped.push_back({rel.tag, \"lacks \" + name, false});\n      continue;\n    }\n",
			"    if (tag->code > s.newestCode) {\n      s.newestCode = tag->code;\n      s.newestVersion = tag->version;\n    }\n    if (!match || match->url.empty()) {\n      s.skipped.push_back({rel.tag, \"lacks \" + name, false});\n      continue;\n    }\n",
			"newest counts only releases carrying this product's MSI"},
		{"prereleases taken anywhere", "ReleaseSelection.h",
			"if (rel.prerelease && !feed.acceptAnyPrerelease &&", "if (false && rel.prerelease && !feed.acceptAnyPrerelease &&",
			"skips every prerelease"},
		{"the soak never applied", "ReleaseSelection.h",
			"if (!HasSoaked(feed, since.unixSeconds, serverUnixSeconds)) {", "if (false) {",
			"23 h 59 m old when the day began is not offered"},
		{"the soak judged at the list's own second", "ReleaseSelection.h",
			"return *sinceUnixSeconds + feed.soakSeconds <= UtcDayStart(serverUnixSeconds);",
			"return *sinceUnixSeconds + feed.soakSeconds <= serverUnixSeconds;",
			"does not change during one server day"},
		{"the soak judged at the day's end", "ReleaseSelection.h",
			"return *sinceUnixSeconds + feed.soakSeconds <= UtcDayStart(serverUnixSeconds);",
			"return *sinceUnixSeconds + feed.soakSeconds <= UtcDayStart(serverUnixSeconds) + kSecondsPerDay;",
			"a release whose 24 hours end during the day waits for the next day"},
		{"23 h 59 m is enough", "ReleaseSelection.h",
			"return *sinceUnixSeconds + feed.soakSeconds <= UtcDayStart(serverUnixSeconds);",
			"return *sinceUnixSeconds + feed.soakSeconds - 60 <= UtcDayStart(serverUnixSeconds);",
			"23 h 59 m old when the day began is not offered"},
		{"24 h exactly is not enough", "ReleaseSelection.h",
			"return *sinceUnixSeconds + feed.soakSeconds <= UtcDayStart(serverUnixSeconds);",
			"return *sinceUnixSeconds + feed.soakSeconds < UtcDayStart(serverUnixSeconds);",
			"24 h old when the day began is offered"},
		{"a release without a publication time counted", "ReleaseSelection.h",
			"if (!sinceUnixSeconds || serverUnixSeconds <= 0) return false;",
			"if (serverUnixSeconds <= 0) return false;\n  if (!sinceUnixSeconds) return true;",
			"without a publication time is never offered"},
		{"a list without a date counted", "ReleaseSelection.h",
			"if (!sinceUnixSeconds || serverUnixSeconds <= 0) return false;",
			"if (!sinceUnixSeconds) return false;",
			"a list with no date counts no release"},
		{"a release dated after the list counted", "ReleaseSelection.h",
			"return *sinceUnixSeconds + feed.soakSeconds <= UtcDayStart(serverUnixSeconds);",
			"return *sinceUnixSeconds + feed.soakSeconds <= UtcDayStart(serverUnixSeconds) ||\n" +
				"         *sinceUnixSeconds > serverUnixSeconds;",
			"a release published after the list's date is not offered"},
		{"the soak counted from the publication alone", "ReleaseSelection.h",
			"return {std::max({*published, *changed, *uploaded}), {}};",
			"return {*published, {}};",
			"a release published a week ago and changed an hour ago is not offered"},
		{"the release's last change not counted", "ReleaseSelection.h",
			"return {std::max({*published, *changed, *uploaded}), {}};",
			"return {std::max({*published, *uploaded}), {}};",
			"a release published a week ago and changed an hour ago is not offered"},
		{"the package's upload not counted", "ReleaseSelection.h",
			"return {std::max({*published, *changed, *uploaded}), {}};",
			"return {std::max({*published, *changed}), {}};",
			"a package uploaded an hour ago is not offered"},
		{"the earliest of the three times taken", "ReleaseSelection.h",
			"return {std::max({*published, *changed, *uploaded}), {}};",
			"return {std::min({*published, *changed, *uploaded}), {}};",
			"a release 23 h 59 m old when the day began is not offered"},
		{"a release without the time of its last change counted", "ReleaseSelection.h",
			"const std::optional<std::int64_t> changed = ParseUtcSecond(updatedAt);",
			"const std::optional<std::int64_t> changed = ParseUtcSecond(updatedAt).value_or(*published);",
			"a release without a readable time of its last change is never offered"},
		{"a package without an upload time counted", "ReleaseSelection.h",
			"const std::optional<std::int64_t> uploaded = ParseUtcSecond(assetUpdatedAt);",
			"const std::optional<std::int64_t> uploaded = ParseUtcSecond(assetUpdatedAt).value_or(*published);",
			"nor one whose package has no readable upload time"},
		{"a release published before it was built counted", "ReleaseSelection.h",
			"if (*published < CodeUnixSeconds(code) - kFutureCodeLimitSeconds) {",
			"if (*published < CodeUnixSeconds(code) - kFutureCodeLimitSeconds * 100000) {",
			"a release published before it was built is never offered: 1970-01-01T00:00:00Z"},
		{"a publication a day before the build refused", "ReleaseSelection.h",
			"if (*published < CodeUnixSeconds(code) - kFutureCodeLimitSeconds) {",
			"if (*published < CodeUnixSeconds(code) - kFutureCodeLimitSeconds / 2) {",
			"a publication time 48 h before the code's instant is within a build clock's skew"},
		{"a publication four days before the build taken", "ReleaseSelection.h",
			"if (*published < CodeUnixSeconds(code) - kFutureCodeLimitSeconds) {",
			"if (*published < CodeUnixSeconds(code) - kFutureCodeLimitSeconds * 2) {",
			"one second more is not"},
		{"a release still inside its day logged as a fault", "ReleaseSelection.h",
			"               \" h when the server's day began\",\n           true});",
			"               \" h when the server's day began\",\n           false});",
			"a release only its day holds back is marked as that"},
		{"a release without the MSI logged as only waiting", "ReleaseSelection.h",
			"s.skipped.push_back({rel.tag, \"lacks \" + name, false});",
			"s.skipped.push_back({rel.tag, \"lacks \" + name, true});",
			"and none of them as only waiting for its day"},
		{"a day its month does not have read as the next month's", "UpdateResult.h",
			"if (day > DaysInMonth(year, month)) return std::nullopt;",
			"if (day > 31) return std::nullopt;",
			"not a day of its month: 2026-02-29T00:00:00Z"},
		{"a leap day every year", "UpdateResult.h",
			"const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);",
			"const bool leap = year != 0;",
			"not a day of its month: 2026-02-29T00:00:00Z"},
		{"a century taken for a leap year", "UpdateResult.h",
			"const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);",
			"const bool leap = year % 4 == 0;",
			"not a day of its month: 2100-02-29T00:00:00Z"},
		{"no leap day at all", "UpdateResult.h",
			"const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);",
			"const bool leap = year < 0;",
			"a leap day in a leap year, read as themselves"},
		{"a day of twelve hours", "ReleaseSelection.h",
			"inline constexpr std::int64_t kSecondsPerDay = 24 * 60 * 60;",
			"inline constexpr std::int64_t kSecondsPerDay = 12 * 60 * 60;",
			"a UTC day runs from its first second"},
		{"a release still soaking named as the newest", "ReleaseSelection.h",
			"    const SoakStart since =\n",
			"    if (tag->code > s.newestCode) {\n      s.newestCode = tag->code;\n      s.newestVersion = tag->version;\n    }\n" +
				"    const SoakStart since =\n",
			"a release still soaking is not named as the newest"},
		{"the held-back release said to count a day early", "ReleaseSelection.h",
			"return day == soaked ? soaked : day + kSecondsPerDay;", "return day;",
			"the first second HasSoaked holds"},
		{"the held-back release not named", "ReleaseSelection.h",
			"if (tag->code > s.waitingCode) {", "if (false && tag->code > s.waitingCode) {",
			"is named as waiting"},
		{"a release of the other platforms named as held back", "ReleaseSelection.h",
			"    if (!match || match->url.empty()) {\n      s.skipped.push_back({rel.tag, \"lacks \" + name, false});\n      continue;\n    }\n",
			"    if (const std::optional<std::int64_t> published = ParseUtcSecond(rel.publishedAt);\n" +
				"        published && !HasSoaked(feed, published, serverUnixSeconds) && tag->code > s.waitingCode) {\n" +
				"      s.waitingCode = tag->code;\n      s.waitingVersion = tag->version;\n" +
				"      s.waitingFromUnixSeconds = SoakEndUnixSeconds(feed, *published);\n    }\n" +
				"    if (!match || match->url.empty()) {\n      s.skipped.push_back({rel.tag, \"lacks \" + name, false});\n      continue;\n    }\n",
			"with this product's MSI that the soak holds back is named as waiting"},
		{"a page of fifteen releases", "ReleaseSelection.h",
			"inline constexpr int kReleaseListPageSize = 30;", "inline constexpr int kReleaseListPageSize = 15;",
			"one page of the list holds two days of the pipeline's builds"},
		{"the list asked for by the repository's name", "ReleaseSelection.h",
			`return "https://api.github.com/repositories/" + std::to_string(feed.numericRepoId) +`,
			`return "https://api.github.com/repos/" + std::string(feed.owner) + "/" + std::string(feed.repo) +`,
			"by the repository's id"},
		{"the day's change not noticed before a click is acted on", "ReleaseSelection.h",
			"  return UtcDayStart(serverUnixSecondsAtCheck + secondsSince) !=\n         UtcDayStart(serverUnixSecondsAtCheck);",
			"  return false;",
			"an offer made before GitHub's day changed is checked again, however fresh"},
		{"an old offer acted on without a check", "ReleaseSelection.h",
			"if (secondsSince < 0 || secondsSince > kOfferFreshSeconds) return true;",
			"if (secondsSince < 0) return true;",
			"an offer older than five minutes is checked again"},
		{"an offer taken as fresh for an hour", "ReleaseSelection.h",
			"inline constexpr std::int64_t kOfferFreshSeconds = 5 * 60;",
			"inline constexpr std::int64_t kOfferFreshSeconds = 60 * 60;",
			"an offer is taken as fresh for five minutes"},
		{"a second check before every click", "ReleaseSelection.h",
			"if (secondsSince < 0 || secondsSince > kOfferFreshSeconds) return true;",
			"if (secondsSince < 0 || secondsSince >= 0) return true;",
			"a fresh offer made the same day needs no second check"},
		{"a feed without a soak never checked again", "ReleaseSelection.h",
			"  if (secondsSince < 0 || secondsSince > kOfferFreshSeconds) return true;\n  if (feed.soakSeconds <= 0 || serverUnixSecondsAtCheck <= 0) return false;\n",
			"  if (feed.soakSeconds <= 0 || serverUnixSecondsAtCheck <= 0) return false;\n  if (secondsSince < 0 || secondsSince > kOfferFreshSeconds) return true;\n",
			"any feed's release can be withdrawn"},
		{"a clock that ran backwards taken for a fresh offer", "ReleaseSelection.h",
			"if (secondsSince < 0 || secondsSince > kOfferFreshSeconds) return true;",
			"if (secondsSince > kOfferFreshSeconds) return true;",
			"a clock that ran backwards since the check"},
		{"an offer that changed is installed anyway", "UpdateSchedule.h",
			"  if (click.checked && click.offeredCode != click.clickedCode) return ClickStep::Replaced;\n", "",
			"an offer that changed waits for a click of its own"},
		{"a withdrawn release is started", "UpdateSchedule.h",
			"  if (click.checked && click.offeredCode == 0) return ClickStep::Withdrawn;\n",
			"  if (click.checked && click.offeredCode == 0) return ClickStep::Proceed;\n",
			"a release the feed no longer offers is not started"},
		{"Later undone by a click in flight", "UpdateSchedule.h",
			"if (!click.bannerHolds || HiddenByLater(click.laterCode, click.clickedCode)) {",
			"if (!click.bannerHolds) {",
			"Later chosen while the check ran is not undone by the click"},
		{"a click that goes on after its banner moved on", "UpdateSchedule.h",
			"if (!click.bannerHolds || HiddenByLater(click.laterCode, click.clickedCode)) {",
			"if (HiddenByLater(click.laterCode, click.clickedCode)) {",
			"a banner that moved on drops the click"},
		{"the banner asked before what the feed offers now", "UpdateSchedule.h",
			"  if (click.checked && click.offeredCode == 0) return ClickStep::Withdrawn;\n",
			"  if (!click.bannerHolds) return ClickStep::Dropped;\n" +
				"  if (click.checked && click.offeredCode == 0) return ClickStep::Withdrawn;\n",
			"a release whose banner the check has already shut is said to be withdrawn"},
		{"a replaced release said to be replaced only while its banner stands", "UpdateSchedule.h",
			"if (click.checked && click.offeredCode != click.clickedCode) return ClickStep::Replaced;",
			"if (click.checked && click.bannerHolds && click.offeredCode != click.clickedCode) return ClickStep::Replaced;",
			"a release the check has already put on the banner is said to have replaced the clicked one"},
		{"a click that survives a change of channel", "UpdateSchedule.h",
			"if (!click.sameFeed || click.clickedCode == 0) return ClickStep::Dropped;",
			"if (click.clickedCode == 0) return ClickStep::Dropped;",
			"a change of channel drops the click"},
		{"an installer shown without asking GitHub", "UpdateSchedule.h",
			"  if (!click.checked && !click.viaHelper) return ClickStep::Unconfirmed;\n", "",
			"an installer is not shown when GitHub could not be asked"},
		{"the helper kept from asking GitHub itself", "UpdateSchedule.h",
			"if (!click.checked && !click.viaHelper) return ClickStep::Unconfirmed;",
			"if (!click.checked) return ClickStep::Unconfirmed;",
			"the helper asks GitHub itself"},
		{"a check that failed read as a withdrawal", "UpdateSchedule.h",
			"if (click.checked && click.offeredCode == 0) return ClickStep::Withdrawn;",
			"if (click.offeredCode == 0) return ClickStep::Withdrawn;",
			"a check that could not be made says nothing about what the feed offers"},
		{"a check that failed read as another offer", "UpdateSchedule.h",
			"if (click.checked && click.offeredCode != click.clickedCode) return ClickStep::Replaced;",
			"if (click.offeredCode != click.clickedCode) return ClickStep::Replaced;",
			"a check that could not be made says nothing about what the feed offers"},
		{"Later hides every release", "UpdateSchedule.h",
			"return laterCode != 0 && offeredCode == laterCode;", "return laterCode != 0 && offeredCode != 0;",
			"Later does not hide a newer release"},
		{"Later hides nothing", "UpdateSchedule.h",
			"return laterCode != 0 && offeredCode == laterCode;", "return laterCode != 0 && offeredCode == laterCode + 1;",
			"Later hides the release it was chosen on"},
		{"no Later hides the offer", "UpdateSchedule.h",
			"return laterCode != 0 && offeredCode == laterCode;", "return offeredCode == laterCode || offeredCode != 0;",
			"a launch starts with no release hidden by Later"},
		{"download URL by prefix", "ReleaseSelection.h",
			"return url == FeedAssetUrl(feed, tag, asset);",
			"return url.substr(0, FeedAssetUrl(feed, tag, asset).size()) == FeedAssetUrl(feed, tag, asset);",
			"refused download URL"},
		{"any https redirect", "ReleaseSelection.h",
			"    if (EqualsAsciiCaseless(authority, host)) return true;\n  }\n  return false;",
			"    if (EqualsAsciiCaseless(authority, host)) return true;\n  }\n  return true;",
			"refused redirect"},
		{"redirect host by suffix", "ReleaseSelection.h",
			"if (EqualsAsciiCaseless(authority, host)) return true;",
			"if (authority.size() >= host.size() && EqualsAsciiCaseless(authority.substr(authority.size() - host.size()), host)) return true;",
			"refused redirect: https://xrelease-assets"},
		{"redirect with user info", "ReleaseSelection.h",
			"if (authority.find('@') != std::string_view::npos) return false;",
			"if (const std::size_t at = authority.rfind('@'); at != std::string_view::npos) authority.remove_prefix(at + 1);",
			"refused redirect: https://user@"},
		{"redirect on another port", "ReleaseSelection.h",
			`if (authority.substr(colon + 1) != "443") return false;`, "",
			"refused redirect: https://release-assets.githubusercontent.com:8443"},
		{"a user-writable install folder", "InstallLocation.h",
			"return (rights.folder & kWriteRights) == 0 && (rights.executable & kWriteRights) == 0;",
			"return (rights.executable & kWriteRights) == 0;",
			"a folder Users can modify is refused"},
		{"a user-writable helper", "InstallLocation.h",
			"return (rights.folder & kWriteRights) == 0 && (rights.executable & kWriteRights) == 0;",
			"return (rights.folder & kWriteRights) == 0;",
			"an executable Users can modify is refused"},
		{"ownership rights ignored", "InstallLocation.h",
			"kWriteAttributes | kDelete | kWriteDac | kWriteOwner;", "kWriteAttributes | kDelete;",
			"right 0x40000 alone is refused"},
		{"reparse points ignored", "InstallLocation.h",
			"if (!path.underProgramFiles || !path.resolvesToItself || path.reparsePoint) return false;",
			"if (!path.underProgramFiles || !path.resolvesToItself) return false;",
			"a reparse point on the way is refused"},
		{"a user folder", "InstallLocation.h",
			"if (!path.underProgramFiles || !path.resolvesToItself || path.reparsePoint) return false;",
			"if (!path.resolvesToItself || path.reparsePoint) return false;",
			"a user folder is refused"},
		{"msi major wraps at the century", "ReleaseSelection.h",
			"const std::int64_t major = date.year - 2000;", "const std::int64_t major = date.year % 100;",
			"UrMsiVersion(73404575990)"},
		{"msi third field rounds", "ReleaseSelection.h",
			"secondOfDay * 2048 / 86400;", "(secondOfDay * 2048 + 43200) / 86400;",
			"against the oracle"},
		{"msi day field from one", "ReleaseSelection.h",
			"static_cast<std::int64_t>(date.day - 1) * 2048", "static_cast<std::int64_t>(date.day) * 2048",
			"UrMsiVersion(1060587890)"},
		{"3010 as a failure", "UpdateResult.h",
			"if (exitCode == kRebootRequired || exitCode == kRebootInitiated) return Outcome::RestartRequired;", "",
			"3010 is restart to finish, not a failure"},
		{"the UpgradeCode not checked", "UpdateApply.h",
			"return EqualsAsciiCaseless(upgradeCode, kUpgradeCode) && !want.empty() &&",
			"return (upgradeCode.empty() || true) && !want.empty() &&",
			"another product's UpgradeCode is refused"},
		{"the ProductVersion not checked", "UpdateApply.h",
			"         productVersion == want;", "         (productVersion.empty() || true);",
			"an older package re-uploaded under a newer tag is refused"},
		{"another tag's offer taken", "UpdateApply.h",
			"return selection.code != 0 && selection.tag == tag && selection.code > ownCode &&",
			"return selection.code != 0 && (tag.empty() || true) && selection.code > ownCode &&",
			"a list that offers another tag is no offer of this one"},
		{"this build's own release taken", "UpdateApply.h",
			"selection.tag == tag && selection.code > ownCode &&", "selection.tag == tag && selection.code >= ownCode &&",
			"a release no newer than this build is no offer"},
		{"msiexec allowed to restart", "UpdateApply.h",
			`command.append(L"\" /passive /norestart /l*v \"");`, `command.append(L"\" /passive /l*v \"");`,
			"msiexec installs passively, never restarts"},
		{"no relaunch asked", "UpdateApply.h",
			`command.append(L"\" UPDATE_RELAUNCH=1");`, `command.append(L"\"");`,
			"asks for the relaunch"},
		{"a failed install keeps its package", "UpdateResult.h",
			"return outcome == Outcome::Installed || outcome == Outcome::RestartRequired;",
			"return outcome != Outcome::Refused;",
			"a failed install deletes it"},
		{"Retry-After ignored", "UpdateSchedule.h",
			"if (limit.retryAfterSeconds > 0) wait = limit.retryAfterSeconds;",
			"if (limit.retryAfterSeconds > 0) wait = 0;",
			"Retry-After holds the next request"},
		{"a reset with requests left holds", "UpdateSchedule.h",
			"if (limit.exhausted && limit.resetUnixSeconds > 0 && limit.serverUnixSeconds > 0) {",
			"if (limit.resetUnixSeconds > 0 && limit.serverUnixSeconds > 0) {",
			"a reset time with requests left holds nothing"},
		{"a reset measured without the server's date", "UpdateSchedule.h",
			"if (limit.exhausted && limit.resetUnixSeconds > 0 && limit.serverUnixSeconds > 0) {",
			"if (limit.exhausted && limit.resetUnixSeconds > 0) {",
			"a reset without the server's date"},
		{"no cap on a hold", "UpdateSchedule.h",
			"  wait = std::min(wait, kMaxBackoffSeconds);\n", "",
			"holds checks for more than a day"},
		{"a hold that brings the cadence forward", "UpdateSchedule.h",
			"return std::max(cadenceSeconds, wait);", "return wait > 0 ? wait : cadenceSeconds;",
			"a short Retry-After does not bring the cadence forward"},
		{"stale after a day", "UpdateSchedule.h",
			"inline constexpr std::int64_t kStaleAfterSeconds = 72 * 60 * 60;",
			"inline constexpr std::int64_t kStaleAfterSeconds = 24 * 60 * 60;",
			"72 hours exactly is not stale yet"},
		{"stale at 72 hours exactly", "UpdateSchedule.h",
			"nowUnixSeconds - lastSuccessUnixSeconds > kStaleAfterSeconds;",
			"nowUnixSeconds - lastSuccessUnixSeconds >= kStaleAfterSeconds;",
			"72 hours exactly is not stale yet"},
		{"stale with automatic checks off", "UpdateSchedule.h",
			"return checking && lastSuccessUnixSeconds > 0 &&",
			"return (checking || true) && lastSuccessUnixSeconds > 0 &&",
			"with automatic checks off nothing is said"},
		{"stale without a baseline", "UpdateSchedule.h",
			"return checking && lastSuccessUnixSeconds > 0 &&",
			"return checking &&",
			"without a baseline nothing is claimed"},
		// The update channel (UpdateChannel.h), each control putting back one
		// rejected design: a feed the caller names, a marker a non-admin could
		// make or hold, a change read from the exit code, a screen that says what
		// was asked or official releases' answer on the developer feed.
		{"a feed that takes betas reading any tag", "ReleaseSelection.h",
			"    if (feed.acceptBetaPrereleases && !selection_detail::EndsWith(rel.tag, \"-beta\")) continue;\n", "",
			"reads only tags that end -beta"},
		{"a channel argument it does not know taken for a row", "UpdateChannel.h",
			"  return FeedById(text);\n}", "  return FeedById(text) ? FeedById(text) : FeedById(\"official\");\n}",
			"an argument that names a repository is refused"},
		{"a channel named in any case", "ReleaseSelection.h",
			"    if (feed.id == id) return &feed;", "    if (EqualsAsciiCaseless(feed.id, id)) return &feed;",
			"an argument that is not a row's id is refused: 'OFFICIAL'"},
		{"a marker with a fourth ACE trusted", "UpdateChannel.h",
			"d.aces.size() == 3 &&", "d.aces.size() >= 3 &&",
			"a marker with any ACE but its three is not trusted: a fourth ACE that names one user"},
		{"a marker Users can read trusted", "UpdateChannel.h",
			"         IsAce(d.aces[2], flags, usersMask, kUsersSid);",
			"         (IsAce(d.aces[2], flags, usersMask, kUsersSid) || d.aces[2].sid == kUsersSid);",
			"a marker with any ACE but its three is not trusted: Users reading data"},
		{"a marker owned by anyone trusted", "UpdateChannel.h",
			"return d.read && d.owner == kAdministratorsSid && d.daclPresent && d.daclProtected &&",
			"return d.read && d.daclPresent && d.daclProtected &&",
			"a marker with any ACE but its three is not trusted: owned by a user"},
		{"a marker under an unprotected DACL trusted", "UpdateChannel.h",
			"d.daclPresent && d.daclProtected &&", "d.daclPresent &&",
			"a marker with any ACE but its three is not trusted: an unprotected DACL"},
		{"a marker under a reparse point trusted", "UpdateChannel.h",
			"  return path.found == PathFacts::Found::Present && (path.attributes & kAttributeDirectory) &&\n         !(path.attributes & kAttributeReparsePoint);",
			"  return path.found == PathFacts::Found::Present && (path.attributes & kAttributeDirectory);",
			"a marker in a folder that is not the helper's is not trusted: a reparse point"},
		{"a marker in a folder open to others trusted", "UpdateChannel.h",
			"  if (!HasFolderDescriptor(folderDescriptor)) {", "  if (!HasFolderDescriptor(folderDescriptor) && false) {",
			"a marker in a folder that is not the helper's is not trusted: owned by a user"},
		{"a marker with bytes trusted", "UpdateChannel.h",
			"!(path.attributes & (kAttributeDirectory | kAttributeReparsePoint)) && path.size == 0;",
			"!(path.attributes & (kAttributeDirectory | kAttributeReparsePoint));",
			"a marker that is not an empty plain file is not trusted"},
		{"a rejected marker taken for the developer channel's", "UpdateChannel.h",
			"return marker == ChannelMarker::Trusted ? ChannelPlan::Nothing : ChannelPlan::AskThenCreate;",
			"return marker != ChannelMarker::Absent ? ChannelPlan::Nothing : ChannelPlan::AskThenCreate;",
			"a marker that is not trusted is created again"},
		{"official keeping a rejected marker", "UpdateChannel.h",
			"return marker == ChannelMarker::Absent ? ChannelPlan::Nothing : ChannelPlan::Delete;",
			"return marker != ChannelMarker::Trusted ? ChannelPlan::Nothing : ChannelPlan::Delete;",
			"official deletes a marker that is not trusted"},
		{"exit 0 taken for a change", "UpdateChannel.h",
			"  // a request this app refused at once started nothing\n",
			"  if (change.startError == 0 && change.exitCode == 0) return ChangeLine::None;\n  // a request this app refused at once started nothing\n",
			"the marker decides whether the change happened"},
		{"S16 replaced by this build is current", "UpdateChannel.h",
			"    lines.check = CheckLine::NoneOfficial;", "    lines.check = CheckLine::AsToday;",
			"is told no official release is offered to it yet (S16)"},
		{"the developer feed told this build is current", "UpdateChannel.h",
			"    lines.check = view.releaseCounts ? CheckLine::NoneDeveloper : CheckLine::NoneListed;",
			"    lines.check = CheckLine::AsToday;",
			"never today's 'this build is current'"},
		{"About saying up to date for a developer build", "UpdateChannel.h",
			"  lines.aboutUpToDate = !onDeveloper && !view.developerBuild;", "  lines.aboutUpToDate = !onDeveloper;",
			"About never says up to date on the developer feed or for a developer build"},
		{"a button on a copy that cannot change the channel", "UpdateChannel.h",
			"  const bool canChange = readsMarker && view.developerRow;", "  const bool canChange = view.developerRow;",
			"no button on a copy that cannot change the channel"},
		{"the switch enabled while a change waits", "UpdateChannel.h",
			"  lines.switchEnabled = canChange && !asking;", "  lines.switchEnabled = canChange;",
			"while a change waits for approval the switch and the buttons are disabled"},
		{"a developer offer handed over as an installer", "UpdateChannel.h",
			"inline bool HandsOverInstaller(const Feed& feed) { return feed.id == kOfficialFeed.id; }",
			"inline bool HandsOverInstaller(const Feed& feed) { return !feed.id.empty(); }",
			"only an official release is handed over as an installer"},
		{"any text that ends -beta a developer build", "UpdateChannel.h",
			"  return version::ParseReleaseCode(version) != 0 && selection_detail::EndsWith(version, \"-beta\");",
			"  return selection_detail::EndsWith(version, \"-beta\");",
			"no developer build: x-beta"},
		{"a declined change not a refusal", "UpdateResult.h",
			"Declined = 0x2000000E,", "Declined = 0x0000000E,",
			"helper refusal 14 is a refusal"},
		{"GitHub's limit not a refusal", "UpdateResult.h",
			"RateLimited = 0x2000000D,", "RateLimited = 0x0000000D,",
			"helper refusal 13 is a refusal"},
		{"a failure shown in a newer build", "UpdateResult.h",
			"      return ownCode < report.code ? ReportView::NotInstalled : ReportView::Hidden;",
			"      return ReportView::NotInstalled;",
			"is moot in a newer build"},
		{"a restart of Windows asked for after one", "UpdateResult.h",
			"      if (!restartedSince) return ReportView::RestartWindows;",
			"      if (!restartedSince || true) return ReportView::RestartWindows;",
			"3010 after a restart reads as installed"},
		{"an install shown in any build", "UpdateResult.h",
			"      if (ownCode == report.code) return ReportView::Installed;\n      return live",
			"      return ReportView::Installed;\n      return live",
			"an older build launched after an install is told nothing"},
		{"the old app told its update is done", "UpdateResult.h",
			"return live && ownCode < report.code ? ReportView::RestartApp : ReportView::Hidden;",
			"return live && false ? ReportView::RestartApp : ReportView::Hidden;",
			"an install the old app outlived asks for URnetwork's restart"},
		{"the report's time read a day off", "UpdateResult.h",
			"(153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;",
			"(153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day;",
			"reads back"},
		{"an earlier report taken for this run's", "UpdateResult.h",
			"return report.tag == tag && report.exitCode == exitCode && finished &&",
			"return report.tag == tag && (report.exitCode == exitCode || true) && finished &&",
			"an earlier report is not the run that refused before writing one"},
		{"a report from before the helper taken", "UpdateResult.h",
			"*finished >= startedUnixSeconds - kReportClockSlackSeconds;",
			"(*finished >= startedUnixSeconds - kReportClockSlackSeconds || true);",
			"a report written before the helper started is not this run's"},
		{"a lowercase override kept", "UpdateApply.h",
			"    if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');\n", "",
			"an app override: urnetwork_app_root"},
		{"overrides folded the wrong way", "UpdateApply.h",
			"if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');",
			"if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');",
			"an app override: URNETWORK_APP_ROOT"},
		{"any URNETWORK name an override", "UpdateApply.h",
			"constexpr std::wstring_view kPrefix = L\"URNETWORK_\";",
			"constexpr std::wstring_view kPrefix = L\"URNETWORK\";",
			"not an app override: URNETWORKX_ROOT"},
		{"the tray's wait deaf to the app's exit", "UpdateApply.h",
			"    if (exiting()) return HelperWait::AppExiting;\n", "    if (false && exiting()) return HelperWait::AppExiting;\n",
			"the wait ends as the app begins to exit"},
		{"the app's exit winning over the helper's end", "UpdateApply.h",
			"    if (ended()) return HelperWait::Ended;\n    if (exiting()) return HelperWait::AppExiting;\n",
			"    if (exiting()) return HelperWait::AppExiting;\n    if (ended()) return HelperWait::Ended;\n",
			"an ended helper wins"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			requireUpdateReleaseFailure(t, tc.header, updateReleaseReplace(tc.old, tc.replacement), tc.want)
		})
	}
}

// The tray app's check asks for the feed's repository by its id, with
// redirects refused, reads the list with the reader the update helper uses,
// and judges releases against the response's Date header
// (update_offer_wiring_test.go pins that nothing else dates them).
func TestUpdateReleaseTheCheckPollsTheFeedById(t *testing.T) {
	checker := stripComments(readAppSource(t, "UpdateChecker.cpp"))
	check := definitionBody(t, "UpdateChecker.cpp", checker, "bool UpdateChecker::RunCheck(std::uint64_t generation) {")
	signOutRequireInOrder(t, "UpdateChecker::RunCheck", check,
		regexp.QuoteMeta("const update::Feed& feed = *followed;"),
		regexp.QuoteMeta("const std::wstring url = Widen(update::ReleaseListUrl(feed));"),
		`kMaxJsonBytes,\s*false,`,
		regexp.QuoteMeta("update::ParseReleaseList(body);"),
		regexp.QuoteMeta("std::int64_t serverUnixSeconds = headers.serverUnixSeconds;"),
		regexp.QuoteMeta("update::SelectRelease(*parsed, kArch, feed, serverUnixSeconds);"),
		regexp.QuoteMeta("!update::IsFeedAssetUrl(feed, sel.tag, sel.assetName, sel.assetUrl)"))
	fetch := definitionBody(t, "UpdateChecker.cpp", checker, "bool FetchUrl(")
	signOutRequireInOrder(t, "FetchUrl", fetch,
		regexp.QuoteMeta("if (!followRedirects) {"),
		regexp.QuoteMeta("DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;"),
		regexp.QuoteMeta("WINHTTP_OPTION_REDIRECT_POLICY, &policy,"),
		regexp.QuoteMeta("headers.serverUnixSeconds = ResponseDateUnixSeconds(request.h);"),
		regexp.QuoteMeta("if (status != 200) {"))
	for name, source := range appSourceFiles(t, ".cpp", ".h") {
		if strings.Contains(stripComments(source), "api.github.com/repos/") {
			t.Errorf("%s asks GitHub for a repository by its name, which a rename or a re-registered owner moves", name)
		}
	}
	common := readCommonSource(t, "Common.vcxproj")
	for _, header := range []string{"InstallLocation.h", "ReleaseJson.h", "ReleaseSelection.h",
		"UpdateChannel.h", "UpdateResult.h", "UpdateResultJson.h"} {
		if !strings.Contains(common, `<ClInclude Include="`+header+`" />`) {
			t.Errorf("Common.vcxproj does not list %s", header)
		}
	}
}
