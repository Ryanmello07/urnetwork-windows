# The one place a URnetwork Windows build's version numbers are derived.
#
# Input: the release version <YYYY.M.D>-<code>[-beta], which is the release
# tag without its leading v (2026.10.1-1060587890). Output: every value the
# build stamps, plus the exact msbuild and WiX arguments that carry them.
# app/build.ps1 (the org release build) and the CI workflow both call this
# script, and tests/ur_version_test.go checks it against an independent Go
# implementation.
#
# Every number comes from t, the UTC instant the code names: floor(code / 10)
# seconds after 2023-05-23T00:00:00Z. (warpctl mints the code as the seconds
# since the company was founded, times ten.) The YYYY.M.D in the string is
# never used as a number, because the org release host writes its LOCAL date
# there, which lags UTC. Builds can only be ordered by an instant they agree
# on, so a numeric date can be one day ahead of the string's.
#
#   UrVersion       the input, verbatim: the ProductVersion string
#   UrVersionCode   the code, which the update checker ranks releases by
#   UrVersionMajor  Y of t     \
#   UrVersionMinor  M of t      > VERSIONINFO FILEVERSION fields 1 to 3
#   UrVersionPatch  D of t     /
#   UrVersionBuild  floor(secondsOfDay(t) / 2), 0 to 43199: FILEVERSION
#                   field 4. With it the file version rises with the code
#                   between two builds of one day, and Windows Installer
#                   decides file replacement by file version.
#   UrMsiVersion    (Y-2000).M.((D-1)*2048 + floor(secondsOfDay(t)*2048/86400))
#                   The MSI ProductVersion. Windows Installer compares only
#                   its first three fields, at most 255.255.65535, so the
#                   time of day goes into the third: 2048 steps of about
#                   42 s per day, peaking at 30*2048+2047 = 63487 on day 31.
#
# It refuses:
#   - anything that is not that grammar: a leading v, zero-padded numbers,
#     a month above 12, a day above 31, non-ASCII digits, any other suffix;
#   - code 0, which every build reads as "dev build, never update";
#   - a code whose instant is more than 24 h ahead of this host's UTC clock.
#     A mistyped code would otherwise outrank every real release for good.
#     -NowUtc stands in for that clock in tests only, so they can check dates
#     that have not happened yet; build.ps1 and CI never pass it.
#
# Pure ASCII on purpose: Windows PowerShell 5.1 reads a BOM-less UTF-8 script
# as ANSI, and the release VM runs this under 5.1. Every integer step goes
# through [math]::Floor, because PowerShell's / is floating point and a
# [long] cast rounds half to even (code 1060587895 would gain a second).
#
#   $v = & app\tools\UrVersion.ps1 -Version 2026.10.1-1060587890
#   & $msbuild URnetwork.sln @($v.MsbuildArgs)
#   dotnet build installer\Installer.wixproj @($v.WixArgs)
#   pwsh -File app/tools/UrVersion.ps1 -Version <v> -GitHubOutput >> "$GITHUB_OUTPUT"
#
# SPDX-License-Identifier: MPL-2.0
[CmdletBinding()]
param(
  # AllowEmptyString: an empty version is refused below, with the same
  # message as any other malformed one, not by parameter binding.
  [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Version,
  # Print name=value lines for $GITHUB_OUTPUT instead of returning an object.
  [switch]$GitHubOutput,
  # Tests only (tests/ur_version_test.go): the UTC instant, as
  # yyyy-MM-ddTHH:mm:ssZ, that the future-code check measures against instead
  # of this host's clock. Release builds never pass it.
  [string]$NowUtc = ''
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$expected = 'expected <YYYY.M.D>-<code>[-beta], e.g. 2026.10.1-1060587890'

# [regex] is case-sensitive, unlike -match, so -BETA is refused as the update
# checker's grammar (Common/VersionGrammar.h) refuses it. \z, not $: $ also
# matches before a trailing newline. [0-9], not \d: \d matches any Unicode
# digit.
$grammar = [regex]'\A([0-9]{4})\.([0-9]{1,2})\.([0-9]{1,2})-([0-9]{1,18})(-beta)?\z'
$parts = $grammar.Match($Version)
if (-not $parts.Success) {
  throw "UrVersion: not a release version: '$Version' ($expected)"
}
$monthText = $parts.Groups[2].Value
$dayText = $parts.Groups[3].Value
$codeText = $parts.Groups[4].Value
if ($monthText.StartsWith('0') -or $dayText.StartsWith('0') -or
    [int]$monthText -gt 12 -or [int]$dayText -gt 31) {
  throw "UrVersion: not a release version: '$Version' (month 1-12 and day 1-31, unpadded; $expected)"
}
if ($codeText -eq '0') {
  throw "UrVersion: code 0 in '$Version' is the dev-build code: a build stamped with it never updates itself"
}
if ($codeText.StartsWith('0')) {
  throw "UrVersion: not a release version: '$Version' (the code has a leading zero; $expected)"
}

$invariant = [Globalization.CultureInfo]::InvariantCulture
$code = [long]::Parse($codeText, [Globalization.NumberStyles]::None, $invariant)

$founded = [DateTime]::new(2023, 5, 23, 0, 0, 0, [DateTimeKind]::Utc)
$seconds = [long][math]::Floor([decimal]$code / [decimal]10)
$now = [DateTime]::UtcNow
$clockName = "this host's UTC clock"
if ($NowUtc) {
  try {
    $now = [DateTime]::ParseExact($NowUtc, "yyyy-MM-dd'T'HH:mm:ss'Z'", $invariant,
      [Globalization.DateTimeStyles]'AssumeUniversal, AdjustToUniversal')
  } catch {
    throw "UrVersion: -NowUtc '$NowUtc' is not a UTC instant of the form yyyy-MM-ddTHH:mm:ssZ"
  }
  $clockName = "-NowUtc $NowUtc"
}
$nowSeconds = [long][math]::Floor(($now - $founded).TotalSeconds)
if ($seconds -gt $nowSeconds + 86400) {
  $ahead = [math]::Floor(($seconds - $nowSeconds) / 3600)
  throw "UrVersion: future code $code in '$Version': its instant is $ahead h ahead of $clockName (more than 24 h)"
}

$t = $founded.AddTicks($seconds * [TimeSpan]::TicksPerSecond)
$year = $t.Year
$month = $t.Month
$day = $t.Day
$secondOfDay = $t.Hour * 3600 + $t.Minute * 60 + $t.Second
$build = [int][math]::Floor($secondOfDay / 2)
$msiMajor = $year - 2000
$msiBuild = ($day - 1) * 2048 + [int][math]::Floor($secondOfDay * 2048 / 86400)
if ($msiMajor -lt 0 -or $msiMajor -gt 255 -or $msiBuild -gt 65535) {
  throw "UrVersion: outside the MSI ProductVersion range: code $code is $($t.ToString('u', $invariant)), which has no 255.255.65535 form"
}
$msiVersion = "$msiMajor.$month.$msiBuild"

if ($GitHubOutput) {
  "version=$Version"
  "version_code=$code"
  "version_major=$year"
  "version_minor=$month"
  "version_patch=$day"
  "version_build=$build"
  "msi_version=$msiVersion"
  return
}

[pscustomobject]@{
  UrVersion      = $Version
  UrVersionCode  = $code
  UrVersionMajor = $year
  UrVersionMinor = $month
  UrVersionPatch = $day
  UrVersionBuild = $build
  UrMsiVersion   = $msiVersion
  # Passed to the solution build. Directory.Build.props turns them into the
  # UR_* definitions for every compile and both VERSIONINFO resources.
  MsbuildArgs    = @(
    "/p:UrVersion=$Version",
    "/p:UrVersionCode=$code",
    "/p:UrVersionMajor=$year",
    "/p:UrVersionMinor=$month",
    "/p:UrVersionPatch=$day",
    "/p:UrVersionBuild=$build"
  )
  # Passed to the installer build (Installer.wixproj).
  WixArgs        = @("-p:UrMsiVersion=$msiVersion")
}
