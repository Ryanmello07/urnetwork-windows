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
	"strconv"
	"strings"
	"testing"
	"time"
)

// app/tools/UrVersion.ps1 derives every number a Windows build stamps: the
// VERSIONINFO FILEVERSION, the MSI ProductVersion, and the msbuild and WiX
// arguments that carry them. It runs under Windows PowerShell 5.1 in the org's
// release VM and under pwsh in CI. This file is its oracle: the same contract
// written again in Go with integer arithmetic, pinned by fixed vectors that
// were computed independently (Python's datetime), swept across every day
// boundary the MSI layout can reach, and then compared with the script itself,
// vector by vector, under every PowerShell this host has.
//
// The suite stays host-portable, since windows/test.sh runs it on the org's
// macOS builder: without a PowerShell it skips the script comparison loudly,
// except under GitHub Actions, where it fails instead. UR_VERSION_SHELLS (for
// example "powershell.exe,pwsh") names shells that must all be present and
// checked; CI's windows-2022 job sets it.

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
	version.WixArgs = []string{"-p:UrMsiVersion=" + version.Msi}
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

type urFixedVector struct {
	input                      string
	major, minor, patch, build int
	msi                        string
}

// Computed with Python's datetime, not with the code above.
var urFixedVectors = []urFixedVector{
	// The two anchors the design review decoded: v2026.10.1-1060587890 is
	// 2026-10-01T12:46:29Z, v2026.10.3-1062717970-beta 2026-10-03T23:56:37Z.
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
		strings.Join(got.WixArgs, " ") != "-p:UrMsiVersion=26.10.6139" {
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

// One PowerShell process runs every vector through UrVersion.ps1 and prints
// one JSON line per vector, in order. Each vector arrives as the hex of its
// UTF-8 bytes, so it can carry a newline or a non-ASCII digit and the file
// stays ASCII, with no JSON parser in between (pwsh 7's ConvertFrom-Json
// turns date-like strings into DateTime).
const urVersionDriver = `param(
  [Parameter(Mandatory = $true)][string]$Script,
  [Parameter(Mandatory = $true)][string]$Vectors
)
$ErrorActionPreference = 'Stop'
$index = 0
foreach ($hex in [IO.File]::ReadAllLines($Vectors)) {
  $vector = ''
  if ($hex.Length -gt 0) {
    $bytes = for ($i = 0; $i -lt $hex.Length; $i += 2) { [Convert]::ToByte($hex.Substring($i, 2), 16) }
    $vector = [Text.Encoding]::UTF8.GetString([byte[]]@($bytes))
  }
  $row = [ordered]@{ index = $index; ok = $false; count = 0; error = '';
    UrVersion = ''; UrVersionCode = 0; UrVersionMajor = 0; UrVersionMinor = 0;
    UrVersionPatch = 0; UrVersionBuild = 0; UrMsiVersion = '';
    MsbuildArgs = @(); WixArgs = @() }
  try {
    $result = @(& $Script -Version $vector)
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
// it resolves to on Windows), so CI can assert each leg ran by name.
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

// The vectors the script can be checked on against the real clock: the fixed
// ones that are not in the future, every refusal but the 2256 range one, the
// 24 h future bound with an hour of slack each way, and the last and first
// second of every month since the founding (with a code ending in 5 for the
// last second, which must floor, and -beta on the first).
func urScriptVectors(now time.Time) []string {
	var vectors []string
	for _, vector := range urFixedVectors {
		if _, err := deriveUrVersion(vector.input, now); err == nil {
			vectors = append(vectors, vector.input)
		}
	}
	for _, vector := range urRefusedVectors {
		vectors = append(vectors, vector.input)
	}
	vectors = append(vectors,
		urVersionAt(now.Add(23*time.Hour), 0, ""),
		urVersionAt(now.Add(25*time.Hour), 0, ""),
	)
	horizon := now.Add(-48 * time.Hour)
	for month := time.Date(2023, 6, 1, 0, 0, 0, 0, time.UTC); month.Before(horizon); month = month.AddDate(0, 1, 0) {
		vectors = append(vectors,
			urVersionAt(month.Add(-time.Second), 0, ""),
			urVersionAt(month.Add(-time.Second), 5, ""),
			urVersionAt(month, 0, "-beta"),
			urVersionAt(month.Add(-12*time.Hour), 9, ""),
		)
	}
	return vectors
}

func TestUrVersionScriptMatchesOracle(t *testing.T) {
	shells := urVersionShells(t)
	if len(shells) == 0 {
		if os.Getenv("GITHUB_ACTIONS") == "true" {
			t.Fatal("no pwsh or powershell.exe on PATH: app/tools/UrVersion.ps1 cannot be checked, and CI must check it")
		}
		t.Skipf("SKIPPING, NOT PASSING: no pwsh or powershell.exe on PATH, so app/tools/UrVersion.ps1 " +
			"was NOT checked against the oracle on this host (CI checks it under both shells)")
	}
	root := repositoryRoot(t)
	script := filepath.Join(root, "app", "tools", "UrVersion.ps1")
	dir := t.TempDir()
	driver := filepath.Join(dir, "ur-version-driver.ps1")
	if err := os.WriteFile(driver, []byte(urVersionDriver), 0600); err != nil {
		t.Fatal(err)
	}
	vectors := urScriptVectors(time.Now())
	encoded := make([]string, len(vectors))
	for index, vector := range vectors {
		encoded[index] = hex.EncodeToString([]byte(vector))
	}
	vectorFile := filepath.Join(dir, "vectors.txt")
	if err := os.WriteFile(vectorFile, []byte(strings.Join(encoded, "\n")+"\n"), 0600); err != nil {
		t.Fatal(err)
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

			accepted, refused := 0, map[string]int{}
			for index, row := range rows {
				input := vectors[index]
				if row.Index != index {
					t.Fatalf("line %d answers vector %d", index, row.Index)
				}
				want, err := deriveUrVersion(input, now)
				if err != nil {
					class := urRefusalClass(err)
					if row.OK || !strings.Contains(row.Error, class) {
						t.Errorf("%q: the oracle refuses it (%s); the script said ok=%v %q", input, class, row.OK, row.Error)
					}
					refused[class]++
					continue
				}
				if !row.OK {
					t.Errorf("%q: refused by the script (%s), accepted by the oracle", input, row.Error)
					continue
				}
				have := urVersion{
					Version: row.UrVersion, Code: row.UrVersionCode, Major: row.UrVersionMajor,
					Minor: row.UrVersionMinor, Patch: row.UrVersionPatch, Build: row.UrVersionBuild,
					Msi: row.UrMsiVersion, MsbuildArgs: row.MsbuildArgs, WixArgs: row.WixArgs,
				}
				have.MsiFields, want.MsiFields = [3]int{}, [3]int{}
				if row.Count != 1 || !reflect.DeepEqual(have, want) {
					t.Errorf("%q:\n script %d object(s) %+v\n oracle %+v", input, row.Count, have, want)
				}
				accepted++
			}
			// Every refusal the lists define happened, for its own reason, plus the
			// one 25-hour future code; everything else was accepted. A vector list
			// that lost a class would otherwise pass without testing it.
			wantRefused := map[string]int{urRefuseFuture: 1}
			for _, vector := range urRefusedVectors {
				wantRefused[vector.class]++
			}
			t.Logf("%s (%s): %d vectors, %d accepted, refused %v", shell.name, shell.path, len(rows), accepted, refused)
			if !reflect.DeepEqual(refused, wantRefused) || accepted < 150 {
				t.Errorf("vector coverage changed: %d accepted, refused %v, want refused %v", accepted, refused, wantRefused)
			}
		})
	}
}
