// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"bytes"
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"testing"
	"time"
)

// app/tools/UrVersion.ps1 derives every number a Windows build stamps: the
// VERSIONINFO FILEVERSION, the MSI ProductVersion, and the msbuild and WiX
// arguments that carry them. It runs under Windows PowerShell 5.1 in the org's
// release VM. This file is its oracle: the same contract written again in Go
// with integer arithmetic, pinned by fixed vectors that were computed
// independently (Python's datetime) and swept, in the oracle, across every day
// boundary the MSI layout can reach. The script itself is then compared with
// the oracle vector by vector, under every PowerShell this host has, on the
// object build.ps1 reads. Most vectors are judged against this host's clock,
// as a build runs the script; the dates that have not happened yet, up to the
// end of the layout in 2255, are judged against a clock the test pins with
// -NowUtc.
//
// The suite stays host-portable, since windows/test.sh runs it on the org's
// macOS builder: without a PowerShell it skips the script comparison loudly.
// UR_VERSION_SHELLS (for example "powershell.exe,pwsh") names shells that must
// all be present and checked.

var urFounded = time.Date(2023, 5, 23, 0, 0, 0, 0, time.UTC)

// Refusal classes. Each is a phrase UrVersion.ps1 puts in its error message,
// so a refused vector proves the script refused it for the expected reason.
const (
	urRefuseGrammar = "not a release version"
	urRefuseDevCode = "code 0"
	urRefuseFuture  = "future code"
	urRefuseRange   = "outside the MSI ProductVersion range"
)

type urRefusal struct{ class, input string }

func (r *urRefusal) Error() string { return fmt.Sprintf("%s: %q", r.class, r.input) }

func urRefusalClass(err error) string {
	var refusal *urRefusal
	if errors.As(err, &refusal) {
		return refusal.class
	}
	return ""
}

type urVersion struct {
	Version                    string
	Code                       int64
	Major, Minor, Patch, Build int
	Msi                        string
	MsiFields                  [3]int
	MsbuildArgs, WixArgs       []string
}

var urVersionGrammar = regexp.MustCompile(`^([0-9]{4})\.([0-9]{1,2})\.([0-9]{1,2})-([0-9]{1,18})(-beta)?$`)

// deriveUrVersion is UrVersion.ps1's contract, written independently.
func deriveUrVersion(input string, now time.Time) (urVersion, error) {
	match := urVersionGrammar.FindStringSubmatch(input)
	if match == nil {
		return urVersion{}, &urRefusal{urRefuseGrammar, input}
	}
	month, _ := strconv.Atoi(match[2])
	day, _ := strconv.Atoi(match[3])
	if match[2][0] == '0' || match[3][0] == '0' || month > 12 || day > 31 {
		return urVersion{}, &urRefusal{urRefuseGrammar, input}
	}
	if match[4] == "0" {
		return urVersion{}, &urRefusal{urRefuseDevCode, input}
	}
	if match[4][0] == '0' {
		return urVersion{}, &urRefusal{urRefuseGrammar, input}
	}
	code, err := strconv.ParseInt(match[4], 10, 64)
	if err != nil {
		return urVersion{}, &urRefusal{urRefuseGrammar, input}
	}
	version, err := deriveUrNumbers(code, now)
	if err != nil {
		var refusal *urRefusal
		if errors.As(err, &refusal) {
			refusal.input = input
		}
		return urVersion{}, err
	}
	version.Version = input
	version.MsbuildArgs = []string{
		"/p:UrVersion=" + input,
		fmt.Sprintf("/p:UrVersionCode=%d", code),
		fmt.Sprintf("/p:UrVersionMajor=%d", version.Major),
		fmt.Sprintf("/p:UrVersionMinor=%d", version.Minor),
		fmt.Sprintf("/p:UrVersionPatch=%d", version.Patch),
		fmt.Sprintf("/p:UrVersionBuild=%d", version.Build),
	}
	version.WixArgs = []string{
		"-p:UrMsiVersion=" + version.Msi,
		fmt.Sprintf("-p:UrFileVersion=%d.%d.%d.%d", version.Major, version.Minor, version.Patch, version.Build),
	}
	return version, nil
}

