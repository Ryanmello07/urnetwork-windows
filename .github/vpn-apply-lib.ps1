# Helpers for the A2s runner tests, on top of vpn-upgrade-lib.ps1:
#   .github/workflows/vpn-apply.yml        the update helper against a
#                                          throwaway prerelease on this fork
#   .github/workflows/vpn-apply-local.yml  the official binaries against a
#                                          local stand-in for GitHub, and the
#                                          installs by hand
# Dot-sourced by every test step:
#   . "$env:GITHUB_WORKSPACE\.github\vpn-apply-lib.ps1"
# Runs on a disposable GitHub runner only: it installs and updates URnetwork
# machine-wide, edits the hosts file, trusts a test CA machine-wide, sets an
# Image File Execution Options value and a Windows Installer policy, and undoes
# each of those before the step that set it ends.
# Pure ASCII, Windows PowerShell 5.1.
#
# SPDX-License-Identifier: MPL-2.0

. (Join-Path $PSScriptRoot 'vpn-upgrade-lib.ps1')

$UrHelperPath = Join-Path $UrInstallDir 'URnetworkUpdate.exe'
$UrUpdatesDir = Join-Path $UrInstallDir 'updates'
$UrResultPath = Join-Path $UrUpdatesDir 'last-result.json'
# The per-user app root with no URNETWORK_APP_ROOT, which is what the
# installer's relaunch gets: it starts through explorer, with the user's own
# environment, and no test sets an override there.
$UrAppRoot = Join-Path $env:LOCALAPPDATA 'URnetwork\app'
$UrAppLog = Join-Path $UrAppRoot 'logs\urnetwork-app.log'
$UrMarkerPath = Join-Path $UrAppRoot 'update_in_progress'

# Common/UpdateResult.h
$UrRefusalDigest = 0x20000009

# The official feed (Common/ReleaseSelection.h kOfficialFeed), which
# vpn-apply-local.yml's stand-in for GitHub answers for.
$UrOfficialRepoId = '1297133846'
$UrOfficialOwner = 'urnetwork'
$UrOfficialRepo = 'windows'

# The hosts a release download may be redirected to (ReleaseSelection.h
# kAssetRedirectHosts), and every host the helper asks.
$UrAssetHosts = @('release-assets.githubusercontent.com', 'objects.githubusercontent.com')
$UrGitHubHosts = @('api.github.com', 'github.com') + $UrAssetHosts
$UrHostsFile = Join-Path $env:SystemRoot 'System32\drivers\etc\hosts'
$UrHostsTag = '# a2s-runner-test'

# What the helper's VERSIONINFO says (Updater.rc): a build with a runner
# test's feed compiled in says so, and app\build.ps1 refuses to package it.
$UrHelperDescription = 'URnetwork update'
$UrRunnerHelperDescription = 'URnetwork update (runner test feed)'

# Whether a URnetworkUpdate.exe was built with the runner feed, by its
# FileDescription; anything else fails.
function Get-UrHelperFeed([string]$path) {
  $description = [Diagnostics.FileVersionInfo]::GetVersionInfo($path).FileDescription
  if ($description -eq $UrRunnerHelperDescription) { return 'runner' }
  if ($description -eq $UrHelperDescription) { return 'official' }
  throw "$path has FileDescription '$description', neither the official helper's nor the runner feed's"
}

# The helper's report, or $null when there is none.
function Read-UrResult {
  if (-not (Test-Path -LiteralPath $UrResultPath)) { return $null }
  $text = Get-Content -Raw -LiteralPath $UrResultPath
  Write-Host "last-result.json: $text"
  return $text | ConvertFrom-Json
}

