# Helpers for .github/workflows/vpn-feed.yml, on top of vpn-apply-lib.ps1 (the
# elevated update's own runner helpers): the official feed as the app sees it,
# an independent reading of which release it offers, and a stand-in for GitHub
# that serves the feed's own list under another date.
# Dot-sourced by every test step:
#   . "$env:GITHUB_WORKSPACE\.github\vpn-feed-lib.ps1"
# Runs on a disposable GitHub runner only. It publishes nothing and creates no
# release or tag: the feed is read, without a token, as the app reads it.
# Pure ASCII, Windows PowerShell 5.1.
#
# SPDX-License-Identifier: MPL-2.0

. (Join-Path $PSScriptRoot 'vpn-apply-lib.ps1')

# The official feed (Common/ReleaseSelection.h kOfficialFeed) and the list both
# the tray app and the update helper ask for (ReleaseListUrl).
$UrOfficialRepoId = '936244679'
$UrOfficialOwner = 'urnetwork'
$UrOfficialRepo = 'build'
$UrFeedListPath = "/repositories/$UrOfficialRepoId/releases?per_page=30"
$UrFeedListUrl = "https://api.github.com$UrFeedListPath"
$UrFeedSoakSeconds = 24 * 60 * 60
$UrFeedDaySeconds = 24 * 60 * 60
# 2023-05-23T00:00:00Z: a release code is tenths of a second after it
$UrFoundedUnix = 1684800000

# Common/UpdateResult.h
$UrRefusalNotOffered = 0x20000007
$UrRefusalRateLimited = 0x2000000D

# Invoke-WebRequest's progress bar slows a 57 MB download to a crawl under
# Windows PowerShell 5.1.
$ProgressPreference = 'SilentlyContinue'

function ConvertTo-UrUnix($value) {
  if ($value -is [datetime]) {
    return [long]([DateTimeOffset]$value.ToUniversalTime()).ToUnixTimeSeconds()
  }
  $styles = [Globalization.DateTimeStyles]::AssumeUniversal -bor [Globalization.DateTimeStyles]::AdjustToUniversal
  return [long]([DateTimeOffset]::Parse([string]$value, [Globalization.CultureInfo]::InvariantCulture, $styles)).ToUnixTimeSeconds()
}

function Format-UrUnix([long]$unix) {
  return [DateTimeOffset]::FromUnixTimeSeconds($unix).UtcDateTime.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'")
}