// deriveUrNumbers holds every numeric rule; the sweeps call it directly.
func deriveUrNumbers(code int64, now time.Time) (urVersion, error) {
	// Whole seconds: a code ending in 5 names the middle of a second, and it
	// must floor, never round.
	seconds := code / 10
	if seconds > now.Unix()-urFounded.Unix()+24*60*60 {
		return urVersion{}, &urRefusal{class: urRefuseFuture}
	}
	at := time.Unix(urFounded.Unix()+seconds, 0).UTC()
	secondOfDay := at.Hour()*3600 + at.Minute()*60 + at.Second()
	msi := [3]int{at.Year() - 2000, int(at.Month()), (at.Day()-1)*2048 + secondOfDay*2048/86400}
	if msi[0] < 0 || msi[0] > 255 || msi[2] > 65535 {
		return urVersion{}, &urRefusal{class: urRefuseRange}
	}
	return urVersion{
		Code:      code,
		Major:     at.Year(),
		Minor:     int(at.Month()),
		Patch:     at.Day(),
		Build:     secondOfDay / 2,
		Msi:       fmt.Sprintf("%d.%d.%d", msi[0], msi[1], msi[2]),
		MsiFields: msi,
	}, nil
}

// The release version for an instant, in the string form warpctl mints.
func urVersionAt(at time.Time, tenths int64, suffix string) string {
	at = at.UTC()
	code := (at.Unix()-urFounded.Unix())*10 + tenths
	return fmt.Sprintf("%d.%d.%d-%d%s", at.Year(), int(at.Month()), at.Day(), code, suffix)
}

// The release version for a code: the UTC date of its instant, then the code.
func urVersionOfCode(code int64, suffix string) string {
	at := time.Unix(urFounded.Unix()+code/10, 0).UTC()
	return fmt.Sprintf("%d.%d.%d-%d%s", at.Year(), int(at.Month()), at.Day(), code, suffix)
}

type urFixedVector struct {
	input                      string
	major, minor, patch, build int
	msi                        string
}

// Computed with Python's datetime, not with the code above.
var urFixedVectors = []urFixedVector{
	// A published release, v2026.10.1-1060587890, minted at
	// 2026-10-01T12:46:29Z, and a -beta version minted at 2026-10-03T23:56:37Z.
	{"2026.10.1-1060587890", 2026, 10, 1, 22994, "26.10.1090"},
	{"2026.10.3-1062717970-beta", 2026, 10, 3, 43098, "26.10.6139"},
	// Codes ending in 5 and 9 floor to the same second as ...890. A
	// [long](code / 10) in PowerShell rounds them up by a second...
	{"2026.10.1-1060587895", 2026, 10, 1, 22994, "26.10.1090"},
	{"2026.10.1-1060587899", 2026, 10, 1, 22994, "26.10.1090"},
	// ...which here would cross into the next month (26.10.0) and the next
	// year (26.1.0).
	{"2026.9.30-1060127995", 2026, 9, 30, 43199, "26.9.61439"},
	{"2026.9.30-1060127999", 2026, 9, 30, 43199, "26.9.61439"},
	{"2025.12.31-824255995", 2025, 12, 31, 43199, "25.12.63487"},
	// The string's date is never used as a number. The org host stamps its
	// LOCAL date, which lags UTC: this 2026.9.30 release was minted at
	// 2026-10-01T06:31:42Z.
	{"2026.9.30-1060363020", 2026, 10, 1, 11751, "26.10.557"},
	// Day, month, year, leap-day and 42-second-step edges.
	{"2026.10.1-1060128000", 2026, 10, 1, 0, "26.10.0"},
	{"2026.10.1-1060128420", 2026, 10, 1, 21, "26.10.0"},
	{"2026.10.1-1060128430", 2026, 10, 1, 21, "26.10.1"},
	{"2026.8.31-1034207990", 2026, 8, 31, 43199, "26.8.63487"},
	{"2026.9.1-1034208000", 2026, 9, 1, 0, "26.9.0"},
	{"2025.12.31-824255990", 2025, 12, 31, 43199, "25.12.63487"},
	{"2026.1.1-824256000", 2026, 1, 1, 0, "26.1.0"},
	{"2024.2.29-244080000", 2024, 2, 29, 21600, "24.2.58368"},
	{"2024.2.29-244511990", 2024, 2, 29, 43199, "24.2.59391"},
	{"2024.3.1-244512000", 2024, 3, 1, 0, "24.3.0"},
	{"2026.10.31-1086911990", 2026, 10, 31, 43199, "26.10.63487"},
	{"2026.11.1-1086912000", 2026, 11, 1, 0, "26.11.0"},
	// The first codes there are, and the last ProductVersion the layout can
	// express (the year 2255).
	{"2023.5.23-1", 2023, 5, 23, 0, "23.5.45056"},
	{"2023.5.23-9", 2023, 5, 23, 0, "23.5.45056"},
	{"2255.12.31-73404575990", 2255, 12, 31, 43199, "255.12.63487"},
}