function Assert-UrResult($result, [string]$tag, [string]$code, [long]$exitCode) {
  if ($null -eq $result) { throw "no last-result.json at $UrResultPath" }
  if ($result.tag -ne $tag) { throw "last-result.json tag $($result.tag), want $tag" }
  if ([string]$result.code -ne $code) { throw "last-result.json code $($result.code), want $code" }
  if ([long]$result.exitCode -ne $exitCode) { throw "last-result.json exitCode $($result.exitCode), want $exitCode" }
  if ($result.finishedUtc -notmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$') {
    throw "last-result.json finishedUtc '$($result.finishedUtc)' is not a UTC second"
  }
  Write-Host "OK: last-result.json reports $tag, code $code, exit $exitCode at $($result.finishedUtc)"
}

# Users may read what the helper writes under updates\, and only SYSTEM and
# Administrators may change it: owned by Administrators, no inherited ACE,
# and no write, delete or ownership right for anyone else.
function Assert-UrAdminOnly([string]$path) {
  $acl = Get-Acl -LiteralPath $path
  if ($acl.Owner -ne 'BUILTIN\Administrators') { throw "$path is owned by $($acl.Owner), not BUILTIN\Administrators" }
  if (-not $acl.AreAccessRulesProtected) { throw "$path inherits its ACL from its folder" }
  $writes = [Security.AccessControl.FileSystemRights]'WriteData, AppendData, WriteExtendedAttributes, DeleteSubdirectoriesAndFiles, WriteAttributes, Delete, ChangePermissions, TakeOwnership'
  foreach ($rule in $acl.Access) {
    $who = $rule.IdentityReference.Value
    if ($who -in @('NT AUTHORITY\SYSTEM', 'BUILTIN\Administrators')) { continue }
    if ($rule.AccessControlType -eq 'Allow' -and ($rule.FileSystemRights -band $writes)) {
      throw "$path grants $who $($rule.FileSystemRights)"
    }
  }
  Write-Host "OK: $path is admin-only ($(($acl.Access | ForEach-Object { "$($_.IdentityReference) $($_.FileSystemRights)" }) -join '; '))"
}

# The update marker the tray app writes when it begins to exit while the
# helper it started still runs (UpdateChecker.cpp RunApply, then
# SingleInstance.cpp RecordUpdateInProgress): "<pid> <creation FILETIME>
# <written at, Unix seconds>\n", written beside the file and renamed in.
function Write-UrUpdateMarker([Diagnostics.Process]$helper) {
  New-Item -ItemType Directory -Force $UrAppRoot | Out-Null
  $created = $helper.StartTime.ToFileTimeUtc()
  $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
  $written = "$UrMarkerPath.new"
  [IO.File]::WriteAllText($written, "$($helper.Id) $created $now`n", [Text.Encoding]::ASCII)
  Move-Item -Force $written $UrMarkerPath
  Write-Host "update marker: $([IO.File]::ReadAllText($UrMarkerPath).Trim())"
}

# Runs the installed helper as the tray app does after its elevation prompt:
# URnetworkUpdate.exe --apply-update <tag>, and waits for it. With $app (the
# tray app this test started), it writes the update marker the moment that app
# exits while the helper runs, as the app itself does when the installer
# closes it. Returns the process (exited), how long it ran, and when the
# marker was written.
function Invoke-UrHelper([string]$tag, [int]$timeoutSeconds = 900, $app = $null) {
  Write-Host "$UrHelperPath --apply-update $tag"
  $started = Get-Date
  $helper = Start-Process -FilePath $UrHelperPath -ArgumentList @('--apply-update', $tag) -PassThru
  $null = $helper.Handle
  $marked = $null
  $deadline = $started.AddSeconds($timeoutSeconds)
  while (-not $helper.WaitForExit(100)) {
    if ($null -ne $app -and $null -eq $marked -and $app.HasExited) {
      Write-UrUpdateMarker $helper
      $marked = Get-Date
      Write-Host "the app exited while the helper ran; the marker names the helper"
    }
    if ((Get-Date) -gt $deadline) {
      Stop-Process -Id $helper.Id -Force -ErrorAction SilentlyContinue
      throw "the helper (pid $($helper.Id)) was still running after $timeoutSeconds s"
    }
  }
  $helper.WaitForExit()
  $seconds = [int]((Get-Date) - $started).TotalSeconds
  Write-Host ("helper pid {0} exit code {1} (0x{1:X8}) after {2} s" -f $helper.Id, $helper.ExitCode, $seconds)
  return [pscustomobject]@{ Process = $helper; ExitCode = [long]$helper.ExitCode; Seconds = $seconds; Marked = $marked }
}

