# Helpers for .github/workflows/a2c-legs.yml, A2c's runner legs (SPEC-A2c
# 8.4), on top of prf-lib.ps1 (PR F's legs, and through it wild-update-lib.ps1
# and A2b's vpn-*-lib.ps1, all unchanged). Dot-sourced by every leg:
#   . "$env:GITHUB_WORKSPACE\.github\a2c-lib.ps1"
# Throwaway: it lives on ci/a2c-legs-* branches of the fork only and is never
# merged. Runs on a disposable GitHub runner only. Windows are read and
# pressed through UI Automation and captured with PrintWindow; nothing here
# synthesizes mouse or keyboard input. It publishes nothing and creates no
# release or tag: both feeds are read without a token, as the app reads them,
# and the stand-in for GitHub (a2c-standin.ps1) serves their own lists, with
# releases added, on this runner alone.
# Pure ASCII, Windows PowerShell 5.1.
#
# SPDX-License-Identifier: MPL-2.0

# prf-lib.ps1's mode: every A2c leg runs the branch's own build
$env:PRF_MODE = 'after'
. (Join-Path $PSScriptRoot 'prf-lib.ps1')

$A2cOs = [string]$env:ImageOS

# ---- the feeds (Common/ReleaseSelection.h kFeeds) ----------------------------

$A2cDevRepoId = '1324127805'
$A2cDevOwner = 'Ryanmello07'
$A2cDevRepo = 'urnetwork-windows'
$A2cRepository = "$A2cDevOwner/$A2cDevRepo"
$A2cDevListPath = "/repositories/$A2cDevRepoId/releases?per_page=30"
$A2cOfficialListPath = $UrFeedListPath

# ---- the channel (ApplyUpdate.cpp, UpdateChannel.h) ------------------------------

$A2cMarkerPath = Join-Path $UrUpdatesDir 'developer-channel'
$A2cLockPath = Join-Path $UrUpdatesDir 'helper.lock'
$A2cMarkerSddl = 'O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x120080;;;BU)'
$A2cFolderSddl = 'O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)'
$A2cCaption = 'URnetwork: update channel'
$A2cMutexName = 'Global\URnetworkUpdateHelper'

# Common/UpdateResult.h Refusal
$A2cRefusal = @{ NotElevated = 0x20000001; NotInstalled = 0x20000002; DevBuild = 0x20000003; BadArguments = 0x20000004
  Busy = 0x20000005; ReleaseList = 0x20000006; NotOffered = 0x20000007; Download = 0x20000008; Staging = 0x2000000B
  Declined = 0x2000000E }

# What the surfaces say (app/src/App/Strings/en, the store's keys), by SPEC-A2c
# 9's numbers.
$A2cS = @{
  S4  = "Update channel: official releases (urnetwork/build)."
  S5  = "Update channel: developer builds from $A2cRepository. Official releases are not offered while this is on. This is set for every user of this PC."
  S6  = "Waiting for administrator approval"
  S7  = "The update channel was not changed."
  S9  = "An update is in progress. Change the update channel when it is done."
  S13 = "Developer builds are turned on in the Developer screen."
  S14 = "The saved update channel is not trusted, so official releases are used. Switching to official releases removes it."
  S15 = "This is a developer build. It follows official releases, so it is offered nothing until a newer official release has been out for a day."
  S16 = "No official release is offered to this developer build yet"
  S17 = "No developer build that this PC can install is listed"
  S18 = "Developer build available: v"
  S19 = "This is a developer build from $A2cRepository, not an official release."
  S21 = "Developer builds from $A2cRepository. Official releases are not offered while this is on."
  S22 = "Switch to official releases"
  S23 = "Official releases."
  S24 = "No developer build is newer than this build"
}

# The ids A2c adds (tests/update_offer_wiring_test.go checkChannelSurfaces).
$A2cIdSwitch = 'acceptance.update.channel-switch'
$A2cIdLine = 'acceptance.update.channel-line'
$A2cIdRepair = 'acceptance.update.channel-repair'
$A2cIdRow = 'acceptance.update.channel-row'
$A2cIdOfficial = 'acceptance.update.channel-official'
$A2cIdNavDeveloper = 'acceptance.nav.developer'

$A2cDir = Join-Path $env:RUNNER_TEMP 'a2c'
New-Item -ItemType Directory -Force $A2cDir | Out-Null

$A2cFailures = New-Object System.Collections.Generic.List[string]
$A2cPasses = New-Object System.Collections.Generic.List[string]

# A check that does not stop the leg: what it found is said, and the leg's
# verdict counts it.
function Test-A2c([bool]$condition, [string]$what, [string]$found = '') {
  if ($condition) {
    $A2cPasses.Add($what)
    Write-Host "OK: $what"
  } else {
    $A2cFailures.Add("$what$(if ($found) { " (found: $found)" })")
    Write-Host "FAIL: $what$(if ($found) { " (found: $found)" })"
  }
}

function Complete-A2cLeg([string]$leg) {
  Write-Step "verdict of $leg on $A2cOs"
  foreach ($press in $A2cPresses) { Write-Host "the question was pressed: $press" }
  foreach ($pass in $A2cPasses) { Write-Host "passed: $pass" }
  foreach ($failure in $A2cFailures) { Write-Host "FAILED: $failure" }
  if ($A2cFailures.Count -gt 0) { throw "$leg`: $($A2cFailures.Count) of $($A2cPasses.Count + $A2cFailures.Count) checks failed" }
  Write-Host "PASS: $leg, $($A2cPasses.Count) checks"
}

# ---- reading the feeds ---------------------------------------------------------

# A feed's release list as the app asks for it: no token, the repository by
# its id, no redirect followed. Saves GitHub's bytes as $saveAs.
function Get-A2cList([string]$path, [string]$saveAs) {
  $response = Invoke-WebRequest -UseBasicParsing -Uri "https://api.github.com$path" -MaximumRedirection 0 `
    -UserAgent 'URnetwork-Windows/runner-test' -Headers @{ Accept = 'application/vnd.github+json' }
  if ($response.StatusCode -ne 200) { throw "$path answered $($response.StatusCode)" }
  $date = [string]$response.Headers['Date']
  $stream = $response.RawContentStream
  $stream.Position = 0
  $bytes = New-Object byte[] $stream.Length
  [void]$stream.Read($bytes, 0, $bytes.Length)
  [IO.File]::WriteAllBytes($saveAs, $bytes)
  $releases = @([Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json)
  if ($releases.Count -eq 1 -and $releases[0] -is [array]) { $releases = @($releases[0]) }
  $server = ConvertTo-UrUnix $date
  Write-Host "list $path`: $($releases.Count) releases, $($bytes.Length) bytes, Date: $date"
  return [pscustomobject]@{ Releases = $releases; ServerUnix = $server; Date = $date; File = $saveAs; Bytes = $bytes.Length }
}