// Refused whatever the clock says.
var urRefusedVectors = []struct{ input, class string }{
	{"", urRefuseGrammar},
	{"v2026.10.1-1060587890", urRefuseGrammar},     // the tag, not the version
	{"2026.10.01-1060587890", urRefuseGrammar},     // zero-padded day
	{"2026.01.1-1060587890", urRefuseGrammar},      // zero-padded month
	{"2026.0.1-1060587890", urRefuseGrammar},       // month 0
	{"2026.13.1-1060587890", urRefuseGrammar},      // month 13
	{"2026.10.0-1060587890", urRefuseGrammar},      // day 0
	{"2026.10.32-1060587890", urRefuseGrammar},     // day 32
	{"26.10.1-1060587890", urRefuseGrammar},        // two-digit year
	{"20260.10.1-1060587890", urRefuseGrammar},     // five-digit year
	{"2026.10.1", urRefuseGrammar},                 // no code
	{"2026.10.1-", urRefuseGrammar},                // empty code
	{"2026.10.1-106058789a", urRefuseGrammar},      // not a number
	{"2026.10.1-01060587890", urRefuseGrammar},     // leading zero in the code
	{"2026.10.1-1060587890-BETA", urRefuseGrammar}, // the trailer is case-sensitive
	{"2026.10.1-1060587890-beta2", urRefuseGrammar},
	{"2026.10.1-1060587890-rc", urRefuseGrammar},
	{"2026.10.1-1060587890 ", urRefuseGrammar},
	{" 2026.10.1-1060587890", urRefuseGrammar},
	{"2026.10.1-1060587890\n", urRefuseGrammar}, // a regex $ would accept this
	{"2026.10.1-1060587890-beta\n", urRefuseGrammar},
	{"2026.10.1-1234567890123456789", urRefuseGrammar},            // 19 digits
	{"\u0662\u0660\u0662\u0666.10.1-1060587890", urRefuseGrammar}, // Arabic-Indic digits
	{"2026.10.1\u22121060587890", urRefuseGrammar},                // U+2212 minus sign
	{"2026.10.1-0", urRefuseDevCode},
	{"2026.10.1-0-beta", urRefuseDevCode},
	{"2026.10.1-999999999999999999", urRefuseFuture}, // 18 digits: grammar ok, year ~3.2e9
}

func TestUrVersionOracleFixedVectors(t *testing.T) {
	// A clock past every vector, so none of them is a future code.
	now := time.Date(2256, 1, 1, 0, 0, 0, 0, time.UTC)
	for _, vector := range urFixedVectors {
		got, err := deriveUrVersion(vector.input, now)
		if err != nil {
			t.Errorf("%s: %v", vector.input, err)
			continue
		}
		want := [5]any{vector.major, vector.minor, vector.patch, vector.build, vector.msi}
		have := [5]any{got.Major, got.Minor, got.Patch, got.Build, got.Msi}
		if want != have {
			t.Errorf("%s: got %v, want %v", vector.input, have, want)
		}
	}
	got, err := deriveUrVersion("2026.10.3-1062717970-beta", now)
	if err != nil {
		t.Fatal(err)
	}
	wantMsbuild := []string{
		"/p:UrVersion=2026.10.3-1062717970-beta",
		"/p:UrVersionCode=1062717970",
		"/p:UrVersionMajor=2026",
		"/p:UrVersionMinor=10",
		"/p:UrVersionPatch=3",
		"/p:UrVersionBuild=43098",
	}
	if strings.Join(got.MsbuildArgs, " ") != strings.Join(wantMsbuild, " ") ||
		strings.Join(got.WixArgs, " ") != "-p:UrMsiVersion=26.10.6139 -p:UrFileVersion=2026.10.3.43098" {
		t.Errorf("arguments: msbuild %q wix %q", got.MsbuildArgs, got.WixArgs)
	}
	// -beta changes the string and nothing else.
	plain, err := deriveUrVersion("2026.10.3-1062717970", now)
	if err != nil {
		t.Fatal(err)
	}
	if plain.Code != got.Code || plain.Msi != got.Msi || plain.Build != got.Build ||
		plain.Version == got.Version {
		t.Errorf("-beta changed a number, or did not change the string: %+v vs %+v", plain, got)
	}
}

