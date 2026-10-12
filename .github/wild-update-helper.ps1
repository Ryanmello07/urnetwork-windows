# The "helper" leg of .github/workflows/wild-update.yml: the published vN,
# installed as a user installs it, its tray app started as a user starts it,
# and then its elevated update helper alone, asked for the release the feed
# offers, as the tray app asks it after the elevation prompt (A2b's "real"
# leg, step 3): URnetworkUpdate.exe --apply-update <tag>, against GitHub
# itself, nothing redirected. The test writes the update marker the moment the
# app exits while the helper runs, as the app itself does when it started the
# helper (vpn-apply-lib.ps1 Invoke-UrHelper).
#
# Reads what the job's earlier steps left in %RUNNER_TEMP%: reading.json,
# official-traits.json, vn.productcode, msis\vn.json and msis\official.json.
# Pure ASCII, Windows PowerShell 5.1.
#
# SPDX-License-Identifier: MPL-2.0

. (Join-Path $PSScriptRoot 'wild-update-lib.ps1')

$reading = Get-Content -Raw (Join-Path $env:RUNNER_TEMP 'reading.json') | ConvertFrom-Json
$traits = Get-Content -Raw (Join-Path $env:RUNNER_TEMP 'official-traits.json') | ConvertFrom-Json
$offered = $reading.Offered
$vn = Get-UrExpect 'vn'
$expect = Get-UrExpect 'official'
$productBefore = Get-Content (Join-Path $env:RUNNER_TEMP 'vn.productcode')
"vN $($vn.version) (code $($vn.code)); vN's rules offer $($offered.Tag) on the UTC day that began $(Format-UrUnix ([long]$reading.DayStart))"

