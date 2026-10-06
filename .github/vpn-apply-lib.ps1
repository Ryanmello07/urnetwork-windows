# Helpers for .github/workflows/vpn-apply.yml (the A2s update helper test), on
# top of vpn-upgrade-lib.ps1. Dot-sourced by every test step:
#   . "$env:GITHUB_WORKSPACE\.github\vpn-apply-lib.ps1"
# Runs on a disposable GitHub runner only: it installs and updates URnetwork
# machine-wide, edits the hosts file and trusts a test CA machine-wide, and
# undoes the last two before the update that must succeed.
# Pure ASCII, Windows PowerShell 5.1.
#
# SPDX-License-Identifier: MPL-2.0

. (Join-Path $PSScriptRoot 'vpn-upgrade-lib.ps1')

$UrHelperPath = Join-Path $UrInstallDir 'URnetworkUpdate.exe'
$UrUpdatesDir = Join-Path $UrInstallDir 'updates'
$UrResultPath = Join-Path $UrUpdatesDir 'last-result.json'
# The per-user app root with no URNETWORK_APP_ROOT: the installer's relaunch
# starts the app without one (the helper drops every URNETWORK_* variable).
$UrAppRoot = Join-Path $env:LOCALAPPDATA 'URnetwork\app'
$UrAppLog = Join-Path $UrAppRoot 'logs\urnetwork-app.log'
$UrMarkerPath = Join-Path $UrAppRoot 'update_in_progress'

# Common/UpdateResult.h
$UrRefusalDigest = 0x20000009

# The hosts a release download may be redirected to (ReleaseSelection.h
# kAssetRedirectHosts); the tampered leg points both at this runner.
$UrAssetHosts = @('release-assets.githubusercontent.com', 'objects.githubusercontent.com')
$UrHostsFile = Join-Path $env:SystemRoot 'System32\drivers\etc\hosts'
$UrHostsTag = '# a2s-runner-test'

# What the runner feed compiles into URnetworkUpdate.exe (Updater.vcxproj's
# /p:UrnUpdateRunnerTest* properties); the official helper has neither. Not
# the repository's name: the PDB path every build embeds carries it, since a
# runner checks out into <repo>\<repo>.
$UrRunnerFeedStrings = @('a2s-runner-test-', 'Ryanmello07')

# Whether a file's bytes contain an ASCII string (Latin-1 maps each byte to
# one character, so the search is over the bytes themselves).
function Test-UrFileHasAscii([string]$path, [string]$text) {
  $bytes = [IO.File]::ReadAllBytes($path)
  return [Text.Encoding]::GetEncoding(28591).GetString($bytes).Contains($text)
}