func TestUrVersionOracleRefusals(t *testing.T) {
	now := time.Date(2026, 10, 4, 12, 0, 0, 0, time.UTC)
	for _, vector := range urRefusedVectors {
		if _, err := deriveUrVersion(vector.input, now); urRefusalClass(err) != vector.class {
			t.Errorf("%q: got %v, want a %q refusal", vector.input, err, vector.class)
		}
	}

	// The future bound is 24 h of whole seconds past the host's clock.
	nowCode := (now.Unix() - urFounded.Unix()) * 10
	for _, edge := range []struct {
		code   int64
		future bool
	}{
		{nowCode + 864000, false}, // exactly 24 h ahead
		{nowCode + 864009, false}, // the same second
		{nowCode + 864010, true},  // one second more
	} {
		input := fmt.Sprintf("2026.10.5-%d", edge.code)
		_, err := deriveUrVersion(input, now)
		if refused := urRefusalClass(err) == urRefuseFuture; refused != edge.future {
			t.Errorf("%s at %s: future refusal %v, want %v (%v)", input, now.Format(time.RFC3339), refused, edge.future, err)
		}
	}

	// Past the year 2255 the major field no longer fits in a byte.
	later := time.Date(2257, 1, 1, 0, 0, 0, 0, time.UTC)
	if _, err := deriveUrVersion("2256.1.1-73404576000", later); urRefusalClass(err) != urRefuseRange {
		t.Errorf("2256: got %v, want a %q refusal", err, urRefuseRange)
	}
	if _, err := deriveUrVersion("2255.12.31-73404575990", later); err != nil {
		t.Errorf("2255: %v", err)
	}
}

func urFileVersionLess(a, b urVersion) bool {
	x := [4]int{a.Major, a.Minor, a.Patch, a.Build}
	y := [4]int{b.Major, b.Minor, b.Patch, b.Build}
	for i := range x {
		if x[i] != y[i] {
			return x[i] < y[i]
		}
	}
	return false
}

func urMsiLess(a, b urVersion) bool {
	for i := range a.MsiFields {
		if a.MsiFields[i] != b.MsiFields[i] {
			return a.MsiFields[i] < b.MsiFields[i]
		}
	}
	return false
}

// The ordering promise behind both versions: a later code never yields a
// lower FILEVERSION or a lower MSI ProductVersion.
func TestUrVersionOracleSweep(t *testing.T) {
	now := time.Date(2256, 1, 1, 0, 0, 0, 0, time.UTC)
	derive := func(at time.Time) urVersion {
		t.Helper()
		version, err := deriveUrNumbers((at.Unix()-urFounded.Unix())*10, now)
		if err != nil {
			t.Fatalf("%s: %v", at.Format(time.RFC3339), err)
		}
		return version
	}

	// Every day boundary the layout can express: the last second of a day must
	// order strictly below the first second of the next.
	boundaries := 0
	end := time.Date(2255, 12, 31, 0, 0, 0, 0, time.UTC)
	for day := urFounded; day.Before(end); day = day.AddDate(0, 0, 1) {
		next := day.AddDate(0, 0, 1)
		before, after := derive(next.Add(-time.Second)), derive(next)
		if !urFileVersionLess(before, after) || !urMsiLess(before, after) {
			t.Fatalf("%s: FILEVERSION %d.%d.%d.%d -> %d.%d.%d.%d, MSI %s -> %s",
				next.Format(time.RFC3339), before.Major, before.Minor, before.Patch, before.Build,
				after.Major, after.Minor, after.Patch, after.Build, before.Msi, after.Msi)
		}
		boundaries++
	}
	// 2023-05-23 to 2255-12-31 is 84,958 days (Python's date arithmetic); a
	// loop that ran short proves nothing about the days it skipped.
	if boundaries != 84958 {
		t.Fatalf("swept %d day boundaries, want 84958", boundaries)
	}

	// Second by second through a month end, a leap day and a year end, each
	// into the next day: never lower; FILEVERSION strictly higher every two
	// seconds; the MSI version at most 43 seconds on one value.
	for _, start := range []time.Time{
		time.Date(2026, 9, 30, 0, 0, 0, 0, time.UTC),
		time.Date(2028, 2, 29, 0, 0, 0, 0, time.UTC),
		time.Date(2025, 12, 31, 0, 0, 0, 0, time.UTC),
	} {
		previous := derive(start)
		fileRun, msiRun, longestFile, longestMsi := 1, 1, 1, 1
		for second := 1; second <= 86400; second++ {
			current := derive(start.Add(time.Duration(second) * time.Second))
			if urFileVersionLess(current, previous) || urMsiLess(current, previous) {
				t.Fatalf("%s + %d s went down: %+v -> %+v", start.Format(time.DateOnly), second, previous, current)
			}
			if urFileVersionLess(previous, current) {
				fileRun = 1
			} else {
				fileRun++
			}
			if urMsiLess(previous, current) {
				msiRun = 1
			} else {
				msiRun++
			}
			longestFile = max(longestFile, fileRun)
			longestMsi = max(longestMsi, msiRun)
			previous = current
		}
		if longestFile != 2 || longestMsi != 43 {
			t.Errorf("%s: longest run on one FILEVERSION %d s (want 2), on one MSI version %d s (want 43)",
				start.Format(time.DateOnly), longestFile, longestMsi)
		}
	}
}