$app = $null
try {
  Write-Step "1. vN's tray app, started as a user starts it, makes its launch check against GitHub"
  [void](Wait-UrAnonymousRequests 6 45)
  Assert-WildSameDay $reading
  $check = $null
  for ($attempt = 1; $attempt -le 2 -and $null -eq $check; $attempt++) {
    $from = Get-UrAppLogLength
    $app = Start-UrTrayApp
    $check = Wait-UrTrayCheck $from
    if ($null -eq $check) {
      "the app's launch check did not complete (attempt $attempt): starting it again once GitHub allows this address requests"
      Stop-UrAppProcesses
      $app.WaitForExit(20000) | Out-Null
      [void](Wait-UrAnonymousRequests 6 45)
    }
  }
  if ($null -eq $check) { throw "vN's own launch check did not complete, twice" }
  "the app (pid $($app.Id)): $(Get-WildTokenText $app.Id)"
  Assert-WildCheck $check $reading $vn
  [void](Save-WildWindowOf $app.Id '1-app-as-started' 20)

  Write-Step "2. the helper alone, asked for the offered release, with vN's app running"
  $stepStarted = Get-Date
  $from = Get-UrAppLogLength
  $run = Invoke-UrHelperPatiently $offered.Tag 1200 $app
  $helperEnded = Get-Date
  Show-UrUpdates
  if ($run.ExitCode -ne 0) { throw ("the helper exited {0} (0x{0:X8}), want 0" -f $run.ExitCode) }
  Assert-UrResult (Read-UrResult) $offered.Tag ([string]$offered.Code) 0

  $log = Get-UrHelperLog $offered.Tag
  Assert-UrHelperLogHas $log ("update helper " + [regex]::Escape([string]$vn.version) + " \(code " + $vn.code + "\) for " + [regex]::Escape([string]$offered.Tag) + "$") "it is vN's helper, asked for the offered tag"
  Assert-UrHelperLogHas $log ("release " + [regex]::Escape([string]$offered.Tag) + " is offered; downloading " + [regex]::Escape([string]$offered.Asset) + " from (release-assets|objects)\.githubusercontent\.com$") "its own list offers the release, downloaded from GitHub's storage"
  Assert-UrHelperLogHas $log ("SHA-256 " + $offered.Digest + " matches GitHub's for " + [regex]::Escape([string]$offered.Asset)) "the download is GitHub's SHA-256"
  Assert-UrHelperLogHas $log ("package " + [regex]::Escape([string]$offered.Asset) + " is this product's ProductVersion " + [regex]::Escape([string]$expect.msi_version) + "$") "the package is this product at the release code's ProductVersion"
  if ($reading.Waiting) {
    Assert-UrHelperLogHas $log ("release " + [regex]::Escape([string]$reading.Waiting.Tag) + " had not been out, unchanged, for 24 h when the server's day began: skipped") 'the newer release is held back by the soak'
  }

  $tagDir = Join-Path $UrUpdatesDir $offered.Tag
  $msiLog = Join-Path $tagDir 'install.log'
  if (-not (Test-Path $msiLog)) { throw "no install.log in $tagDir" }
  Copy-Item $msiLog (Join-Path $UrLogDir 'official-apply.log')
  $product = Assert-UrInstalled $expect 'official-apply.log'
  if ($product.ProductCode -eq $productBefore) { throw "vN's product is still registered" }
  Assert-WildExeVersions $expect
  [void](Show-WildInstalled 'after')
  $service = Get-UrServiceInfo
  if (-not $service -or $service.State -ne 'Running') { throw "urnetworkd is not running: $($service | Out-String)" }
  "OK: urnetworkd is $($service.State) from $($service.PathName)"

  $kept = Join-Path $tagDir $offered.Asset
  if (-not (Test-Path $kept)) { throw "the MSI is not kept at $kept" }
  if ((Get-FileHash -Algorithm SHA256 $kept).Hash.ToLowerInvariant() -ne $offered.Digest) { throw "the kept MSI is not GitHub's bytes" }
  foreach ($path in $UrUpdatesDir, $tagDir, $kept, $msiLog, (Join-Path $tagDir 'update-helper.log'), $UrResultPath) {
    Assert-UrAdminOnly $path
  }
  $moved = Join-Path $tagDir "URnetworkUpdate.$($run.Process.Id).running"
  if (-not (Test-Path $moved)) { throw "the helper's image is not at $moved" }
  if ((Get-UrFileVersion $moved) -ne $vn.file_version) { throw "$moved is not vN's helper" }
  "OK: vN's helper installed $($offered.Tag): exit 0, the report, one product at $($expect.msi_version), the MSI kept and admin-only"

  if ($null -eq $run.Marked) { throw "vN's app did not exit while the helper ran: the installer did not close it" }
  if ($traits.CloseApp) {
    if ((Get-UrMsiLogLines $msiLog 'Property\(S\): URNETWORK_APP_CLOSABLE = ').Count -eq 0) { throw "URNETWORK_APP_CLOSABLE was not set" }
    $evidence = Get-UrCloseEvidence $msiLog
    $closed = Get-UrAppLogLines 'tray: WM_CLOSE received, exiting' $from
    if ($closed.Count -ne 1) { throw "the app logged $($closed.Count) WM_CLOSE lines, want one" }
    $closedAt = Get-UrAppLogTime $closed[0]
    "Wix4CloseApplications_X64 $($evidence.CloseStart) - $($evidence.CloseEnd); WM_CLOSE received $closedAt"
    if ($null -eq $evidence.CloseStart -or $null -eq $evidence.CloseEnd) { throw "no Wix4CloseApplications_X64 in the log" }
    if ($closedAt -lt $evidence.CloseStart -or $closedAt -gt $evidence.CloseEnd) { throw "the app's WM_CLOSE came outside Wix4CloseApplications_X64" }
    "OK: the release's own CloseApplication closed vN's app at $closedAt, inside its action"
  } else {
    "OBSERVED: $($offered.Tag)'s package has no CloseApplication; vN's app exited while the helper ran"
  }
  $rmEvents = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; StartTime = $stepStarted } -ErrorAction SilentlyContinue |
    Where-Object { $_.ProviderName -match 'RestartManager|MsiInstaller' } | Sort-Object TimeCreated)
  $rmEvents | ForEach-Object { "event $($_.ProviderName) $($_.Id) at $($_.TimeCreated.ToString('HH:mm:ss.fff')): $(($_.Message -split "`r?`n")[0])" }

  if (-not $traits.Relaunch) { throw "$($offered.Tag)'s package has no RelaunchAfterUpdate" }
  if ((Get-UrMsiLogLines $msiLog 'Action start \d{1,2}:\d{2}:\d{2}: RelaunchAfterUpdate\.').Count -eq 0) { throw "RelaunchAfterUpdate did not run" }
  $seen = Wait-UrRelaunch $from 150
  if ($seen.Processes.Count -eq 0) { throw "no URnetwork.exe --after-update was started" }
  Assert-UrRelaunchWaited $seen $helperEnded
  if ($seen.Report.Count -eq 0) { throw "the relaunched app did not show the helper's report" }
  $relaunched = $seen.Processes[0]
  "the relaunched app (pid $($relaunched.Id)): $(Get-WildTokenText $relaunched.Id)"
  $relaunchedImage = (Get-Process -Id $relaunched.Id).Path
  if ((Get-UrFileVersion $relaunchedImage) -ne $expect.file_version) { throw "the relaunched app runs $relaunchedImage at $(Get-UrFileVersion $relaunchedImage)" }
  "OK: the package started the app again (pid $($relaunched.Id), $relaunchedImage at $($expect.file_version)), which waited for the helper and read its report"

  $left = Wait-WildNoHelper 90
  if ($left.Count -ne 0) { throw "an update helper is still running: $($left -join '; ')" }
  "OK: no URnetworkUpdate.exe is left running"
  [void](Save-WildWindowOf $relaunched.Id '3-after-relaunch' 60)
  "PASS: vN's helper alone updated the published vN to $($offered.Tag)"
} finally {
  Stop-UrAppProcesses
}