# The helper's own log, msiexec's log and everything else under updates\.
function Show-UrUpdates {
  if (-not (Test-Path $UrUpdatesDir)) { Write-Host "no $UrUpdatesDir"; return }
  Get-ChildItem -Recurse -Force $UrUpdatesDir | ForEach-Object {
    Write-Host ("updates: {0} {1}" -f $_.FullName.Substring($UrUpdatesDir.Length), $(if ($_.PSIsContainer) { '<dir>' } else { $_.Length }))
  }
  Get-ChildItem -Recurse -Force $UrUpdatesDir -Filter 'update-helper.log' | ForEach-Object {
    Get-Content -LiteralPath $_.FullName | ForEach-Object { Write-Host "helper log: $_" }
  }
}

# URnetwork.exe processes and their command lines.
function Get-UrAppProcesses {
  return ,@(Get-CimInstance Win32_Process -Filter "Name='URnetwork.exe'" |
    ForEach-Object { [pscustomobject]@{ Id = [int]$_.ProcessId; CommandLine = [string]$_.CommandLine; Created = $_.CreationDate } })
}

function Stop-UrAppProcesses {
  foreach ($process in (Get-UrAppProcesses)) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
}

# Starts the installed tray app as a user would and waits for its tray window.
function Start-UrTrayApp([int]$seconds = 90) {
  $app = Start-Process -FilePath (Join-Path $UrInstallDir 'URnetwork.exe') -PassThru
  $null = $app.Handle
  $deadline = (Get-Date).AddSeconds($seconds)
  $tray = $null
  while (-not $tray -and (Get-Date) -lt $deadline) {
    if ($app.HasExited) { throw "the app exited by itself (code $($app.ExitCode))" }
    $tray = (Get-UrTopLevelWindows $app.Id) | Where-Object { $_.Class -eq 'URnetworkTrayWindow' } | Select-Object -First 1
    if (-not $tray) { Start-Sleep -Milliseconds 500 }
  }
  if (-not $tray) { throw "no URnetworkTrayWindow for pid $($app.Id) after $seconds s" }
  Write-Host "app running as pid $($app.Id), tray window $($tray.Handle)"
  return $app
}

# The app's log lines (with their timestamps) that match $pattern, from line
# $from on.
function Get-UrAppLogLines([string]$pattern, [int]$from = 0) {
  if (-not (Test-Path -LiteralPath $UrAppLog)) { return ,@() }
  $lines = @(Get-Content -LiteralPath $UrAppLog)
  if ($from -ge $lines.Count) { return ,@() }
  return ,@($lines[$from..($lines.Count - 1)] | Where-Object { $_ -match $pattern })
}

function Get-UrAppLogLength {
  if (-not (Test-Path -LiteralPath $UrAppLog)) { return 0 }
  return @(Get-Content -LiteralPath $UrAppLog).Count
}

# An app log line's time, as the time of day: "2026-10-06 10:28:54.0339455 [app] ...".
function Get-UrAppLogTime([string]$line) {
  $match = [regex]::Match($line, '^\d{4}-\d{2}-\d{2} (\d{2}:\d{2}:\d{2}\.\d+) ')
  if (-not $match.Success) { throw "not an app log line: $line" }
  return [TimeSpan]::Parse($match.Groups[1].Value)
}

# An msiexec log line's time of day: "MSI (s) (94:C8) [10:28:53:989]: ...".
function Get-UrMsiLogTime([string]$line) {
  $match = [regex]::Match($line, '\[(\d{2}):(\d{2}):(\d{2}):(\d{3})\]')
  if (-not $match.Success) { throw "not a timed msiexec log line: $line" }
  return New-Object TimeSpan 0, ([int]$match.Groups[1].Value), ([int]$match.Groups[2].Value), ([int]$match.Groups[3].Value), ([int]$match.Groups[4].Value)
}