func TestUrVersionScriptIsASCII(t *testing.T) {
	root := repositoryRoot(t)
	// Windows PowerShell 5.1 reads a BOM-less UTF-8 script as ANSI, where an
	// en or em dash ends in a byte it takes for a double quote. These are the
	// scripts the org's release VM runs under 5.1.
	for _, relative := range []string{
		"app/tools/UrVersion.ps1",
		"app/build.ps1",
		"app/tools/fetch-deps.ps1",
		"build-sdk.ps1",
	} {
		data, err := os.ReadFile(filepath.Join(root, filepath.FromSlash(relative)))
		if err != nil {
			t.Fatal(err)
		}
		if len(data) == 0 {
			t.Fatalf("%s is empty", relative)
		}
		for offset, value := range data {
			if value >= 0x80 {
				line := 1 + bytes.Count(data[:offset], []byte("\n"))
				t.Errorf("%s:%d: byte 0x%02x is not ASCII", relative, line, value)
				break
			}
		}
	}
}

// One PowerShell process runs every vector through UrVersion.ps1 for the
// object build.ps1 reads. It prints one JSON line per vector, in order. Each
// input line holds the hex of the vector's UTF-8 bytes, so a vector can carry
// a newline or a non-ASCII digit and the file stays ASCII, with no JSON parser
// in between (pwsh 7's ConvertFrom-Json turns date-like strings into
// DateTime), then the -NowUtc to judge it against, or "-" for this host's
// clock.
const urVersionDriver = `param(
  [Parameter(Mandatory = $true)][string]$Script,
  [Parameter(Mandatory = $true)][string]$Vectors
)
$ErrorActionPreference = 'Stop'
$index = 0
foreach ($line in [IO.File]::ReadAllLines($Vectors)) {
  $fields = $line.Split(' ')
  $hex = $fields[0]
  $vector = ''
  if ($hex.Length -gt 0) {
    $bytes = for ($i = 0; $i -lt $hex.Length; $i += 2) { [Convert]::ToByte($hex.Substring($i, 2), 16) }
    $vector = [Text.Encoding]::UTF8.GetString([byte[]]@($bytes))
  }
  $clock = @{}
  if ($fields[1] -ne '-') { $clock.NowUtc = $fields[1] }
  $row = [ordered]@{ index = $index; ok = $false; count = 0; error = '';
    UrVersion = ''; UrVersionCode = 0; UrVersionMajor = 0; UrVersionMinor = 0;
    UrVersionPatch = 0; UrVersionBuild = 0; UrMsiVersion = '';
    MsbuildArgs = @(); WixArgs = @() }
  try {
    $result = @(& $Script -Version $vector @clock)
    $row.count = $result.Count
    $first = $result[0]
    $row.UrVersion = [string]$first.UrVersion
    $row.UrVersionCode = [long]$first.UrVersionCode
    $row.UrVersionMajor = [int]$first.UrVersionMajor
    $row.UrVersionMinor = [int]$first.UrVersionMinor
    $row.UrVersionPatch = [int]$first.UrVersionPatch
    $row.UrVersionBuild = [int]$first.UrVersionBuild
    $row.UrMsiVersion = [string]$first.UrMsiVersion
    $row.MsbuildArgs = @($first.MsbuildArgs | ForEach-Object { [string]$_ })
    $row.WixArgs = @($first.WixArgs | ForEach-Object { [string]$_ })
    $row.ok = $true
  } catch {
    $row.error = [string]$_.Exception.Message
  }
  [Console]::Out.WriteLine(($row | ConvertTo-Json -Compress -Depth 3))
  $index++
}
`