# This test's own reading of the developer feed, written from SPEC-A2c 3 and
# not from the app's code: drafts are skipped, and so is every release whose
# tag does not end -beta, prerelease or not; of the rest, the newest that is
# immutable, tagged v<YYYY.M.D>-<code>-beta, carries
# URnetwork-<version>-<arch>.msi with a sha256 digest at the fork's own
# download URL, and has a code no more than 48 h ahead of the list's date. No
# soak: it counts from the moment it is published.
function Get-A2cDevReading($releases, [long]$serverUnix, [string]$arch = 'x64') {
  $rows = New-Object System.Collections.Generic.List[object]
  foreach ($release in $releases) {
    $tag = [string]$release.tag_name
    $row = [pscustomobject]@{ Tag = $tag; Why = ''; Code = [uint64]0; Version = ''; Asset = ''; Url = ''; Digest = ''; Counts = $false }
    $match = [regex]::Match($tag, '^v(\d{4})\.(\d{1,2})\.(\d{1,2})-(\d{1,18})-beta$')
    if ($release.draft -isnot [bool] -or $release.draft) { $row.Why = 'a draft' }
    elseif (-not $tag.EndsWith('-beta')) { $row.Why = 'not a -beta tag' }
    elseif (-not $match.Success) { $row.Why = 'a tag outside the grammar' }
    elseif ($release.immutable -ne $true) { $row.Why = 'not immutable' }
    else {
      $row.Version = $tag.Substring(1)
      $row.Code = [uint64]$match.Groups[4].Value
      $codeUnix = $UrFoundedUnix + [long][math]::Floor([decimal]$row.Code / 10)
      $name = "URnetwork-$($row.Version)-$arch.msi"
      $assets = @($release.assets | Where-Object { $_.name -eq $name })
      $wantUrl = "https://github.com/$A2cDevOwner/$A2cDevRepo/releases/download/$tag/$name"
      if ($codeUnix -gt $serverUnix + 48 * 3600) { $row.Why = 'a code more than 48 h ahead of the list' }
      elseif ($assets.Count -ne 1) { $row.Why = "without $name" }
      elseif ([string]$assets[0].digest -notmatch '^sha256:[0-9a-fA-F]{64}$') { $row.Why = 'without a sha256 digest' }
      elseif ([string]$assets[0].browser_download_url -ne $wantUrl) { $row.Why = "at $($assets[0].browser_download_url), not the fork's URL" }
      else {
        $row.Asset = $name
        $row.Url = $wantUrl
        $row.Digest = ([string]$assets[0].digest).Substring(7).ToLowerInvariant()
        $row.Counts = $true
      }
    }
    $rows.Add($row)
  }
  $offered = @($rows | Where-Object { $_.Counts } | Sort-Object Code -Descending | Select-Object -First 1)
  foreach ($row in $rows) { Write-Host ("developer feed: {0,-34} {1}" -f $row.Tag, $(if ($row.Counts) { 'counts' } else { "skipped: $($row.Why)" })) }
  $result = [pscustomobject]@{ Rows = $rows.ToArray(); ServerUnix = $serverUnix; Offered = $null }
  if ($offered.Count -eq 1) { $result.Offered = $offered[0] }
  Write-Host "developer feed: offers $(if ($result.Offered) { $result.Offered.Tag } else { 'nothing' })"
  return $result
}

# ---- the stand-in for GitHub -------------------------------------------------------

# A route of the stand-in's routes file (a2c-standin.ps1).
function New-A2cRoute([string]$hostName, [string]$path, [int]$status, [string]$target, [int]$delay = 0) {
  return "route`t$hostName`t$path`t$status`t$target`t$delay"
}

# The routes that serve $file as $owner/$repo's asset $asset of $tag: the
# download URL redirects once to the release-asset host, which answers after
# $delay seconds.
function New-A2cAssetRoutes([string]$owner, [string]$repo, [string]$tag, [string]$asset, [string]$file, [int]$delay = 0) {
  $path = "/$owner/$repo/releases/download/$tag/$asset"
  return @((New-A2cRoute 'github.com' $path 302 "https://release-assets.githubusercontent.com$path"),
    (New-A2cRoute 'release-assets.githubusercontent.com' $path 200 $file $delay))
}