function Get-UrMsiLogText([string]$path) {
  $bytes = [IO.File]::ReadAllBytes($path)
  if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) {
    return [Text.Encoding]::Unicode.GetString($bytes, 2, $bytes.Length - 2)
  }
  return [Text.Encoding]::Default.GetString($bytes)
}

function Get-UrMsiLogLines([string]$path, [string]$pattern) {
  return ,@((Get-UrMsiLogText $path) -split "`r?`n" | Where-Object { $_ -match $pattern })
}

# The first install's lines of an msiexec log: up to the nested removal of the
# old product (RunEngine for the UPGRADINGPRODUCTCODE run) when there is one.
function Get-UrMsiOuterLines([string]$path) {
  $lines = @((Get-UrMsiLogText $path) -split "`r?`n")
  $nested = -1
  for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'Doing action: RemoveExistingProducts') { $nested = $i; break }
  }
  if ($nested -lt 0) { return ,$lines }
  return ,@($lines[0..$nested])
}

# Requirement b's and e's evidence in one install log: the time window of the
# first Wix4CloseApplications_X64 (from "Doing action" to the next action), and
# whether the first InstallValidate's Restart Manager shut anything down.
function Get-UrCloseEvidence([string]$path) {
  $lines = Get-UrMsiOuterLines $path
  $start = $null
  $end = $null
  $validateStart = -1
  $validateEnd = -1
  for ($i = 0; $i -lt $lines.Count; $i++) {
    $line = $lines[$i]
    if ($null -eq $start -and $line -match 'Doing action: Wix4CloseApplications_X64') { $start = Get-UrMsiLogTime $line; continue }
    if ($null -ne $start -and $null -eq $end -and $line -match '\]: Doing action: ') { $end = Get-UrMsiLogTime $line }
    if ($validateStart -lt 0 -and $line -match 'Doing action: InstallValidate') { $validateStart = $i }
    if ($validateStart -ge 0 -and $validateEnd -lt 0 -and $line -match 'Action ended \d{1,2}:\d{2}:\d{2}: InstallValidate\.') { $validateEnd = $i }
  }
  $shutDown = @()
  if ($validateStart -ge 0 -and $validateEnd -gt $validateStart) {
    $shutDown = @($lines[$validateStart..$validateEnd] | Where-Object { $_ -match 'RESTART MANAGER: Successfully shut down' })
  }
  $rm = @($lines | Where-Object { $_ -match 'RESTART MANAGER' })
  return [pscustomobject]@{ CloseStart = $start; CloseEnd = $end; ShutDownInValidate = $shutDown; RestartManagerLines = $rm }
}

# Waits for the installer's relaunch after an update: a URnetwork.exe
# --after-update, its "waiting" line, the line that says the update was still
# running when it asked, the marker's removal once the helper ended, and the
# tray icon. Lines are read from line $from of the app log on. Returns what
# it saw; the caller asserts.
function Wait-UrRelaunch([int]$from, [int]$seconds = 120) {
  $deadline = (Get-Date).AddSeconds($seconds)
  $seen = [pscustomobject]@{ Processes = @(); Waiting = @(); Running = @(); Ended = @(); Started = @(); Report = @() }
  do {
    $seen.Processes = @((Get-UrAppProcesses) | Where-Object { $_.CommandLine -like '*--after-update*' })
    $seen.Waiting = Get-UrAppLogLines 'update: relaunched after an update; waiting for it to end' $from
    if ($seen.Waiting.Count -gt 0) {
      $seen.Running = Get-UrAppLogLines "update: the updater's installer is running" $from
      $seen.Ended = Get-UrAppLogLines 'update: removed a stale update marker' $from
      $seen.Started = Get-UrAppLogLines 'tray: icon added' $from
      $seen.Report = Get-UrAppLogLines "update: the update helper's last report" $from
    }
    if ($seen.Started.Count -gt 0 -and $seen.Report.Count -gt 0) { break }
    Start-Sleep -Seconds 2
  } while ((Get-Date) -lt $deadline)
  $seen.Processes | ForEach-Object { Write-Host "relaunched: pid $($_.Id) $($_.CommandLine)" }
  foreach ($name in 'Waiting', 'Running', 'Ended', 'Started', 'Report') {
    $seen.$name | Select-Object -Last 3 | ForEach-Object { Write-Host "app log ($name): $_" }
  }
  return $seen
}