type urScriptRow struct {
	Index          int      `json:"index"`
	OK             bool     `json:"ok"`
	Count          int      `json:"count"`
	Error          string   `json:"error"`
	UrVersion      string   `json:"UrVersion"`
	UrVersionCode  int64    `json:"UrVersionCode"`
	UrVersionMajor int      `json:"UrVersionMajor"`
	UrVersionMinor int      `json:"UrVersionMinor"`
	UrVersionPatch int      `json:"UrVersionPatch"`
	UrVersionBuild int      `json:"UrVersionBuild"`
	UrMsiVersion   string   `json:"UrMsiVersion"`
	MsbuildArgs    []string `json:"MsbuildArgs"`
	WixArgs        []string `json:"WixArgs"`
}

type urShell struct{ name, path string }

// The PowerShells to run the script under: every one named in
// UR_VERSION_SHELLS (all required), else whichever of pwsh and powershell.exe
// this host has. Subtests are named as requested ("pwsh", not the "pwsh.exe"
// it resolves to on Windows).
func urVersionShells(t *testing.T) []urShell {
	t.Helper()
	names := []string{"pwsh", "powershell.exe"}
	required := os.Getenv("UR_VERSION_SHELLS")
	if required != "" {
		names = strings.Split(required, ",")
	}
	var shells []urShell
	for _, name := range names {
		name = strings.TrimSpace(name)
		path, err := exec.LookPath(name)
		if err != nil {
			if required != "" {
				t.Fatalf("UR_VERSION_SHELLS requires %q, which is not on PATH: %v", name, err)
			}
			continue
		}
		shells = append(shells, urShell{name, path})
	}
	return shells
}

// A vector for the script, and the instant it is judged against (-NowUtc).
// The zero time means this host's clock, which is how builds run the script.
type urScriptVector struct {
	input string
	clock time.Time
}

// The clock for the dates that have not happened yet: the first second the
// MSI layout cannot express, so every instant up to the end of 2255 is past.
var urPinnedClock = time.Date(2256, 1, 1, 0, 0, 0, 0, time.UTC)

// The vectors the script is checked on, and how many of them each refusal
// class must claim.
//
// Against this host's clock: the fixed vectors that are not in the future,
// every refusal in the list, the 24 h future bound with an hour of slack each
// way, and the last and first second of every month since the founding (with
// a code ending in 5 for the last second, which must floor, and -beta on the
// first).
//
// Against a pinned clock: every fixed vector; the last and first second of
// every February and every year up to 2255; the same for every month of the
// years where a number grows past a size (the code passes 2^31 on 2030-03-12
// and 2^32 on 2036-12-31, its seconds pass 2^31 on 2091-06-10) or the century
// turns (2100 and 2200 are no leap years); the codes either side of those
// crossings; the 24 h future bound to the second, in three eras; and the first
// second of 2256, which has no ProductVersion.
func urScriptVectors(now time.Time) ([]urScriptVector, map[string]int) {
	var vectors []urScriptVector
	wantRefused := map[string]int{}
	add := func(input string, clock time.Time) {
		vectors = append(vectors, urScriptVector{input, clock})
	}

	var hostClock time.Time
	for _, vector := range urFixedVectors {
		if _, err := deriveUrVersion(vector.input, now); err == nil {
			add(vector.input, hostClock)
		}
	}
	for _, vector := range urRefusedVectors {
		add(vector.input, hostClock)
		wantRefused[vector.class]++
	}
	add(urVersionAt(now.Add(23*time.Hour), 0, ""), hostClock)
	add(urVersionAt(now.Add(25*time.Hour), 0, ""), hostClock)
	wantRefused[urRefuseFuture]++
	horizon := now.Add(-48 * time.Hour)
	for month := time.Date(2023, 6, 1, 0, 0, 0, 0, time.UTC); month.Before(horizon); month = month.AddDate(0, 1, 0) {
		add(urVersionAt(month.Add(-time.Second), 0, ""), hostClock)
		add(urVersionAt(month.Add(-time.Second), 5, ""), hostClock)
		add(urVersionAt(month, 0, "-beta"), hostClock)
		add(urVersionAt(month.Add(-12*time.Hour), 9, ""), hostClock)
	}

	for _, vector := range urFixedVectors {
		add(vector.input, urPinnedClock)
	}
	boundaries := map[time.Time]bool{}
	for year := 2023; year <= 2255; year++ {
		boundaries[time.Date(year, time.March, 1, 0, 0, 0, 0, time.UTC)] = true
		boundaries[time.Date(year+1, time.January, 1, 0, 0, 0, 0, time.UTC)] = true
	}
	for _, year := range []int{2030, 2036, 2037, 2091, 2099, 2100, 2199, 2200, 2255} {
		for month := time.January; month <= time.December; month++ {
			boundaries[time.Date(year, month+1, 1, 0, 0, 0, 0, time.UTC)] = true
		}
	}
	ordered := make([]time.Time, 0, len(boundaries))
	for boundary := range boundaries {
		if boundary.After(urFounded) {
			ordered = append(ordered, boundary)
		}
	}
	sort.Slice(ordered, func(i, j int) bool { return ordered[i].Before(ordered[j]) })
	for _, boundary := range ordered {
		add(urVersionAt(boundary.Add(-time.Second), 5, ""), urPinnedClock)
		if boundary.Before(urPinnedClock) {
			add(urVersionAt(boundary, 0, "-beta"), urPinnedClock)
		}
	}
	for _, code := range []int64{
		1<<31 - 1, 1 << 31, 1<<31 + 9,
		1<<32 - 1, 1 << 32, 1<<32 + 9,
		(1<<31-1)*10 + 9, (1 << 31) * 10, (1<<31)*10 + 9,
	} {
		add(urVersionOfCode(code, ""), urPinnedClock)
	}
	for _, clock := range []time.Time{
		time.Date(2030, 3, 12, 0, 0, 0, 0, time.UTC),
		time.Date(2100, 2, 28, 12, 0, 0, 0, time.UTC),
		time.Date(2255, 12, 30, 0, 0, 0, 0, time.UTC),
	} {
		code := (clock.Unix() - urFounded.Unix()) * 10
		add(urVersionOfCode(code+864000, ""), clock) // exactly 24 h ahead
		add(urVersionOfCode(code+864009, ""), clock) // the same second
		add(urVersionOfCode(code+864010, ""), clock) // one second more
		wantRefused[urRefuseFuture]++
	}
	add(urVersionAt(urPinnedClock, 0, ""), urPinnedClock)
	wantRefused[urRefuseRange]++
	return vectors, wantRefused
}