# Writes the stand-in's routes: its date (a clock running from $unix now, or
# fixed at $unix with -Fixed) and $routes. Written whole beside the file and
# renamed in: the stand-in reads it for every request.
function Set-A2cRoutes($state, [string[]]$routes, [long]$unix, [switch]$Fixed) {
  $date = [DateTimeOffset]::FromUnixTimeSeconds($unix).UtcDateTime.ToString('r', [Globalization.CultureInfo]::InvariantCulture)
  $ticks = [long]0
  if (-not $Fixed) { $ticks = [DateTime]::UtcNow.Ticks }
  $state.ClockBase = $unix
  $state.ClockStartTicks = $ticks
  $lines = @("date`t$date`t$ticks") + $routes
  $written = "$($state.Routes).new"
  [IO.File]::WriteAllLines($written, $lines, (New-Object Text.UTF8Encoding($false)))
  Move-Item -LiteralPath $written -Destination $state.Routes -Force
  $state.Lines = $routes
  Write-Host "stand-in routes: Date $date$(if (-not $Fixed) { ', running' }); $($routes.Count) routes"
  foreach ($route in $routes) { Write-Host "  $($route -replace "`t", ' | ')" }
}

# Starts the stand-in: a test CA trusted machine-wide, the hosts file sending
# GitHub's hosts here, and a2c-standin.ps1 serving $routes. Returns what
# Remove-UrTestNetwork undoes, and what Set-A2cRoutes rewrites.
function Start-A2cStandIn([string[]]$routes, [long]$unix, [string]$name = 'a2c-standin') {
  $state = [pscustomobject]@{ Server = $null; Log = ''; Hosts = $null; Tls = $null; Names = $UrGitHubHosts
    Routes = ''; Lines = @(); ClockBase = $unix; ClockStartTicks = [long]0 }
  try {
    $work = New-Item -ItemType Directory -Force (Join-Path $env:RUNNER_TEMP $name)
    $state.Routes = Join-Path $work 'routes.txt'
    $state.Log = Join-Path $work 'server.log'
    Set-A2cRoutes $state $routes $unix
    $state.Tls = New-UrTestTls $UrGitHubHosts
    $state.Hosts = Add-UrHostsEntries $UrGitHubHosts
    $state.Server = Start-UrTestServer 'a2c-standin.ps1' @('-Thumbprint', $state.Tls.Leaf, '-Routes', $state.Routes, '-Log', $state.Log) $state.Log
    return $state
  } catch {
    Remove-UrTestNetwork $state
    throw
  }
}

# The stand-in's log lines that match $pattern.
function Get-A2cStandInLines($state, [string]$pattern) {
  if (-not (Test-Path -LiteralPath $state.Log)) { return ,@() }
  return ,@(Get-Content -LiteralPath $state.Log | Where-Object { $_ -match $pattern })
}

# GitHub's own list (the bytes of $listFile) with one release put in front,
# as GitHub lists one: $tag, out since $publishedUnix, neither a draft nor
# (unless $prerelease) a prerelease, immutable as $immutable says, its x64 MSI
# $asset at $owner/$repo's download URL, with the SHA-256 and size of $msi
# (or of nothing, for a release only listed). Writes it to $out; returns the
# list.
function Add-A2cRelease([string]$listFile, [string]$owner, [string]$repo, [string]$tag, [string]$asset, [string]$msi,
                        [long]$publishedUnix, [bool]$prerelease, [bool]$immutable, [string]$out) {
  $text = [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($listFile)).Trim()
  if (-not $text.StartsWith('[')) { throw "the release list is not a JSON array" }
  if ($text.Contains('"tag_name":"' + $tag + '"')) { throw "the list already names $tag" }
  $digest = ('0' * 64)
  $size = 0
  if ($msi) {
    $digest = (Get-FileHash -Algorithm SHA256 -LiteralPath $msi).Hash.ToLowerInvariant()
    $size = (Get-Item -LiteralPath $msi).Length
  } else {
    $digest = ('ab' * 32)
    $size = 1000000
  }
  $at = Format-UrUnix $publishedUnix
  $url = "https://github.com/$owner/$repo/releases/download/$tag/$asset"
  $release = '{"url":"https://api.github.com/repos/' + $owner + '/' + $repo + '/releases/1","id":1,' +
    '"tag_name":"' + $tag + '","name":"' + $tag + '","draft":false,"prerelease":' + $prerelease.ToString().ToLowerInvariant() +
    ',"immutable":' + $immutable.ToString().ToLowerInvariant() + ',' +
    '"created_at":"' + $at + '","published_at":"' + $at + '","updated_at":"' + $at + '",' +
    '"assets":[{"name":"' + $asset + '","content_type":"application/x-msi","state":"uploaded","size":' + $size + ',' +
    '"digest":"sha256:' + $digest + '","created_at":"' + $at + '","updated_at":"' + $at + '",' +
    '"browser_download_url":"' + $url + '"}]}'
  $rest = $text.Substring(1).TrimStart()
  $separator = ','
  if ($rest.StartsWith(']')) { $separator = '' }
  $joined = '[' + $release + $separator + $rest
  [IO.File]::WriteAllBytes($out, [Text.Encoding]::UTF8.GetBytes($joined))
  $releases = @($joined | ConvertFrom-Json)
  if ($releases.Count -eq 1 -and $releases[0] -is [array]) { $releases = @($releases[0]) }
  Write-Host "list $(Split-Path -Leaf $out): $tag added in front of $($releases.Count - 1) releases of $owner/$repo (prerelease $prerelease, immutable $immutable, $asset$(if ($msi) { ", $size bytes, sha256 $digest" } else { ', listed only' }), out since $at)"
  return [pscustomobject]@{ Releases = $releases; File = $out }
}

# ---- the marker and the lock -------------------------------------------------------

# A file's owner and DACL in SDDL, or '' when it cannot be read.
function Get-A2cSddl([string]$path) {
  try {
    return (Get-Acl -LiteralPath $path).GetSecurityDescriptorSddlForm([Security.AccessControl.AccessControlSections]'Owner, Access')
  } catch { return '' }
}

# What lies at the marker's name: whether anything does, and what.
function Get-A2cMarker {
  $item = Get-Item -LiteralPath $A2cMarkerPath -Force -ErrorAction SilentlyContinue
  if (-not $item) { return [pscustomobject]@{ Exists = $false; Text = 'no marker' } }
  $attributes = [int]$item.Attributes
  $directory = ($attributes -band 0x10) -ne 0
  $reparse = ($attributes -band 0x400) -ne 0
  $length = 0
  if (-not $directory) { $length = $item.Length }
  $sddl = Get-A2cSddl $A2cMarkerPath
  $text = "a $(if ($directory) { 'folder' } else { 'file' })$(if ($reparse) { ' (a reparse point)' }), $length bytes, $sddl"
  return [pscustomobject]@{ Exists = $true; Directory = $directory; Reparse = $reparse; Length = $length; Sddl = $sddl; Text = $text }
}

# Whether the marker is there as the helper makes it: a plain empty file with
# exactly the security of SPEC-A2c 2.1, in a folder with the helper's own.
function Test-A2cMarkerTrusted {
  $marker = Get-A2cMarker
  Write-Host "the marker: $($marker.Text); updates\ $(Get-A2cSddl $UrUpdatesDir)"
  return ($marker.Exists -and -not $marker.Directory -and -not $marker.Reparse -and $marker.Length -eq 0 -and
    $marker.Sddl -eq $A2cMarkerSddl)
}

# Writes what lies at the marker's name, as the elevated test (nothing else
# could): $kind is the shape, $sddl the security a file gets.
function Set-A2cMarkerAs([string]$kind, [string]$sddl = $A2cMarkerSddl) {
  Clear-A2cMarker
  switch ($kind) {
    'folder' { New-Item -ItemType Directory -Path $A2cMarkerPath | Out-Null }
    'link' {
      $target = Join-Path $A2cDir 'link-target.txt'
      Set-Content -LiteralPath $target -Value '' -Encoding ascii
      New-Item -ItemType SymbolicLink -Path $A2cMarkerPath -Target $target | Out-Null
    }
    'bytes' {
      [IO.File]::WriteAllBytes($A2cMarkerPath, [byte[]](0x78))
      $acl = Get-Acl -LiteralPath $A2cMarkerPath
      $acl.SetSecurityDescriptorSddlForm($sddl, [Security.AccessControl.AccessControlSections]'Owner, Access')
      Set-Acl -LiteralPath $A2cMarkerPath -AclObject $acl
    }
    'file' {
      [IO.File]::WriteAllBytes($A2cMarkerPath, [byte[]]@())
      $acl = Get-Acl -LiteralPath $A2cMarkerPath
      $acl.SetSecurityDescriptorSddlForm($sddl, [Security.AccessControl.AccessControlSections]'Owner, Access')
      Set-Acl -LiteralPath $A2cMarkerPath -AclObject $acl
    }
    default { throw "no marker kind $kind" }
  }
  Write-Host "the test wrote at the marker's name: $((Get-A2cMarker).Text)"
}

# Takes away whatever lies at the marker's name, as the elevated test.
function Clear-A2cMarker {
  $item = Get-Item -LiteralPath $A2cMarkerPath -Force -ErrorAction SilentlyContinue
  if (-not $item) { return }
  if (([int]$item.Attributes -band 0x400) -ne 0) {
    # a link goes, never what it names
    if (([int]$item.Attributes -band 0x10) -ne 0) { [IO.Directory]::Delete($A2cMarkerPath) } else { [IO.File]::Delete($A2cMarkerPath) }
  } elseif (([int]$item.Attributes -band 0x10) -ne 0) {
    [IO.Directory]::Delete($A2cMarkerPath)
  } else {
    [IO.File]::Delete($A2cMarkerPath)
  }
}

# ---- windows: the helper's question, z-order, and captures ---------------------------

$A2cWinSource = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class A2cWin {
  [StructLayout(LayoutKind.Sequential)]
  public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] static extern int GetWindowLongW(IntPtr h, int index);
  [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")]
  static extern IntPtr SendMessageTimeoutW(IntPtr h, uint msg, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);
  [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageTimeoutW")]
  static extern IntPtr SendMessageTimeoutText(IntPtr h, uint msg, IntPtr w, StringBuilder l, uint flags, uint timeout, out IntPtr result);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr dialog, int id);
  [DllImport("user32.dll", SetLastError = true)] static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);

  // A control's text by WM_GETTEXT, which reaches a control of another
  // process: a read.
  public static string Text(IntPtr h) {
    IntPtr length;
    if (SendMessageTimeoutW(h, 0x000E, IntPtr.Zero, IntPtr.Zero, 0x0002, 2000, out length) == IntPtr.Zero) return "";
    StringBuilder text = new StringBuilder(length.ToInt32() + 1);
    IntPtr copied;
    SendMessageTimeoutText(h, 0x000D, (IntPtr)text.Capacity, text, 0x0002, 2000, out copied);
    return text.ToString();
  }

  // The fallback press: the button's own BN_CLICKED, posted to its dialog
  // as a click delivers it. A window message, not input. 0, or the error.
  public static int PostCommand(IntPtr dialog, int id) {
    IntPtr button = GetDlgItem(dialog, id);
    return PostMessageW(dialog, 0x0111, (IntPtr)id, button) ? 0 : Marshal.GetLastWin32Error();
  }

  public static string Title(IntPtr h) { StringBuilder s = new StringBuilder(512); GetWindowTextW(h, s, s.Capacity); return s.ToString(); }
  public static string ClassName(IntPtr h) { StringBuilder s = new StringBuilder(256); GetClassNameW(h, s, s.Capacity); return s.ToString(); }

  // The dialog's default button, by DM_GETDEFID: its id, or 0 when it has
  // none. A window message that asks; nothing is pressed.
  public static int DefaultButton(IntPtr dialog) {
    IntPtr result;
    if (SendMessageTimeoutW(dialog, 0x0400, IntPtr.Zero, IntPtr.Zero, 0x0002, 2000, out result) == IntPtr.Zero) return -1;
    int value = result.ToInt32();
    return ((value >> 16) & 0xFFFF) == 0x534B ? (value & 0xFFFF) : 0;
  }

  // Where the window is and how it shows: visible, minimized, topmost, its
  // rectangle against the screen, and whether it is the foreground window.
  public static string Placement(IntPtr h, out bool shown) {
    RECT r;
    GetWindowRect(h, out r);
    int width = GetSystemMetrics(0), height = GetSystemMetrics(1);
    bool visible = IsWindowVisible(h), minimized = IsIconic(h);
    bool topmost = (GetWindowLongW(h, -20) & 0x00000008) != 0;
    bool onScreen = r.Left >= 0 && r.Top >= 0 && r.Right <= width && r.Bottom <= height && r.Right > r.Left && r.Bottom > r.Top;
    bool foreground = GetForegroundWindow() == h;
    shown = visible && !minimized && onScreen;
    return "visible=" + visible + " minimized=" + minimized + " topmost=" + topmost + " foreground=" + foreground +
           " at " + r.Left + "," + r.Top + " " + (r.Right - r.Left) + "x" + (r.Bottom - r.Top) + " on a " + width + "x" + height + " screen";
  }

  // The visible windows above `h` in the z-order that overlap it: each
  // "pid <id> <class> '<title>'". None means nothing covers it.
  public static string[] Above(IntPtr h) {
    List<string> found = new List<string>();
    RECT mine;
    GetWindowRect(h, out mine);
    for (IntPtr w = GetWindow(h, 3); w != IntPtr.Zero; w = GetWindow(w, 3)) {
      if (!IsWindowVisible(w) || IsIconic(w)) continue;
      RECT r;
      if (!GetWindowRect(w, out r)) continue;
      if (r.Right <= mine.Left || r.Left >= mine.Right || r.Bottom <= mine.Top || r.Top >= mine.Bottom) continue;
      uint pid;
      GetWindowThreadProcessId(w, out pid);
      found.Add("pid " + pid + " " + ClassName(w) + " '" + Title(w) + "'");
    }
    return found.ToArray();
  }
}
'@


# UI Automation's own providers for Win32 controls (a dialog's buttons and
# text), registered once: without them a dialog's controls read as bare
# windows, with no Invoke pattern.
$script:A2cProvidersRegistered = $false
function Register-A2cWin32Providers {
  if ($script:A2cProvidersRegistered) { return }
  $script:A2cProvidersRegistered = $true
  try {
    Add-Type -AssemblyName UIAutomationClientsideProviders
    # the client is set up on its first use; registering before it fails
    [void][System.Windows.Automation.AutomationElement]::RootElement.Current.Name
    [System.Windows.Automation.ClientSettings]::RegisterClientSideProviderAssembly(
      [UIAutomationClientsideProviders.UIAutomationClientSideProviders].Assembly.GetName())
    Write-Host "UI Automation: the client-side providers for Win32 controls are registered"
  } catch {
    Write-Host "UI Automation: the client-side providers were not registered: $($_.Exception.Message)"
  }
}

function Initialize-A2cWin {
  Initialize-UrUi
  Register-A2cWin32Providers
  if (-not ('A2cWin' -as [type])) { Add-Type -TypeDefinition $A2cWinSource }
}

# The helper's question (ApplyUpdate.cpp AdministratorConfirms): a top-level
# window named $A2cCaption, found through UI Automation within $seconds. Its
# element, handle, process, texts, buttons, default button and placement; or
# $null.
function Find-A2cDialog([int]$seconds = 30) {
  Initialize-A2cWin
  $condition = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::NameProperty, $A2cCaption)
  $deadline = (Get-Date).AddSeconds($seconds)
  do {
    $dialog = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst([System.Windows.Automation.TreeScope]::Children, $condition)
    if ($dialog) {
      Start-Sleep -Milliseconds 500
      return Get-A2cDialogFacts $dialog
    }
    Start-Sleep -Milliseconds 250
  } while ((Get-Date) -lt $deadline)
  return $null
}

# Every element under $element, with its control type, id, name, class and
# patterns, for the log.
function Show-A2cTree($element, [string]$what) {
  foreach ($e in $element.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)) {
    try {
      $patterns = @($e.GetSupportedPatterns() | ForEach-Object { $_.ProgrammaticName -replace 'PatternIdentifiers.Pattern', '' }) -join ','
      Write-Host ("{0}: {1} id='{2}' name='{3}' class='{4}' hwnd={5} patterns={6}" -f $what, $e.Current.ControlType.ProgrammaticName,
        $e.Current.AutomationId, $e.Current.Name, $e.Current.ClassName, $e.Current.NativeWindowHandle, $patterns)
    } catch { }
  }
}

function Get-A2cDialogFacts($dialog) {
  $hwnd = [IntPtr]$dialog.Current.NativeWindowHandle
  $processId = [int]$dialog.Current.ProcessId
  $process = Get-CimInstance Win32_Process -Filter "ProcessId=$processId" -ErrorAction SilentlyContinue
  $buttonType = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Button)
  # the tree of a window just made can be read before its controls are in it
  $deadline = (Get-Date).AddSeconds(10)
  do {
    $buttons = @($dialog.FindAll([System.Windows.Automation.TreeScope]::Descendants, $buttonType) |
      ForEach-Object { "$($_.Current.AutomationId)='$($_.Current.Name)'" })
    $texts = Get-UrTexts $dialog
    if ($buttons.Count -ge 2 -and $texts.Count -ge 1) { break }
    Start-Sleep -Milliseconds 300
  } while ((Get-Date) -lt $deadline)
  # the message, read from its static control too (id 0xFFFF), in case UI
  # Automation gives no text
  $message = [A2cWin]::Text([A2cWin]::GetDlgItem($hwnd, 0xFFFF))
  if ($texts.Count -eq 0 -and $message) { $texts = @($message) }
  $shown = $false
  $placement = [A2cWin]::Placement($hwnd, [ref]$shown)
  $facts = [pscustomobject]@{ Element = $dialog; Hwnd = $hwnd; ProcessId = $processId
    Process = $(if ($process) { [string]$process.Name } else { '' }); CommandLine = $(if ($process) { [string]$process.CommandLine } else { '' })
    Class = [A2cWin]::ClassName($hwnd); Texts = $texts; Message = $message; Buttons = $buttons
    Default = [A2cWin]::DefaultButton($hwnd); Placement = $placement; Shown = $shown; Above = @([A2cWin]::Above($hwnd)) }
  Show-A2cTree $dialog 'dialog element'
  Write-Host "dialog '$A2cCaption': pid $processId $($facts.Process) ($($facts.CommandLine)), class $($facts.Class)"
  Write-Host "dialog texts: $($facts.Texts -join ' | ')"
  Write-Host "dialog buttons: $($buttons -join ', '); default button id $($facts.Default); $placement"
  Write-Host "dialog: windows above it that overlap it: $(if ($facts.Above.Count) { $facts.Above -join '; ' } else { 'none' })"
  return $facts
}

# How each press of the question was made, for the leg's verdict.
$A2cPresses = New-Object System.Collections.Generic.List[string]

# UI Automation's own COM API, for the presses: the .NET client reads a
# dialog's Win32 controls as bare windows with no pattern, where
# UIAutomationCore's providers give its buttons the Invoke pattern.
$A2cUiaSource = @'
using System;
using System.Runtime.InteropServices;

public static class A2cUia {
  [ComImport, Guid("ff48dba4-60ef-4201-aa87-54103eef594e")]
  class CUIAutomation { }

  [ComImport, Guid("352ffba8-0973-437c-a61f-f64cafd81df9"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IUIAutomationCondition { }

  [ComImport, Guid("fb377fbe-8ea6-46d5-9c73-6499642d3059"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IUIAutomationInvokePattern {
    void Invoke();
  }

  [ComImport, Guid("d22108aa-8ac5-49a5-837b-37bbb3d7591e"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IUIAutomationElement {
    // SetFocus, GetRuntimeId
    void _VtblGap1_2();
    [return: MarshalAs(UnmanagedType.Interface)]
    IUIAutomationElement FindFirst(int scope, [MarshalAs(UnmanagedType.Interface)] IUIAutomationCondition condition);
    // FindAll through GetCachedPatternAs
    void _VtblGap2_10();
    [return: MarshalAs(UnmanagedType.IUnknown)]
    object GetCurrentPattern(int patternId);
  }

  [ComImport, Guid("30cbe57d-d9d0-452a-ab13-7ac5ac4825ee"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IUIAutomation {
    // CompareElements, CompareRuntimeIds, GetRootElement
    void _VtblGap1_3();
    [return: MarshalAs(UnmanagedType.Interface)]
    IUIAutomationElement ElementFromHandle(IntPtr hwnd);
    // ElementFromPoint through CreateFalseCondition
    void _VtblGap2_16();
    [return: MarshalAs(UnmanagedType.Interface)]
    IUIAutomationCondition CreatePropertyCondition(int propertyId, [MarshalAs(UnmanagedType.Struct)] object value);
  }

  // Presses the control with `automationId` under the window `hwnd` through
  // UI Automation's Invoke pattern, as a click does. Says what it did.
  public static string Invoke(IntPtr hwnd, string automationId) {
    try {
      IUIAutomation uia = (IUIAutomation)new CUIAutomation();
      IUIAutomationElement window = uia.ElementFromHandle(hwnd);
      // UIA_AutomationIdPropertyId; TreeScope_Descendants
      IUIAutomationElement control = window.FindFirst(4, uia.CreatePropertyCondition(30011, automationId));
      if (control == null) return "no element with AutomationId " + automationId;
      // UIA_InvokePatternId
      object pattern = control.GetCurrentPattern(10000);
      if (pattern == null) return "no Invoke pattern";
      ((IUIAutomationInvokePattern)pattern).Invoke();
      return "invoked";
    } catch (Exception e) {
      return "failed: " + e.GetType().Name + ": " + e.Message;
    }
  }
}
'@

function Initialize-A2cUia {
  if (-not ('A2cUia' -as [type])) { Add-Type -TypeDefinition $A2cUiaSource }
}

# Presses the question's button $id (1 OK, 2 Cancel) through its Invoke
# pattern, as a click does: the button reached through its own window first,
# then through the dialog's tree. Only when neither offers the Invoke pattern
# is the button's own BN_CLICKED posted to its dialog, and the leg says so.
function Invoke-A2cDialogButton($facts, [string]$id) {
  Initialize-A2cUia
  $pattern = $null
  $deadline = (Get-Date).AddSeconds(15)
  do {
    $native = [A2cUia]::Invoke($facts.Hwnd, $id)
    if ($native -eq 'invoked') {
      Write-Host "invoke the question's button $id through UI Automation (UIAutomationCore's Invoke pattern)"
      $A2cPresses.Add("button $id by UI Automation's Invoke pattern (UIAutomationCore)")
      return
    }
    Write-Host "UI Automation (UIAutomationCore) on the question's button $id`: $native"
    $buttonHwnd = [A2cWin]::GetDlgItem($facts.Hwnd, [int]$id)
    if ($buttonHwnd -ne [IntPtr]::Zero) {
      try {
        $button = [System.Windows.Automation.AutomationElement]::FromHandle($buttonHwnd)
        if ($button.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) {
          Write-Host "invoke the question's button $id ('$($button.Current.Name)', reached through its window $buttonHwnd)"
          $pattern.Invoke()
          $A2cPresses.Add("button $id by UI Automation's Invoke pattern")
          return
        }
      } catch { Write-Host "the button's window $buttonHwnd gave no element: $($_.Exception.Message)" }
    }
    $condition = New-Object System.Windows.Automation.AndCondition(
      (New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::AutomationIdProperty, $id)),
      (New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Button)))
    $button = $facts.Element.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
    if ($button -and $button.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) {
      Write-Host "invoke the question's button $id ('$($button.Current.Name)', found in the dialog's tree)"
      $pattern.Invoke()
      $A2cPresses.Add("button $id by UI Automation's Invoke pattern")
      return
    }
    Start-Sleep -Milliseconds 500
  } while ((Get-Date) -lt $deadline)
  Show-A2cTree $facts.Element 'dialog element, no Invoke pattern'
  $result = [A2cWin]::PostCommand($facts.Hwnd, [int]$id)
  Write-Host "OBSERVED: UI Automation offers no Invoke pattern on the question's button $id; its BN_CLICKED was posted to the dialog instead ($(if ($result -eq 0) { 'posted' } else { "PostMessage error $result" }))"
  $A2cPresses.Add("button $id by its dialog's WM_COMMAND (no Invoke pattern)")
}