# Whether a URnetworkUpdate.exe was built with the runner feed: every string
# the feed compiles in is there, or none is.
function Get-UrHelperFeed([string]$path) {
  $found = @($UrRunnerFeedStrings | Where-Object { Test-UrFileHasAscii $path $_ })
  if ($found.Count -eq $UrRunnerFeedStrings.Count) { return 'runner' }
  if ($found.Count -eq 0) { return 'official' }
  throw "$path carries only part of the runner feed: $($found -join ', ')"
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

# The update marker the tray app writes once the helper has started
# (SingleInstance.cpp RecordUpdateInProgress): "<pid> <creation FILETIME>
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
# URnetworkUpdate.exe --apply-update <tag>, and waits for it. Returns the
# process (exited) and how long it ran.
function Invoke-UrHelper([string]$tag, [int]$timeoutSeconds = 900, [switch]$WriteMarker) {
  Write-Host "$UrHelperPath --apply-update $tag"
  $started = Get-Date
  $helper = Start-Process -FilePath $UrHelperPath -ArgumentList @('--apply-update', $tag) -PassThru
  $null = $helper.Handle
  if ($WriteMarker) { Write-UrUpdateMarker $helper }
  if (-not $helper.WaitForExit($timeoutSeconds * 1000)) {
    Stop-Process -Id $helper.Id -Force -ErrorAction SilentlyContinue
    throw "the helper (pid $($helper.Id)) was still running after $timeoutSeconds s"
  }
  $helper.WaitForExit()
  $seconds = [int]((Get-Date) - $started).TotalSeconds
  Write-Host ("helper pid {0} exit code {1} (0x{1:X8}) after {2} s" -f $helper.Id, $helper.ExitCode, $seconds)
  return [pscustomobject]@{ Process = $helper; ExitCode = [long]$helper.ExitCode; Seconds = $seconds }
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

function Get-UrMsiLogLines([string]$path, [string]$pattern) {
  $bytes = [IO.File]::ReadAllBytes($path)
  $text = if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) {
    [Text.Encoding]::Unicode.GetString($bytes, 2, $bytes.Length - 2)
  } else { [Text.Encoding]::Default.GetString($bytes) }
  return ,@($text -split "`r?`n" | Where-Object { $_ -match $pattern })
}

# ---- the tampered download ---------------------------------------------------

# Points the release-asset hosts at this runner and serves $msi there with one
# byte flipped, over TLS with a certificate a test CA in LocalMachine\Root
# issued: everything the helper checks about the connection passes, and only
# the bytes are wrong. Returns what Remove-UrTamperedDownload undoes.
function Set-UrTamperedDownload([string]$msi) {
  $work = New-Item -ItemType Directory -Force (Join-Path $env:RUNNER_TEMP 'tamper')
  $body = Join-Path $work 'tampered.msi'
  $bytes = [IO.File]::ReadAllBytes($msi)
  $flip = [int]($bytes.Length / 2)
  $bytes[$flip] = $bytes[$flip] -bxor 0xFF
  [IO.File]::WriteAllBytes($body, $bytes)
  Write-Host ("tampered copy: byte {0} of {1} flipped; SHA-256 {2} (the release's is {3})" -f $flip, $bytes.Length,
    (Get-FileHash -Algorithm SHA256 $body).Hash.ToLowerInvariant(), (Get-FileHash -Algorithm SHA256 $msi).Hash.ToLowerInvariant())

  $ca = New-SelfSignedCertificate -Type Custom -KeySpec Signature -Subject 'CN=A2s runner test CA' `
    -KeyExportPolicy Exportable -HashAlgorithm SHA256 -KeyLength 2048 -CertStoreLocation Cert:\LocalMachine\My `
    -KeyUsageProperty Sign -KeyUsage CertSign, CRLSign, DigitalSignature `
    -TextExtension @('2.5.29.19={critical}{text}ca=1&pathlength=0') -NotAfter (Get-Date).AddDays(1)
  $caFile = Join-Path $work 'ca.cer'
  Export-Certificate -Cert $ca -FilePath $caFile | Out-Null
  $root = Import-Certificate -FilePath $caFile -CertStoreLocation Cert:\LocalMachine\Root
  $leaf = New-SelfSignedCertificate -Type SSLServerAuthentication -Subject "CN=$($UrAssetHosts[0])" `
    -DnsName $UrAssetHosts -Signer $ca -CertStoreLocation Cert:\LocalMachine\My -HashAlgorithm SHA256 `
    -KeyLength 2048 -NotAfter (Get-Date).AddDays(1)
  Write-Host "test CA $($ca.Thumbprint) in LocalMachine\Root; leaf $($leaf.Thumbprint) for $($UrAssetHosts -join ', ')"

  $original = [IO.File]::ReadAllText($UrHostsFile)
  $lines = ($UrAssetHosts | ForEach-Object { "127.0.0.1 $_ $UrHostsTag" }) -join "`r`n"
  [IO.File]::AppendAllText($UrHostsFile, "`r`n$lines`r`n", [Text.Encoding]::ASCII)
  ipconfig /flushdns | Out-Null
  foreach ($name in $UrAssetHosts) {
    $resolved = [Net.Dns]::GetHostAddresses($name) | ForEach-Object { $_.IPAddressToString }
    if (-not ($resolved -contains '127.0.0.1')) { throw "$name resolves to $($resolved -join ', '), not 127.0.0.1" }
  }

  $log = Join-Path $work 'server.log'
  $server = Start-Process -FilePath 'powershell.exe' -PassThru -WindowStyle Hidden -ArgumentList @(
    '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File',
    (Join-Path $PSScriptRoot 'vpn-apply-tamper-server.ps1'), '-Thumbprint', $leaf.Thumbprint,
    '-Body', $body, '-Log', $log)
  $deadline = (Get-Date).AddSeconds(30)
  while (-not (Test-Path $log) -or -not ((Get-Content $log) -match '^listening')) {
    if ($server.HasExited) { throw "the tamper server exited ($($server.ExitCode)): $(Get-Content $log -ErrorAction SilentlyContinue)" }
    if ((Get-Date) -gt $deadline) { throw "the tamper server is not listening after 30 s" }
    Start-Sleep -Milliseconds 250
  }
  Write-Host "tamper server pid $($server.Id) listening on 127.0.0.1:443"
  return [pscustomobject]@{ Server = $server; Log = $log; Hosts = $original; Ca = $ca.Thumbprint;
    Root = $root.Thumbprint; Leaf = $leaf.Thumbprint }
}

function Remove-UrTamperedDownload($state) {
  if ($null -eq $state) { return }
  if ($state.Server -and -not $state.Server.HasExited) { Stop-Process -Id $state.Server.Id -Force -ErrorAction SilentlyContinue }
  if (Test-Path $state.Log) { Get-Content $state.Log | ForEach-Object { Write-Host "tamper server: $_" } }
  [IO.File]::WriteAllText($UrHostsFile, $state.Hosts, [Text.Encoding]::ASCII)
  ipconfig /flushdns | Out-Null
  foreach ($store in 'Cert:\LocalMachine\My', 'Cert:\LocalMachine\Root') {
    Get-ChildItem $store | Where-Object { $_.Thumbprint -in @($state.Ca, $state.Root, $state.Leaf) } |
      ForEach-Object { Remove-Item -LiteralPath $_.PSPath -Force; Write-Host "removed $($_.Thumbprint) from $store" }
  }
  if ((Get-Content $UrHostsFile) -match [regex]::Escape($UrHostsTag)) { throw "the hosts file still points asset hosts here" }
  foreach ($name in $UrAssetHosts) {
    $resolved = [Net.Dns]::GetHostAddresses($name) | ForEach-Object { $_.IPAddressToString }
    if ($resolved -contains '127.0.0.1') { throw "$name still resolves to 127.0.0.1" }
  }
  Write-Host "OK: the hosts file, the server and the test certificates are gone"
}