func TestUrVersionScriptMatchesOracle(t *testing.T) {
	shells := urVersionShells(t)
	if len(shells) == 0 {
		t.Skipf("SKIPPING, NOT PASSING: no pwsh or powershell.exe on PATH, so app/tools/UrVersion.ps1 " +
			"was NOT checked against the oracle on this host")
	}
	root := repositoryRoot(t)
	script := filepath.Join(root, "app", "tools", "UrVersion.ps1")
	dir := t.TempDir()
	driver := filepath.Join(dir, "ur-version-driver.ps1")
	if err := os.WriteFile(driver, []byte(urVersionDriver), 0600); err != nil {
		t.Fatal(err)
	}
	vectors, wantRefused := urScriptVectors(time.Now())
	lines := make([]string, len(vectors))
	for index, vector := range vectors {
		clock := "-"
		if !vector.clock.IsZero() {
			clock = vector.clock.Format("2006-01-02T15:04:05Z")
		}
		lines[index] = hex.EncodeToString([]byte(vector.input)) + " " + clock
	}
	vectorFile := filepath.Join(dir, "vectors.txt")
	if err := os.WriteFile(vectorFile, []byte(strings.Join(lines, "\n")+"\n"), 0600); err != nil {
		t.Fatal(err)
	}
	// What the pinned vectors exist for: each of these must be among them, and
	// accepted.
	reach := []string{
		urVersionAt(time.Date(2100, time.March, 1, 0, 0, 0, 0, time.UTC).Add(-time.Second), 5, ""),
		urVersionAt(time.Date(2100, time.March, 1, 0, 0, 0, 0, time.UTC), 0, "-beta"),
		urVersionOfCode(1<<31, ""),
		urVersionOfCode(1<<32, ""),
		urVersionOfCode((1<<31)*10, ""),
		urVersionAt(urPinnedClock.Add(-time.Second), 5, ""),
	}

	for _, shell := range shells {
		t.Run(shell.name, func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Minute)
			defer cancel()
			command := exec.CommandContext(ctx, shell.path, "-NoProfile", "-NonInteractive",
				"-ExecutionPolicy", "Bypass", "-File", driver, "-Script", script, "-Vectors", vectorFile)
			var stdout, stderr bytes.Buffer
			command.Stdout, command.Stderr = &stdout, &stderr
			if err := command.Run(); err != nil {
				t.Fatalf("%s: %v\nstderr:\n%s\nstdout:\n%s", shell.path, err, stderr.String(), stdout.String())
			}
			now := time.Now()
			var rows []urScriptRow
			for _, line := range strings.Split(stdout.String(), "\n") {
				line = strings.TrimSpace(line)
				if line == "" {
					continue
				}
				var row urScriptRow
				if err := json.Unmarshal([]byte(line), &row); err != nil {
					t.Fatalf("unreadable driver line %q: %v\nstderr:\n%s", line, err, stderr.String())
				}
				rows = append(rows, row)
			}
			if len(rows) != len(vectors) {
				t.Fatalf("%d result lines for %d vectors\nstderr:\n%s", len(rows), len(vectors), stderr.String())
			}

			accepted, pinnedAccepted, refused := 0, 0, map[string]int{}
			acceptedPinned := map[string]bool{}
			for index, row := range rows {
				vector := vectors[index]
				if row.Index != index {
					t.Fatalf("line %d answers vector %d", index, row.Index)
				}
				clock, label := now, fmt.Sprintf("%q", vector.input)
				if !vector.clock.IsZero() {
					clock = vector.clock
					label += " at -NowUtc " + vector.clock.Format(time.RFC3339)
				}
				want, err := deriveUrVersion(vector.input, clock)
				if err != nil {
					class := urRefusalClass(err)
					if row.OK || !strings.Contains(row.Error, class) {
						t.Errorf("%s: the oracle refuses it (%s); the script said ok=%v %q", label, class, row.OK, row.Error)
					}
					refused[class]++
					continue
				}
				if !row.OK {
					t.Errorf("%s: refused by the script (%s), accepted by the oracle", label, row.Error)
					continue
				}
				have := urVersion{
					Version: row.UrVersion, Code: row.UrVersionCode, Major: row.UrVersionMajor,
					Minor: row.UrVersionMinor, Patch: row.UrVersionPatch, Build: row.UrVersionBuild,
					Msi: row.UrMsiVersion, MsbuildArgs: row.MsbuildArgs, WixArgs: row.WixArgs,
				}
				have.MsiFields, want.MsiFields = [3]int{}, [3]int{}
				if row.Count != 1 || !reflect.DeepEqual(have, want) {
					t.Errorf("%s:\n script %d object(s) %+v\n oracle %+v", label, row.Count, have, want)
				}
				if vector.clock.IsZero() {
					accepted++
				} else {
					pinnedAccepted++
					acceptedPinned[vector.input] = true
				}
			}
			// Every refusal the vectors were built to draw happened, for its own
			// reason, and everything else was accepted, in number. A vector list
			// that lost a class or a range would otherwise pass without testing it.
			t.Logf("%s (%s): %d vectors; accepted %d against this host's clock and %d against -NowUtc; refused %v",
				shell.name, shell.path, len(rows), accepted, pinnedAccepted, refused)
			if !reflect.DeepEqual(refused, wantRefused) || accepted < 150 || pinnedAccepted < 1100 {
				t.Errorf("vector coverage changed: accepted %d and %d, refused %v, want refused %v",
					accepted, pinnedAccepted, refused, wantRefused)
			}
			for _, input := range reach {
				if !acceptedPinned[input] {
					t.Errorf("the pinned vectors no longer reach %q", input)
				}
			}
		})
	}
}