# The relaunch's order: it said it waits, the marker went only once the
# helper had ended (a relaunch that did not wait would have been refused by
# the marker and never started), then it started and showed the report.
# Whether its first look found the helper still running depends on which of
# the two was quicker, so that line is checked for order when it is there,
# and printed. $helperEnded is when the test saw the helper exit.
function Assert-UrRelaunchWaited($seen, [datetime]$helperEnded) {
  if ($seen.Waiting.Count -eq 0) { throw "no relaunch logged that it waited for the update" }
  if ($seen.Ended.Count -eq 0) { throw "the relaunch's wait did not end with the marker's removal" }
  if ($seen.Started.Count -eq 0) { throw "the relaunched app did not start" }
  $waited = Get-UrAppLogTime $seen.Waiting[0]
  $ended = Get-UrAppLogTime $seen.Ended[0]
  $started = Get-UrAppLogTime $seen.Started[-1]
  if (-not ($waited -le $ended -and $ended -le $started)) {
    throw "the relaunch's lines are out of order: waiting $waited, marker removed $ended, icon $started"
  }
  $helperEndedAt = $helperEnded.TimeOfDay
  # the test saw the helper end up to one poll late
  if ($ended -lt $helperEndedAt.Subtract([TimeSpan]::FromSeconds(1))) {
    throw "the marker was removed at $ended, before the helper ended at $helperEndedAt"
  }
  if ($seen.Running.Count -gt 0) {
    $running = Get-UrAppLogTime $seen.Running[0]
    if (-not ($waited -le $running -and $running -le $ended)) { throw "the update-running line is out of order: $running" }
    Write-Host "OK: the relaunch waited ($waited), found the helper still running ($running), saw it end ($ended) and started ($started)"
  } else {
    Write-Host "OK: the relaunch waited ($waited), the helper had ended (seen $helperEndedAt) before its first look, the marker went ($ended) and it started ($started)"
  }
}

# ---- the test network: hosts entries and a test CA --------------------------