# What GitHub allows this runner's address without a token, which is how the
# app and the helper ask. /rate_limit itself is not counted.
function Get-UrAnonymousLimit {
  $response = Invoke-WebRequest -UseBasicParsing -Uri 'https://api.github.com/rate_limit' `
    -UserAgent 'URnetwork-Windows/runner-test' -Headers @{ Accept = 'application/vnd.github+json' }
  $core = ($response.Content | ConvertFrom-Json).resources.core
  return [pscustomobject]@{ Limit = [int]$core.limit; Remaining = [int]$core.remaining; Reset = [long]$core.reset }
}

# Waits, up to $maxMinutes, until this address has at least $need anonymous
# requests left. Says what it found either way.
function Wait-UrAnonymousRequests([int]$need = 8, [int]$maxMinutes = 30) {
  $deadline = (Get-Date).AddMinutes($maxMinutes)
  while ($true) {
    $limit = Get-UrAnonymousLimit
    Write-Host "anonymous GitHub API requests left for this address: $($limit.Remaining) of $($limit.Limit), reset $(Format-UrUnix $limit.Reset)"
    if ($limit.Remaining -ge $need) { return $limit }
    $wait = $limit.Reset - [DateTimeOffset]::UtcNow.ToUnixTimeSeconds() + 5
    if ($wait -lt 5) { $wait = 5 }
    if ((Get-Date).AddSeconds($wait) -gt $deadline) {
      throw "this runner's address has $($limit.Remaining) anonymous GitHub API requests left, and the limit resets after this test's patience ($maxMinutes min)"
    }
    Write-Host "waiting $wait s for the limit to reset"
    Start-Sleep -Seconds $wait
  }
}

# The official feed's release list as the app asks for it: no token, the
# repository by its id, no redirect followed. Saves the body's bytes as
# $saveAs. Returns the parsed list, the response's Date header in Unix
# seconds, and the file.
function Get-UrFeedList([string]$saveAs) {
  $response = Invoke-WebRequest -UseBasicParsing -Uri $UrFeedListUrl -MaximumRedirection 0 `
    -UserAgent 'URnetwork-Windows/runner-test' -Headers @{ Accept = 'application/vnd.github+json' }
  if ($response.StatusCode -ne 200) { throw "the release list answered $($response.StatusCode)" }
  $date = [string]$response.Headers['Date']
  if (-not $date) { throw "the release list has no Date header" }
  $stream = $response.RawContentStream
  $stream.Position = 0
  $bytes = New-Object byte[] $stream.Length
  [void]$stream.Read($bytes, 0, $bytes.Length)
  [IO.File]::WriteAllBytes($saveAs, $bytes)
  $releases = @([Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json)
  # a JSON array of one comes back as one object from the pipeline
  if ($releases.Count -eq 1 -and $releases[0] -is [array]) { $releases = @($releases[0]) }
  $server = ConvertTo-UrUnix $date
  Write-Host "release list: $($releases.Count) releases, $($bytes.Length) bytes, Date: $date ($(Format-UrUnix $server))"
  return [pscustomobject]@{ Releases = $releases; ServerUnix = $server; Date = $date; File = $saveAs; Bytes = $bytes.Length }
}

# This test's own reading of the feed, written from the rules and not from the
# app's code: the newest release that the list marks, with the boolean false,
# as no draft and no prerelease, that is immutable, tagged v<YYYY.M.D>-<code>,
# carries URnetwork-<version>-<arch>.msi with a sha256 digest at the feed's own
# download URL, has a code no more than 48 h ahead of the list's date, was not
# published more than 48 h before its code's instant, and had been out,
# unchanged, for 24 h when the UTC day of the list's date began: counted from
# the latest of its published_at, its updated_at and that MSI's updated_at. And
# the newest one that only that last rule holds back, with the first second it
# counts from.
function Get-UrFeedReading($releases, [long]$serverUnix, [string]$arch = 'x64') {
  $dayStart = $serverUnix - ($serverUnix % $UrFeedDaySeconds)
  $rows = New-Object System.Collections.Generic.List[object]
  $waits = 'out, unchanged, for less than 24 h when the day began'
  foreach ($release in $releases) {
    $tag = [string]$release.tag_name
    $row = [pscustomobject]@{ Tag = $tag; Why = ''; Code = [uint64]0; Version = ''; PublishedAt = [string]$release.published_at
      Published = [long]0; Since = [long]0; CountsFrom = [long]0; Asset = ''; Url = ''; Digest = ''; Size = [long]0; Soaked = $false }
    $match = [regex]::Match($tag, '^v(\d{4})\.(\d{1,2})\.(\d{1,2})-(\d{1,18})$')
    if ($release.draft -isnot [bool] -or $release.draft) { $row.Why = 'a draft' }
    elseif ($release.prerelease -isnot [bool] -or $release.prerelease) { $row.Why = 'a prerelease' }
    elseif (-not $match.Success) { $row.Why = 'a tag outside the grammar' }
    elseif ($release.immutable -ne $true) { $row.Why = 'not immutable' }
    else {
      $row.Version = $tag.Substring(1)
      $row.Code = [uint64]$match.Groups[4].Value
      $codeUnix = $UrFoundedUnix + [long][math]::Floor([decimal]$row.Code / 10)
      $name = "URnetwork-$($row.Version)-$arch.msi"
      $assets = @($release.assets | Where-Object { $_.name -eq $name })
      $wantUrl = "https://github.com/$UrOfficialOwner/$UrOfficialRepo/releases/download/$tag/$name"
      if ($codeUnix -gt $serverUnix + 48 * 3600) { $row.Why = 'a code more than 48 h ahead of the list' }
      elseif ($assets.Count -ne 1) { $row.Why = "without $name" }
      elseif ([string]$assets[0].digest -notmatch '^sha256:[0-9a-fA-F]{64}$') { $row.Why = 'without a sha256 digest' }
      elseif ([string]$assets[0].browser_download_url -ne $wantUrl) { $row.Why = "at $($assets[0].browser_download_url), not the feed's URL" }
      elseif (-not $release.published_at) { $row.Why = 'without published_at' }
      elseif (-not $release.updated_at) { $row.Why = 'without updated_at' }
      elseif (-not $assets[0].updated_at) { $row.Why = "without its package's updated_at" }
      elseif ((ConvertTo-UrUnix $release.published_at) -lt $codeUnix - 48 * 3600) { $row.Why = 'published more than 48 h before its code' }
      else {
        $row.Asset = $name
        $row.Url = $wantUrl
        $row.Digest = ([string]$assets[0].digest).Substring(7).ToLowerInvariant()
        $row.Size = [long]$assets[0].size
        $row.Published = ConvertTo-UrUnix $release.published_at
        $row.Since = [math]::Max($row.Published, [math]::Max((ConvertTo-UrUnix $release.updated_at), (ConvertTo-UrUnix $assets[0].updated_at)))
        $ripe = $row.Since + $UrFeedSoakSeconds
        if ($ripe % $UrFeedDaySeconds -eq 0) { $row.CountsFrom = $ripe } else { $row.CountsFrom = $ripe - ($ripe % $UrFeedDaySeconds) + $UrFeedDaySeconds }
        $row.Soaked = $ripe -le $dayStart
        if (-not $row.Soaked) { $row.Why = $waits }
      }
    }
    $rows.Add($row)
  }
  $offered = @($rows | Where-Object { $_.Soaked } | Sort-Object Code -Descending | Select-Object -First 1)
  $waiting = @($rows | Where-Object { $_.Why -eq $waits } |
    Sort-Object Code -Descending | Select-Object -First 1)
  foreach ($row in $rows) {
    $state = 'skipped: ' + $row.Why
    $changed = ''
    if ($row.Since -ne $row.Published) { $changed = ", last changed $(Format-UrUnix $row.Since)" }
    if ($row.Soaked) { $state = "counts (published $($row.PublishedAt)$changed, since $(Format-UrUnix $row.CountsFrom))" }
    elseif ($row.CountsFrom -ne 0) { $state = "waits (published $($row.PublishedAt)$changed, counts from $(Format-UrUnix $row.CountsFrom))" }
    Write-Host ("feed: {0,-28} {1}" -f $row.Tag, $state)
  }
  $result = [pscustomobject]@{ Rows = $rows.ToArray(); DayStart = $dayStart; ServerUnix = $serverUnix; Offered = $null; Waiting = $null }
  if ($offered.Count -eq 1) { $result.Offered = $offered[0] }
  if ($waiting.Count -eq 1) { $result.Waiting = $waiting[0] }
  Write-Host "feed, on the day that began $(Format-UrUnix $dayStart): offers $(if ($result.Offered) { $result.Offered.Tag } else { 'nothing' }); holds back $(if ($result.Waiting) { "$($result.Waiting.Tag) until $(Format-UrUnix $result.Waiting.CountsFrom)" } else { 'nothing' })"
  return $result
}

# The feed, read now, well clear of midnight UTC: a list read a moment before
# the day changes and a helper that asks a moment after would be judged on
# different days, and this test would blame the product for it.
function Get-UrFeedNow([string]$saveAs) {
  while ($true) {
    $list = Get-UrFeedList $saveAs
    $toMidnight = $UrFeedDaySeconds - ($list.ServerUnix % $UrFeedDaySeconds)
    if ($toMidnight -gt 1500) { return $list }
    Write-Host "GitHub's day ends in $toMidnight s: waiting for the next one before reading the feed"
    Start-Sleep -Seconds ($toMidnight + 20)
  }
}

# What a release's tag gives app\tools\UrVersion.ps1: the ProductVersion and
# the FILEVERSION its package must carry.
function Get-UrExpectOfRelease($row, [string]$key) {
  $ur = & (Join-Path $env:GITHUB_WORKSPACE 'app\tools\UrVersion.ps1') -Version $row.Version
  if ([string]$ur.UrVersionCode -ne [string]$row.Code) { throw "UrVersion.ps1 read code $($ur.UrVersionCode) from $($row.Version)" }
  return [pscustomobject]@{ key = $key; version = $row.Version; code = [string]$row.Code; msi_version = [string]$ur.UrMsiVersion
    file_version = ('{0}.{1}.{2}.{3}' -f $ur.UrVersionMajor, $ur.UrVersionMinor, $ur.UrVersionPatch, $ur.UrVersionBuild) }
}

# The first column of a package's query, or nothing when the package has no
# such table. Invoke-UrMsiQuery returns its rows as one array, so they are
# walked with foreach, never piped.
function Get-UrMsiColumn([string]$msi, [string]$table, [string]$sql) {
  $values = New-Object System.Collections.Generic.List[string]
  $present = $false
  $tables = Invoke-UrMsiQuery $msi 'SELECT `Name` FROM `_Tables`' 1
  foreach ($row in $tables) { if ($row[0] -eq $table) { $present = $true } }
  if ($present) {
    $rows = Invoke-UrMsiQuery $msi $sql 1
    foreach ($row in $rows) { $values.Add([string]$row[0]) }
  }
  return ,$values.ToArray()
}

# What a package does about the running app and the relaunch, read from its
# tables: whether it carries the update helper, the relaunch after an update,
# and the close before one.
function Get-UrPackageTraits([string]$msi) {
  $files = New-Object System.Collections.Generic.List[string]
  foreach ($name in (Get-UrMsiColumn $msi 'File' 'SELECT `FileName` FROM `File`')) { $files.Add((Get-UrLongName $name)) }
  $actions = Get-UrMsiColumn $msi 'CustomAction' 'SELECT `Action` FROM `CustomAction`'
  $sequence = Get-UrMsiColumn $msi 'InstallExecuteSequence' 'SELECT `Action` FROM `InstallExecuteSequence`'
  $upgradeCode = Get-UrMsiColumn $msi 'Property' "SELECT ``Value`` FROM ``Property`` WHERE ``Property``='UpgradeCode'"
  $traits = [pscustomobject]@{
    Files          = $files.Count
    Helper         = ($files -contains 'URnetworkUpdate.exe')
    Relaunch       = (($actions -contains 'RelaunchAfterUpdate') -and ($sequence -contains 'RelaunchAfterUpdate'))
    CloseApp       = (@($sequence | Where-Object { $_ -like 'Wix4CloseApplications_*' }).Count -gt 0)
    ProductVersion = (Get-UrMsiProductVersion $msi)
    UpgradeCode    = $(if ($upgradeCode.Count -eq 1) { $upgradeCode[0] } else { '' })
  }
  Write-Host "package $(Split-Path -Leaf $msi): ProductVersion $($traits.ProductVersion), UpgradeCode $($traits.UpgradeCode), $($traits.Files) files, update helper $($traits.Helper), RelaunchAfterUpdate $($traits.Relaunch), CloseApplication $($traits.CloseApp)"
  return $traits
}

# The lines of an update-helper.log.
function Get-UrHelperLog([string]$tag) {
  $path = Join-Path (Join-Path $UrUpdatesDir $tag) 'update-helper.log'
  if (-not (Test-Path -LiteralPath $path)) { return ,@() }
  return ,@(Get-Content -LiteralPath $path)
}

function Assert-UrHelperLogHas([string[]]$lines, [string]$pattern, [string]$what) {
  $found = @($lines | Where-Object { $_ -match $pattern })
  if ($found.Count -eq 0) { throw "the helper's log does not say $what (no line matches $pattern)" }
  Write-Host "OK: the helper's log says $what`: $($found[0])"
}

# Runs the helper for $tag and, when GitHub refuses this runner's address the
# release list (Refusal::RateLimited), waits for the limit to reset and runs
# it once more. Anything else is the helper's answer.
function Invoke-UrHelperPatiently([string]$tag, [int]$timeoutSeconds = 900, $app = $null) {
  $run = Invoke-UrHelper $tag $timeoutSeconds $app
  if ($run.ExitCode -ne $UrRefusalRateLimited) { return $run }
  Write-Host "GitHub refused this runner's address the release list; the helper said so (Refusal::RateLimited)"
  [void](Wait-UrAnonymousRequests 4 40)
  return Invoke-UrHelper $tag $timeoutSeconds $app
}

# The tray app's own check, from its log, from line $from on: the release it
# found newest and the one it says is held back. Waits for the line; $null
# when the check failed or never ran, with what the log says instead.
function Wait-UrTrayCheck([int]$from, [int]$seconds = 150) {
  $deadline = (Get-Date).AddSeconds($seconds)
  do {
    $done = Get-UrAppLogLines 'update: check complete' $from
    if ($done.Count -gt 0) {
      $line = $done[-1]
      Write-Host "app log: $line"
      $match = [regex]::Match($line, 'own code (\d+), newest release (\S+) \(code (\d+)\)(?:; (\S+) counts from (\S+))?')
      if (-not $match.Success) { throw "the app's check line does not read: $line" }
      (Get-UrAppLogLines 'update: release ' $from) | ForEach-Object { Write-Host "app log: $_" }
      return [pscustomobject]@{ Line = $line; OwnCode = $match.Groups[1].Value; Newest = $match.Groups[2].Value
        NewestCode = $match.Groups[3].Value; Waiting = $match.Groups[4].Value; WaitingFrom = $match.Groups[5].Value }
    }
    $failed = Get-UrAppLogLines 'update: release check failed|update: GitHub asked for no request|update: the release list had no|update: release list was not|update: check threw' $from
    if ($failed.Count -gt 0) {
      $failed | ForEach-Object { Write-Host "app log: $_" }
      return $null
    }
    Start-Sleep -Seconds 2
  } while ((Get-Date) -lt $deadline)
  (Get-UrAppLogLines 'update: ' $from) | ForEach-Object { Write-Host "app log: $_" }
  throw "the app logged no update check within $seconds s of its start"
}

# ---- a stand-in for GitHub that serves the feed's own list on another day ------

# Answers for GitHub on this runner (vpn-feed-standin.ps1): the official feed's
# release list is the file $listFile, GitHub's own bytes, and every response's
# Date header is $serverUnix. $row's MSI, $msi, is served at the download URL
# the feed names, redirected once to the release-asset host. Everything the
# helper checks about the connection passes; what differs from GitHub is the
# day. With -Running the date is a clock: $serverUnix now, and a second later
# every second from now, so the stand-in's day can change while the app runs
# (Get-UrStandInClock reads it). For as long as the file $state.AltList
# exists, the stand-in serves that file as the list instead
# (Set-UrWithdrawnList writes it, Remove-UrWithdrawnList removes it). Returns
# what Remove-UrTestNetwork undoes.
function Set-UrFeedStandIn([string]$listFile, [long]$serverUnix, $row, [string]$msi, [string]$name = 'feed-standin', [switch]$Running) {
  $state = [pscustomobject]@{ Server = $null; Log = ''; Hosts = $null; Tls = $null; Names = $UrGitHubHosts
    ClockBase = $serverUnix; ClockStartTicks = [long]0; List = $listFile; AltList = '' }
  try {
    $work = New-Item -ItemType Directory -Force (Join-Path $env:RUNNER_TEMP $name)
    $state.AltList = Join-Path $work 'alternate-list.json'
    if (Test-Path -LiteralPath $state.AltList) { Remove-Item -LiteralPath $state.AltList -Force }
    $date = [DateTimeOffset]::FromUnixTimeSeconds($serverUnix).UtcDateTime.ToString('r', [Globalization.CultureInfo]::InvariantCulture)
    $download = "/$UrOfficialOwner/$UrOfficialRepo/releases/download/$($row.Tag)/$($row.Asset)"
    $state.Tls = New-UrTestTls $UrGitHubHosts
    $state.Hosts = Add-UrHostsEntries $UrGitHubHosts
    $state.Log = Join-Path $work 'server.log'
    # the clock starts here, once the certificates and the hosts file are done
    if ($Running) { $state.ClockStartTicks = [DateTime]::UtcNow.Ticks }
    Write-Host "stand-in: $UrFeedListPath is $listFile with Date: $date$(if ($Running) { ', running from now' }); $download is $msi"
    $state.Server = Start-UrTestServer 'vpn-feed-standin.ps1' @('-Thumbprint', $state.Tls.Leaf, '-ListJson', $listFile,
      '-ListPath', $UrFeedListPath, '-DateHeader', "`"$date`"", '-DownloadPath', $download, '-Body', $msi, '-Log', $state.Log,
      '-ClockStartTicks', [string]$state.ClockStartTicks, '-AltList', $state.AltList) $state.Log
    return $state
  } catch {
    Remove-UrTestNetwork $state
    throw
  }
}

# Makes the stand-in serve the feed's own list with the release $tag changed
# at the stand-in's date now: its updated_at is that second, and with
# $prerelease it is marked a prerelease, which is how GitHub lets a published
# release be withdrawn without losing its tag. Without $prerelease it is the
# list as it would read the second that mark is cleared again, or the
# release's notes are edited. The list is GitHub's bytes with those values
# changed in that one release and nothing else. Returns this test's own
# reading of the list it now serves.
function Set-UrChangedList($state, [string]$tag, [bool]$prerelease) {
  $text = [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($state.List))
  $name = '"tag_name":"' + $tag + '"'
  $at = $text.IndexOf($name, [StringComparison]::Ordinal)
  if ($at -lt 0 -or $text.IndexOf($name, $at + 1, [StringComparison]::Ordinal) -ge 0) { throw "the list does not name $tag exactly once" }
  # a release's own marks and times follow its tag and come before its assets
  $assets = $text.IndexOf('"assets":[', $at, [StringComparison]::Ordinal)
  if ($assets -lt 0) { throw "the list gives $tag no assets" }
  $head = $text.Substring($at, $assets - $at)
  $now = Get-UrStandInClock $state
  $mark = [regex]'"prerelease":false'
  $stamp = [regex]'"updated_at":"[^"]*"'
  if ($mark.Matches($head).Count -ne 1 -or $stamp.Matches($head).Count -ne 1) { throw "the list does not give $tag one prerelease mark and one updated_at" }
  $changed = $stamp.Replace($head, ('"updated_at":"' + (Format-UrUnix $now) + '"'))
  if ($prerelease) { $changed = $mark.Replace($changed, '"prerelease":true') }
  $bytes = [Text.Encoding]::UTF8.GetBytes($text.Substring(0, $at) + $changed + $text.Substring($assets))
  # written whole under another name, then renamed: the stand-in reads the
  # file the moment it exists
  $written = "$($state.AltList).new"
  [IO.File]::WriteAllBytes($written, $bytes)
  Move-Item -LiteralPath $written -Destination $state.AltList -Force
  $releases = @([Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json)
  if ($releases.Count -eq 1 -and $releases[0] -is [array]) { $releases = @($releases[0]) }
  $marked = @($releases | Where-Object { $_.tag_name -eq $tag })
  if ($marked.Count -ne 1 -or $marked[0].prerelease -ne $prerelease -or (ConvertTo-UrUnix $marked[0].updated_at) -ne $now) {
    throw "the alternate list does not give $tag prerelease $prerelease and updated_at $(Format-UrUnix $now)"
  }
  Write-Host "stand-in: from now the list gives $tag prerelease $prerelease and updated_at $(Format-UrUnix $now) ($($bytes.Length) bytes)"
  return Get-UrFeedReading $releases $now
}

# Ends Set-UrChangedList: the stand-in serves GitHub's own bytes again.
function Remove-UrChangedList($state) {
  if (Test-Path -LiteralPath $state.AltList) { Remove-Item -LiteralPath $state.AltList -Force }
  Write-Host "stand-in: the list is GitHub's own bytes again"
}

# The stand-in's date now, in Unix seconds: what its next response carries.
function Get-UrStandInClock($state) {
  if ($state.ClockStartTicks -eq 0) { return [long]$state.ClockBase }
  return [long]$state.ClockBase + [long][math]::Floor(([DateTime]::UtcNow.Ticks - $state.ClockStartTicks) / 10000000)
}

# Downloads $row's MSI from GitHub as a browser would, and requires GitHub's
# SHA-256 for it. Says where the one redirect went.
function Save-UrReleaseMsi($row, [string]$saveAs) {
  $request = [Net.HttpWebRequest]::Create($row.Url)
  $request.AllowAutoRedirect = $false
  $request.UserAgent = 'URnetwork-Windows/runner-test'
  $response = $request.GetResponse()
  $status = [int]$response.StatusCode
  $location = [string]$response.Headers['Location']
  $response.Close()
  $hostName = ''
  if ($location) { $hostName = ([Uri]$location).Host }
  Write-Host "download: $($row.Url) answered $status, Location host '$hostName'"
  if ($status -ne 302 -or -not ($UrAssetHosts -contains $hostName)) { throw "the download is not one redirect to GitHub's release assets" }
  Invoke-WebRequest -UseBasicParsing -Uri $location -OutFile $saveAs -UserAgent 'URnetwork-Windows/runner-test'
  $hash = (Get-FileHash -Algorithm SHA256 $saveAs).Hash.ToLowerInvariant()
  if ($hash -ne $row.Digest) { throw "$saveAs is SHA-256 $hash, and GitHub lists $($row.Digest)" }
  Write-Host "OK: $($row.Asset) from $hostName is the $((Get-Item $saveAs).Length) bytes GitHub lists (SHA-256 $hash)"
  return $hostName
}