// -NowUtc moves the clock the future-code check measures against. Only the
// tests may pass it: a build that did could stamp a code from the future,
// which would then outrank every real release. The search's control is the
// script itself, which must be the one file that names the parameter.
func TestUrVersionClockOverrideIsTestOnly(t *testing.T) {
	root := repositoryRoot(t)
	skipped := map[string]bool{
		".git": true, "bin": true, "build": true, "node_modules": true, "obj": true, "out": true, "tests": true,
	}
	var found []string
	err := filepath.WalkDir(root, func(path string, entry os.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if entry.IsDir() {
			if path != root && skipped[entry.Name()] {
				return filepath.SkipDir
			}
			return nil
		}
		switch strings.ToLower(filepath.Ext(path)) {
		case ".ps1", ".psm1", ".cmd", ".bat", ".sh", ".yml", ".yaml", ".props", ".targets", ".vcxproj", ".wixproj":
		default:
			return nil
		}
		data, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		if strings.Contains(strings.ToLower(string(data)), "nowutc") {
			relative, err := filepath.Rel(root, path)
			if err != nil {
				return err
			}
			found = append(found, filepath.ToSlash(relative))
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if want := []string{"app/tools/UrVersion.ps1"}; !reflect.DeepEqual(found, want) {
		t.Errorf("-NowUtc appears in %v; want it only in %v, which declares it", found, want)
	}
}