# Captures the question as <name>.png (PrintWindow) and the screen beside it.
function Save-A2cDialogShot($facts, [string]$name) {
  Initialize-UrUi
  $path = Join-Path $UrShotDir "$name.png"
  Write-Host "screenshot $name.png: $([UrUiApi]::SaveWindow($facts.Hwnd, $path))"
  try {
    $screen = Join-Path $UrShotDir "$name.screen.png"
    Write-Host "screenshot $name.screen.png: $([UrUiApi]::SaveScreen($screen))"
  } catch { Write-Host "screenshot $name.screen.png: not taken ($($_.Exception.Message))" }
}

# ---- a process without administrator rights ------------------------------------------

$A2cFilteredSource = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class A2cFiltered {
  [StructLayout(LayoutKind.Sequential)] struct SID_AND_ATTRIBUTES { public IntPtr Sid; public uint Attributes; }
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  struct STARTUPINFO {
    public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
    public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
  }
  [StructLayout(LayoutKind.Sequential)] struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
  [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
  [DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenProcessToken(IntPtr p, uint a, out IntPtr t);
  [DllImport("advapi32.dll", SetLastError = true)]
  static extern bool CreateRestrictedToken(IntPtr existing, uint flags, uint disableCount, SID_AND_ATTRIBUTES[] disable,
                                           uint deleteCount, IntPtr delete, uint restrictCount, IntPtr restrict, out IntPtr created);
  [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool ConvertStringSidToSidW(string s, out IntPtr sid);
  [DllImport("advapi32.dll", SetLastError = true)] static extern bool SetTokenInformation(IntPtr t, int c, IntPtr info, int length);
  [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern bool CreateProcessAsUserW(IntPtr t, string app, string cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags,
                                          IntPtr env, string dir, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
  [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern bool CreateProcessWithTokenW(IntPtr t, uint logon, string app, string cmd, uint flags, IntPtr env, string dir,
                                             ref STARTUPINFO si, out PROCESS_INFORMATION pi);
  [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h, uint ms);
  [DllImport("kernel32.dll")] static extern bool GetExitCodeProcess(IntPtr h, out uint code);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);

  static readonly Dictionary<int, IntPtr> started = new Dictionary<int, IntPtr>();

  // The creation flags of every start: DETACHED_PROCESS (0x8) unless the
  // runner's probe chose another. With CREATE_NEW_CONSOLE (0x10) the console
  // is hidden.
  public static uint Flags = 0x8;

  // Starts `commandLine` with this process's token filtered as UAC filters an
  // administrator's (LUA_TOKEN, every privilege but a standard user's gone,
  // Administrators deny-only) and at Medium integrity, on the interactive
  // desktop. Returns its pid; `how` says which call made it, or why none did.
  public static int Start(string commandLine, out string how) {
    IntPtr token, filtered, admins, medium;
    if (!OpenProcessToken(GetCurrentProcess(), 0x0002 | 0x0001 | 0x0008 | 0x0080 | 0x0100, out token)) {
      how = "OpenProcessToken " + Marshal.GetLastWin32Error(); return 0;
    }
    ConvertStringSidToSidW("S-1-5-32-544", out admins);
    SID_AND_ATTRIBUTES[] disable = { new SID_AND_ATTRIBUTES { Sid = admins, Attributes = 0 } };
    // DISABLE_MAX_PRIVILEGE | LUA_TOKEN
    if (!CreateRestrictedToken(token, 0x1 | 0x4, 1, disable, 0, IntPtr.Zero, 0, IntPtr.Zero, out filtered)) {
      how = "CreateRestrictedToken " + Marshal.GetLastWin32Error(); return 0;
    }
    ConvertStringSidToSidW("S-1-16-8192", out medium);
    IntPtr label = Marshal.AllocHGlobal(IntPtr.Size * 2);
    Marshal.WriteIntPtr(label, medium);
    Marshal.WriteInt32(label, IntPtr.Size, 0x20);  // SE_GROUP_INTEGRITY
    if (!SetTokenInformation(filtered, 25, label, IntPtr.Size * 2)) {
      how = "SetTokenInformation " + Marshal.GetLastWin32Error(); return 0;
    }
    STARTUPINFO si = new STARTUPINFO();
    si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
    si.lpDesktop = "winsta0\\default";
    if ((Flags & 0x10) != 0) {
      si.dwFlags = 1;  // STARTF_USESHOWWINDOW
      si.wShowWindow = 0;  // SW_HIDE
    }
    PROCESS_INFORMATION pi;
    if (CreateProcessAsUserW(filtered, null, commandLine, IntPtr.Zero, IntPtr.Zero, false, Flags, IntPtr.Zero, null, ref si, out pi)) {
      how = "CreateProcessAsUserW";
    } else {
      int asUser = Marshal.GetLastWin32Error();
      if (!CreateProcessWithTokenW(filtered, 0, null, commandLine, Flags, IntPtr.Zero, null, ref si, out pi)) {
        how = "CreateProcessAsUserW " + asUser + ", CreateProcessWithTokenW " + Marshal.GetLastWin32Error(); return 0;
      }
      how = "CreateProcessWithTokenW (CreateProcessAsUserW " + asUser + ")";
    }
    CloseHandle(pi.hThread);
    started[pi.dwProcessId] = pi.hProcess;
    return pi.dwProcessId;
  }

  // Its exit code once it ends within `ms`, or -1.
  public static long Wait(int pid, uint ms) {
    IntPtr process = started[pid];
    if (WaitForSingleObject(process, ms) != 0) return -1;
    uint code;
    GetExitCodeProcess(process, out code);
    return code;
  }
}
'@

function Initialize-A2cFiltered {
  if (-not ('A2cFiltered' -as [type])) { Add-Type -TypeDefinition $A2cFilteredSource }
  # every program started filtered is a Windows program: no console flag
  [A2cFiltered]::Flags = [uint32]0
}

# a2c-medium.exe, built once from a2c-medium.cs and UI Automation's COM API
# ($A2cUiaSource): the program the filtered child runs.
$A2cMediumExe = Join-Path $A2cDir 'a2c-medium.exe'
function Build-A2cMedium {
  if (Test-Path -LiteralPath $A2cMediumExe) { return }
  $uia = Join-Path $A2cDir 'a2c-uia.cs'
  [IO.File]::WriteAllText($uia, $A2cUiaSource, (New-Object Text.UTF8Encoding($false)))
  Add-Type -Path @($uia, (Join-Path $PSScriptRoot 'a2c-medium.cs')) -OutputAssembly $A2cMediumExe -OutputType WindowsApplication
  Write-Host "built $A2cMediumExe ($((Get-Item -LiteralPath $A2cMediumExe).Length) bytes)"
}

# Starts a2c-medium.exe <action> <out> <arguments> under the filtered token,
# writing to a new file; returns its pid and that file.
function Start-A2cMedium([string]$action, [string[]]$arguments) {
  Initialize-A2cUia
  Initialize-A2cFiltered
  Build-A2cMedium
  $out = Join-Path $A2cDir ("medium-$action-" + [Guid]::NewGuid().ToString('N').Substring(0, 8) + '.txt')
  $quoted = @($arguments | ForEach-Object { if ($_ -eq '' -or $_ -match '\s') { '"' + $_ + '"' } else { $_ } })
  $command = "`"$A2cMediumExe`" $action `"$out`" $($quoted -join ' ')"
  $how = ''
  $processId = [A2cFiltered]::Start($command, [ref]$how)
  if ($processId -eq 0) { throw "the filtered child did not start: $how" }
  Write-Host "filtered child pid $processId ($how): $action $($quoted -join ' '); its token: $(Get-WildTokenText $processId)"
  return [pscustomobject]@{ Id = $processId; Out = $out; How = $how }
}

# What the filtered child wrote, as key -> value.
function Read-A2cMedium($child) {
  $facts = [ordered]@{}
  if (Test-Path -LiteralPath $child.Out) {
    foreach ($line in (Get-Content -LiteralPath $child.Out)) {
      $at = $line.IndexOf(': ')
      if ($at -gt 0) { $facts[$line.Substring(0, $at)] = $line.Substring($at + 2) }
      Write-Host "filtered child $($child.Id): $line"
    }
  }
  return $facts
}

# Runs a2c-medium.exe under the filtered token to its end; returns what it wrote.
function Invoke-A2cMedium([string]$action, [string[]]$arguments, [int]$seconds = 120) {
  $child = Start-A2cMedium $action $arguments
  $code = [A2cFiltered]::Wait($child.Id, [uint32]($seconds * 1000))
  if ($code -lt 0) { throw "the filtered child $($child.Id) did not end within $seconds s" }
  Write-Host "filtered child $($child.Id) exit code $code"
  return Read-A2cMedium $child
}

# Waits for the filtered child to say $key; returns what it wrote then.
function Wait-A2cMedium($child, [string]$key, [int]$seconds = 60) {
  $deadline = (Get-Date).AddSeconds($seconds)
  do {
    if ((Test-Path -LiteralPath $child.Out) -and (Select-String -LiteralPath $child.Out -Pattern "^$([regex]::Escape($key)): " -Quiet)) {
      return Read-A2cMedium $child
    }
    Start-Sleep -Milliseconds 250
  } while ((Get-Date) -lt $deadline)
  [void](Read-A2cMedium $child)
  throw "the filtered child $($child.Id) did not say '$key' within $seconds s"
}

# Runs $exe $arguments under the filtered token; returns its exit code.
function Invoke-A2cFilteredExe([string]$exe, [string]$arguments, [int]$seconds = 60) {
  Initialize-A2cFiltered
  $how = ''
  $processId = [A2cFiltered]::Start("`"$exe`" $arguments", [ref]$how)
  if ($processId -eq 0) { throw "$exe did not start filtered: $how" }
  Write-Host "filtered: $exe $arguments is pid $processId ($how); its token: $(Get-WildTokenText $processId)"
  $code = [A2cFiltered]::Wait($processId, [uint32]($seconds * 1000))
  if ($code -lt 0) { throw "$exe (pid $processId) did not end within $seconds s" }
  Write-Host ("filtered: exit code {0} (0x{0:X8})" -f $code)
  return $code
}

# ---- the helper, started by the test --------------------------------------------------

# Every start of URnetworkUpdate.exe on the machine, from Windows' own trace
# of process starts, from now on.
function Start-A2cHelperWatch {
  Get-EventSubscriber -SourceIdentifier 'a2c-helper-start' -ErrorAction SilentlyContinue | Unregister-Event
  Get-Event -SourceIdentifier 'a2c-helper-start' -ErrorAction SilentlyContinue | Remove-Event
  Register-CimIndicationEvent -Namespace 'root/cimv2' -SourceIdentifier 'a2c-helper-start' `
    -Query "SELECT * FROM Win32_ProcessStartTrace WHERE ProcessName = 'URnetworkUpdate.exe'" | Out-Null
  Write-Host "watching for URnetworkUpdate.exe starts"
}

# The helper starts seen so far: each "pid <id> <command line>" (the line as
# Win32_Process gives it while the process lives).
function Get-A2cHelperStarts {
  $starts = New-Object System.Collections.Generic.List[string]
  foreach ($event in @(Get-Event -SourceIdentifier 'a2c-helper-start' -ErrorAction SilentlyContinue)) {
    $processId = [int]$event.SourceEventArgs.NewEvent.ProcessID
    $process = Get-CimInstance Win32_Process -Filter "ProcessId=$processId" -ErrorAction SilentlyContinue
    $line = ''
    if ($process) { $line = [string]$process.CommandLine }
    $starts.Add("pid $processId $(if ($line) { $line } else { '(ended)' })")
  }
  return ,$starts.ToArray()
}

function Clear-A2cHelperStarts {
  Get-Event -SourceIdentifier 'a2c-helper-start' -ErrorAction SilentlyContinue | Remove-Event
}

# Runs the installed helper with $arguments (an array) and waits; returns the
# process (exited) and its exit code. With $dialog set to 'ok' or 'cancel',
# it presses that button of the question that must come.
function Invoke-A2cHelper([string[]]$arguments, [string]$dialog = '', [int]$seconds = 300, [string]$shot = '') {
  Write-Host "$UrHelperPath $($arguments -join ' ')"
  $helper = Start-Process -FilePath $UrHelperPath -ArgumentList $arguments -PassThru
  $null = $helper.Handle
  $facts = $null
  if ($dialog) {
    $facts = Find-A2cDialog 60
    if (-not $facts) { throw "the helper asked nothing within 60 s" }
    if ($shot) { Save-A2cDialogShot $facts $shot }
    Invoke-A2cDialogButton $facts $(if ($dialog -eq 'ok') { '1' } else { '2' })
  }
  if (-not $helper.WaitForExit($seconds * 1000)) {
    Stop-Process -Id $helper.Id -Force -ErrorAction SilentlyContinue
    throw "the helper (pid $($helper.Id)) was still running after $seconds s"
  }
  $helper.WaitForExit()
  Write-Host ("helper pid {0} exit code {1} (0x{1:X8})" -f $helper.Id, $helper.ExitCode)
  return [pscustomobject]@{ Process = $helper; ExitCode = [long]$helper.ExitCode; Dialog = $facts }
}

# The channel verb, run as an administrator does: --set-update-channel $id,
# answering the question for the developer channel with $answer.
function Set-A2cChannel([string]$id, [string]$answer = 'ok') {
  $dialog = ''
  if ($id -eq 'developer' -and -not (Test-A2cMarkerTrusted)) { $dialog = $answer }
  $run = Invoke-A2cHelper @('--set-update-channel', $id) $dialog
  return $run
}

# The helper's log for $tag's folder, as lines.
function Get-A2cHelperLog([string]$tag) { return Get-UrHelperLog $tag }

# ---- the app's window ---------------------------------------------------------------

# Brings the element with $automationId into view through its ScrollItem
# pattern (UI Automation, not input) and returns it; $null when absent.
function Show-A2cId([int]$processId, [string]$automationId) {
  $window = Get-UrMainWindow $processId
  if (-not $window) { throw "the app (pid $processId) has no window" }
  $element = Find-UrId $window $automationId
  if (-not $element) { return $null }
  $pattern = $null
  if ($element.Current.IsOffscreen -and $element.TryGetCurrentPattern([System.Windows.Automation.ScrollItemPattern]::Pattern, [ref]$pattern)) {
    $pattern.ScrollIntoView()
    Start-Sleep -Milliseconds 600
    $element = Find-UrId (Get-UrMainWindow $processId) $automationId
  }
  return $element
}

# The Developer screen's Updates card and Settings' channel row, as they read
# now: the switch (shown, on, enabled), the channel line, the repair button,
# the check line, and Settings' note and button when that screen is shown.
function Get-A2cChannel([int]$processId) {
  $window = Get-UrMainWindow $processId
  if (-not $window) { throw "the app (pid $processId) has no window" }
  $facts = [pscustomobject]@{ Switch = $false; SwitchOn = $false; SwitchEnabled = $false; SwitchName = ''; Line = ''
    Repair = $false; RepairEnabled = $false; Check = ''; Row = $false; Note = ''; Official = $false; OfficialEnabled = $false }
  $switch = Find-UrId $window $A2cIdSwitch
  if ($switch) {
    $facts.Switch = $true
    $facts.SwitchName = [string]$switch.Current.Name
    $facts.SwitchEnabled = [bool]$switch.Current.IsEnabled
    $toggle = $null
    if ($switch.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$toggle)) {
      $facts.SwitchOn = $toggle.Current.ToggleState -eq [System.Windows.Automation.ToggleState]::On
    }
  }
  $line = Find-UrId $window $A2cIdLine
  if ($line) { $facts.Line = [string]$line.Current.Name }
  $repair = Find-UrId $window $A2cIdRepair
  if ($repair) { $facts.Repair = -not $repair.Current.IsOffscreen; $facts.RepairEnabled = [bool]$repair.Current.IsEnabled }
  $check = Find-UrId $window $UrIdCheckLine
  if ($check) { $facts.Check = [string]$check.Current.Name }
  $row = Find-UrId $window $A2cIdRow
  if ($row) { $facts.Row = $true; $facts.Note = [string]$row.Current.Name }
  $official = Find-UrId $window $A2cIdOfficial
  if ($official) { $facts.Official = -not $official.Current.IsOffscreen; $facts.OfficialEnabled = [bool]$official.Current.IsEnabled }
  return $facts
}

function Show-A2cChannel($facts, [string]$when) {
  Write-Host ("channel {0}: switch shown={1} on={2} enabled={3} '{4}'; line '{5}'; repair shown={6} enabled={7}; check line '{8}'; settings row={9} note '{10}', button shown={11} enabled={12}" -f $when,
    $facts.Switch, $facts.SwitchOn, $facts.SwitchEnabled, $facts.SwitchName, $facts.Line, $facts.Repair, $facts.RepairEnabled,
    $facts.Check, $facts.Row, $facts.Note, $facts.Official, $facts.OfficialEnabled)
}

# Polls the channel's surfaces until $until says yes; returns them, or the
# last reading with a FAIL said, never throwing: the leg's verdict counts it.
function Wait-A2cChannel([int]$processId, [scriptblock]$until, [string]$what, [int]$seconds = 60) {
  $deadline = (Get-Date).AddSeconds($seconds)
  $facts = $null
  do {
    try { $facts = Get-A2cChannel $processId } catch { $facts = $null }
    if ($facts -and (& $until $facts)) {
      Show-A2cChannel $facts "($what)"
      return $facts
    }
    Start-Sleep -Milliseconds 500
  } while ((Get-Date) -lt $deadline)
  if ($facts) { Show-A2cChannel $facts "at the end of the wait for $what" }
  return $facts
}

# Flips the Developer screen's switch through its Toggle pattern, as a click does.
function Invoke-A2cToggle([int]$processId) {
  $switch = Show-A2cId $processId $A2cIdSwitch
  if (-not $switch) { throw "the Developer screen has no $A2cIdSwitch" }
  if (-not $switch.Current.IsEnabled) { throw "$A2cIdSwitch is disabled" }
  $toggle = $switch.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
  Write-Host "toggle $A2cIdSwitch ('$($switch.Current.Name)', $($toggle.Current.ToggleState))"
  $toggle.Toggle()
}

# Presses a control after bringing it into view.
function Invoke-A2cId([int]$processId, [string]$automationId) {
  $element = Show-A2cId $processId $automationId
  if (-not $element) { throw "no $automationId" }
  if ($element.Current.IsOffscreen) { throw "$automationId is not on the screen" }
  if (-not $element.Current.IsEnabled) { throw "$automationId is disabled" }
  Write-Host "invoke $automationId ('$($element.Current.Name)')"
  $element.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}

# The tray app's check of a feed, from its log, from line $from on: which
# feed, its own code, the release it found newest, and the one it holds back.
function Wait-A2cTrayCheck([int]$from, [int]$seconds = 150) {
  $deadline = (Get-Date).AddSeconds($seconds)
  do {
    $done = Get-UrAppLogLines 'update: check of the \S+ feed complete' $from
    if ($done.Count -gt 0) {
      $line = $done[-1]
      Write-Host "app log: $line"
      $match = [regex]::Match($line, 'check of the (\S+) feed complete .*own code (\d+), newest release (\S+) \(code (\d+)\)(?:; (\S+) counts from (\S+))?')
      if (-not $match.Success) { throw "the app's check line does not read: $line" }
      (Get-UrAppLogLines 'update: release ' $from) | ForEach-Object { Write-Host "app log: $_" }
      return [pscustomobject]@{ Line = $line; Feed = $match.Groups[1].Value; OwnCode = $match.Groups[2].Value
        Newest = $match.Groups[3].Value; NewestCode = $match.Groups[4].Value; Waiting = $match.Groups[5].Value; WaitingFrom = $match.Groups[6].Value }
    }
    $failed = Get-UrAppLogLines 'update: release check failed|update: GitHub asked for no request|update: release list was not|update: check threw' $from
    if ($failed.Count -gt 0) {
      $failed | ForEach-Object { Write-Host "app log: $_" }
      return $null
    }
    Start-Sleep -Seconds 2
  } while ((Get-Date) -lt $deadline)
  (Get-UrAppLogLines 'update: ' $from) | ForEach-Object { Write-Host "app log: $_" }
  return $null
}

# The app started with --preview-ui=<destination>, its window grown to lay
# its column out whole.
function Start-A2cApp([string]$destination) {
  $app = Start-UrPreviewApp $destination
  Start-Sleep -Seconds 4
  Expand-UrWindow $app.Id 1500
  return $app
}

# Is EnableLUA on (UAC, and with it the integrity checks between windows)?
function Get-A2cUac {
  $value = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System' -Name EnableLUA -ErrorAction SilentlyContinue).EnableLUA
  $consent = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System' -Name ConsentPromptBehaviorAdmin -ErrorAction SilentlyContinue).ConsentPromptBehaviorAdmin
  Write-Host "UAC: EnableLUA=$value ConsentPromptBehaviorAdmin=$consent; this test runs as $(Get-WildTokenText $PID)"
  return [pscustomobject]@{ EnableLua = $value; Consent = $consent }
}