# A test CA in LocalMachine\Root and a TLS certificate it issued for $names,
# in LocalMachine\My.
function New-UrTestTls([string[]]$names) {
  $work = New-Item -ItemType Directory -Force (Join-Path $env:RUNNER_TEMP 'test-tls')
  $ca = New-SelfSignedCertificate -Type Custom -KeySpec Signature -Subject 'CN=A2s runner test CA' `
    -KeyExportPolicy Exportable -HashAlgorithm SHA256 -KeyLength 2048 -CertStoreLocation Cert:\LocalMachine\My `
    -KeyUsageProperty Sign -KeyUsage CertSign, CRLSign, DigitalSignature `
    -TextExtension @('2.5.29.19={critical}{text}ca=1&pathlength=0') -NotAfter (Get-Date).AddDays(1)
  $caFile = Join-Path $work "ca-$($ca.Thumbprint).cer"
  Export-Certificate -Cert $ca -FilePath $caFile | Out-Null
  $root = Import-Certificate -FilePath $caFile -CertStoreLocation Cert:\LocalMachine\Root
  $leaf = New-SelfSignedCertificate -Type SSLServerAuthentication -Subject "CN=$($names[0])" `
    -DnsName $names -Signer $ca -CertStoreLocation Cert:\LocalMachine\My -HashAlgorithm SHA256 `
    -KeyLength 2048 -NotAfter (Get-Date).AddDays(1)
  Write-Host "test CA $($ca.Thumbprint) in LocalMachine\Root; leaf $($leaf.Thumbprint) for $($names -join ', ')"
  return [pscustomobject]@{ Ca = $ca.Thumbprint; Root = $root.Thumbprint; Leaf = $leaf.Thumbprint; Work = $work.FullName }
}

# Points $names at this runner; returns the hosts file as it was.
function Add-UrHostsEntries([string[]]$names) {
  $original = [IO.File]::ReadAllText($UrHostsFile)
  $lines = ($names | ForEach-Object { "127.0.0.1 $_ $UrHostsTag" }) -join "`r`n"
  [IO.File]::AppendAllText($UrHostsFile, "`r`n$lines`r`n", [Text.Encoding]::ASCII)
  ipconfig /flushdns | Out-Null
  foreach ($name in $names) {
    $resolved = [Net.Dns]::GetHostAddresses($name) | ForEach-Object { $_.IPAddressToString }
    if (-not ($resolved -contains '127.0.0.1')) { throw "$name resolves to $($resolved -join ', '), not 127.0.0.1" }
  }
  return $original
}

# Starts one of the test servers and waits until it listens.
function Start-UrTestServer([string]$script, [string[]]$arguments, [string]$log) {
  $server = Start-Process -FilePath 'powershell.exe' -PassThru -WindowStyle Hidden -ArgumentList (@(
    '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot $script)) + $arguments)
  $deadline = (Get-Date).AddSeconds(30)
  while (-not (Test-Path $log) -or -not ((Get-Content $log) -match '^listening')) {
    if ($server.HasExited) { throw "$script exited ($($server.ExitCode)): $(Get-Content $log -ErrorAction SilentlyContinue)" }
    if ((Get-Date) -gt $deadline) { throw "$script is not listening after 30 s" }
    Start-Sleep -Milliseconds 250
  }
  Write-Host "$script pid $($server.Id) listening on 127.0.0.1:443"
  return $server
}

# Undoes Set-UrTamperedDownload or Set-UrFakeGitHub: the server, the hosts
# entries and the test certificates, each checked gone.
function Remove-UrTestNetwork($state) {
  if ($null -eq $state) { return }
  if ($state.Server -and -not $state.Server.HasExited) { Stop-Process -Id $state.Server.Id -Force -ErrorAction SilentlyContinue }
  if (Test-Path $state.Log) { Get-Content $state.Log | ForEach-Object { Write-Host "test server: $_" } }
  if ($null -ne $state.Hosts) {
    [IO.File]::WriteAllText($UrHostsFile, $state.Hosts, [Text.Encoding]::ASCII)
    ipconfig /flushdns | Out-Null
  }
  foreach ($store in 'Cert:\LocalMachine\My', 'Cert:\LocalMachine\Root') {
    Get-ChildItem $store | Where-Object { $_.Thumbprint -in @($state.Tls.Ca, $state.Tls.Root, $state.Tls.Leaf) } |
      ForEach-Object { Remove-Item -LiteralPath $_.PSPath -Force; Write-Host "removed $($_.Thumbprint) from $store" }
  }
  if ((Get-Content $UrHostsFile) -match [regex]::Escape($UrHostsTag)) { throw "the hosts file still points hosts here" }
  foreach ($name in $state.Names) {
    $resolved = [Net.Dns]::GetHostAddresses($name) | ForEach-Object { $_.IPAddressToString }
    if ($resolved -contains '127.0.0.1') { throw "$name still resolves to 127.0.0.1" }
  }
  Write-Host "OK: the hosts file, the server and the test certificates are gone"
}

# ---- the tampered download ---------------------------------------------------

# Points the release-asset hosts at this runner and serves $msi there with one
# byte flipped, over TLS with a certificate a test CA in LocalMachine\Root
# issued: everything the helper checks about the connection passes, and only
# the bytes are wrong. Returns what Remove-UrTestNetwork undoes.
function Set-UrTamperedDownload([string]$msi) {
  $state = [pscustomobject]@{ Server = $null; Log = ''; Hosts = $null; Tls = $null; Names = $UrAssetHosts }
  try {
    $work = New-Item -ItemType Directory -Force (Join-Path $env:RUNNER_TEMP 'tamper')
    $body = Join-Path $work 'tampered.msi'
    $bytes = [IO.File]::ReadAllBytes($msi)
    $flip = [int]($bytes.Length / 2)
    $bytes[$flip] = $bytes[$flip] -bxor 0xFF
    [IO.File]::WriteAllBytes($body, $bytes)
    Write-Host ("tampered copy: byte {0} of {1} flipped; SHA-256 {2} (the release's is {3})" -f $flip, $bytes.Length,
      (Get-FileHash -Algorithm SHA256 $body).Hash.ToLowerInvariant(), (Get-FileHash -Algorithm SHA256 $msi).Hash.ToLowerInvariant())
    $state.Tls = New-UrTestTls $UrAssetHosts
    $state.Hosts = Add-UrHostsEntries $UrAssetHosts
    $state.Log = Join-Path $work 'server.log'
    $state.Server = Start-UrTestServer 'vpn-apply-tamper-server.ps1' @('-Thumbprint', $state.Tls.Leaf, '-Body', $body, '-Log', $state.Log) $state.Log
    return $state
  } catch {
    Remove-UrTestNetwork $state
    throw
  }
}

# ---- a local stand-in for GitHub ------------------------------------------------

# Answers for GitHub on this runner (vpn-apply-fake-github.ps1): the release
# list of the official feed holds one release, $tag, immutable and not a
# prerelease, with $msi as its x64 MSI and GitHub's SHA-256 of it, at the
# download URL the official feed names, redirected once to the release-asset
# host. Everything the official helper checks passes except that the list is
# not GitHub's. Returns what Remove-UrTestNetwork undoes.
function Set-UrFakeGitHub([string]$msi, [string]$version, [string]$name = 'fake-github') {
  $state = [pscustomobject]@{ Server = $null; Log = ''; Hosts = $null; Tls = $null; Names = $UrGitHubHosts }
  try {
    $work = New-Item -ItemType Directory -Force (Join-Path $env:RUNNER_TEMP $name)
    $tag = "v$version"
    $asset = "URnetwork-$version-x64.msi"
    $download = "/$UrOfficialOwner/$UrOfficialRepo/releases/download/$tag/$asset"
    $digest = (Get-FileHash -Algorithm SHA256 $msi).Hash.ToLowerInvariant()
    $list = @(@{
        tag_name = $tag; draft = $false; prerelease = $false; immutable = $true
        assets = @(@{ name = $asset; browser_download_url = "https://github.com$download"; digest = "sha256:$digest" })
      })
    $listPath = Join-Path $work 'releases.json'
    [IO.File]::WriteAllText($listPath, (ConvertTo-Json -InputObject $list -Depth 5 -Compress), [Text.Encoding]::ASCII)
    Write-Host "stand-in release list: $(Get-Content -Raw $listPath)"
    $state.Tls = New-UrTestTls $UrGitHubHosts
    $state.Hosts = Add-UrHostsEntries $UrGitHubHosts
    $state.Log = Join-Path $work 'server.log'
    $state.Server = Start-UrTestServer 'vpn-apply-fake-github.ps1' @('-Thumbprint', $state.Tls.Leaf, '-ListJson', $listPath,
      '-RepoId', $UrOfficialRepoId, '-DownloadPath', $download, '-Body', $msi, '-Log', $state.Log) $state.Log
    return $state
  } catch {
    Remove-UrTestNetwork $state
    throw
  }
}

# ---- the other failures the update meets ------------------------------------------

# A hidden process that holds $path open for reading, sharing read only (no
# delete), so Windows Installer can neither replace nor rename it. Returns
# the process; stop it to let go.
function Start-UrFileHolder([string]$path) {
  $script = "`$f = [IO.File]::Open('$path', 'Open', 'Read', 'Read'); Start-Sleep -Seconds 3600; `$f.Close()"
  $holder = Start-Process -FilePath 'powershell.exe' -PassThru -WindowStyle Hidden -ArgumentList @(
    '-NoProfile', '-NonInteractive', '-Command', $script)
  Start-Sleep -Seconds 3
  if ($holder.HasExited) { throw "the file holder exited at once ($($holder.ExitCode))" }
  try {
    $probe = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite, Delete')
    $probe.Close()
    throw "$path is not held: it opened with delete sharing"
  } catch [IO.IOException] {
    Write-Host "OK: $path is held by pid $($holder.Id) without delete sharing"
  }
  return $holder
}

# Windows Installer's machine policy that turns its Restart Manager off, so a
# held file stays held and the install ends in 3010.
$UrInstallerPolicy = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\Installer'
function Set-UrRestartManagerOff([bool]$off) {
  if ($off) {
    New-Item -Force $UrInstallerPolicy | Out-Null
    New-ItemProperty -Force -Path $UrInstallerPolicy -Name DisableAutomaticApplicationShutdown -PropertyType DWord -Value 1 | Out-Null
  } else {
    Remove-ItemProperty -Path $UrInstallerPolicy -Name DisableAutomaticApplicationShutdown -ErrorAction SilentlyContinue
  }
  $value = (Get-ItemProperty -Path $UrInstallerPolicy -ErrorAction SilentlyContinue).DisableAutomaticApplicationShutdown
  Write-Host "Windows Installer DisableAutomaticApplicationShutdown = $value"
}

# msiexec as SYSTEM in session 0, through a scheduled task, as a deployment
# tool runs it; returns its exit code.
function Invoke-UrMsiexecAsSystem([string]$verb, [string]$target, [string]$mode, [string]$logName, [int]$timeoutSeconds = 600) {
  $log = Join-Path $UrLogDir $logName
  $taskName = 'a2s-msiexec-as-system'
  $arguments = "$verb `"$target`" $mode /norestart /l*v `"$log`""
  Write-Host "as SYSTEM: msiexec $arguments"
  $action = New-ScheduledTaskAction -Execute "$env:SystemRoot\System32\msiexec.exe" -Argument $arguments
  $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
  Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Force | Out-Null
  try {
    Start-ScheduledTask -TaskName $taskName
    $deadline = (Get-Date).AddSeconds($timeoutSeconds)
    # 0x41301: the task is running; 0x41303: it has not run yet
    $pending = @(0x41301, 0x41303)
    do {
      Start-Sleep -Seconds 2
      $task = Get-ScheduledTask -TaskName $taskName
      $info = Get-ScheduledTaskInfo -TaskName $taskName
      if ((Get-Date) -gt $deadline) { throw "msiexec as SYSTEM still runs after $timeoutSeconds s" }
    } while ($task.State -eq 'Running' -or $pending -contains [int]$info.LastTaskResult)
    $code = [int]$info.LastTaskResult
    Write-Host "msiexec as SYSTEM exit code: $code"
    return $code
  } finally {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
  }
}

# The app's process, the installed exes and what the session says about them,
# after an install that met a running app.
function Show-UrAfterInstall($app) {
  "OBSERVED: the app (pid $($app.Id)) still running: $(-not $app.HasExited)" | Write-Host
  if (-not $app.HasExited) {
    $tray = @((Get-UrTopLevelWindows $app.Id) | Where-Object { $_.Class -eq 'URnetworkTrayWindow' })
    "OBSERVED: its tray window still exists: $($tray.Count -gt 0)" | Write-Host
  }
  foreach ($exe in 'URnetwork.exe', 'urnetworkd.exe', 'URnetworkUpdate.exe') {
    $path = Join-Path $UrInstallDir $exe
    if (Test-Path $path) { "OBSERVED: $exe on disk is $(Get-UrFileVersion $path)" | Write-Host }
  }
  $pending = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager' -Name PendingFileRenameOperations -ErrorAction SilentlyContinue)
  if ($pending) { "OBSERVED: pending file renames: $(@($pending.PendingFileRenameOperations | Where-Object { $_ }) -join ' | ')" | Write-Host }
}
