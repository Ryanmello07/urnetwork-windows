# The "window" leg of .github/workflows/wild-update.yml: the published vN,
# installed as a user installs it, updating itself from its own window
# against GitHub itself, nothing redirected and nothing stood in for.
#
# The window is the signed-in shell the app shows with --preview-ui
# (app/src/App/Startup.h): the update banner lives on the connect screen,
# behind sign-in, and no account signs in on a runner. The update path behind
# the banner is the app's own, whatever the shell: UpdateChecker's launch
# check, its banner, and Update, invoked through UI Automation's Invoke
# pattern as a click does, which runs the app's download, its check against
# GitHub's SHA-256 and its hand-off to the elevated URnetworkUpdate.exe; the
# helper fetches the list itself, installs the release with msiexec, the
# package closes the app and starts it again. Captured with PrintWindow into
# %RUNNER_TEMP%\ui-shots.
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
$offeredV = [regex]::Escape([string]$offered.Version)
"vN $($vn.version) (code $($vn.code)); vN's rules offer $($offered.Tag) on the UTC day that began $(Format-UrUnix ([long]$reading.DayStart))"

$app = $null
try {
  # ---- 1. vN's own check, in its own window --------------------------------------
  Write-Step "1. vN's tray app, started as a user starts it, in its window, makes its launch check against GitHub"
  [void](Wait-UrAnonymousRequests 6 45)
  Assert-WildSameDay $reading
  $check = $null
  for ($attempt = 1; $attempt -le 2 -and $null -eq $check; $attempt++) {
    $from = Get-UrAppLogLength
    $app = Start-UrPreviewApp ''
    # the launch check runs 30 s after the app starts (UpdateChecker.cpp kLaunchDelay)
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
  Start-Sleep -Seconds 2
  Save-UrShot $app.Id '1-window-as-opened'
  $asOpened = Get-UrBanner $app.Id
  Show-UrBanner $asOpened 'at the size the window opens with'
  # tall enough to lay the whole connect column out, so the banner's controls
  # are on the screen to be read and invoked (vpn-feed-ui-lib.ps1 Expand-UrWindow)
  Expand-UrWindow $app.Id
  $banner = Wait-UrBanner $app.Id { param($b) $b.Open -and $b.Later } 'the offer, with Later' 30
  Assert-WildOffer $banner $offered 'before the click'
  Save-UrShot $app.Id '1-banner-before-click'
  "OK: vN's banner offers v$($offered.Version) with Update and Later"

  # ---- 2. Update ------------------------------------------------------------------
  Write-Step "2. Update, invoked as a click does: the app's download and check, the helper's elevation, the install and the relaunch"
  $stepStarted = Get-Date
  $from = Get-UrAppLogLength
  Invoke-UrId $app.Id $UrIdAction
  [void](Wait-UrAppLog ("update: applying v" + $offeredV + " \(code " + $offered.Code + "\)$") $from 'that it applies the release on the banner' 60)
  $again = Get-UrAppLogLines 'is no longer fresh' $from
  if ($again.Count -ne 0) { "OBSERVED: the offer was checked again before the click was acted on: $($again -join '; ')" }
  # the helper, the moment the tray app starts it: after the app's own download
  # of the release and its check against GitHub's SHA-256
  $helper = $null
  $deadline = (Get-Date).AddSeconds(900)
  while ($null -eq $helper -and (Get-Date) -lt $deadline) {
    $helper = Get-Process -Name URnetworkUpdate -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $helper) {
      $over = Get-UrAppLogLines 'update: the update helper ended with|update: the update helper did not start|update: download failed|update: checksum mismatch|update: GitHub asked for no request yet|nothing was started' $from
      if ($over.Count -gt 0) { break }
      Start-Sleep -Milliseconds 100
    }
  }
  if ($null -eq $helper) {
    (Get-UrAppLogLines 'update: ' $from) | ForEach-Object { Write-Host "app log: $_" }
    Show-UrBanner (Get-UrBanner $app.Id) 'with no helper running'
    Save-UrShot $app.Id '2-no-helper'
    Show-UrUpdates
    throw "the tray app's Update did not leave a running update helper"
  }
  $null = $helper.Handle
  $helperLine = @(Get-CimInstance Win32_Process -Filter "ProcessId=$($helper.Id)" | ForEach-Object { $_.CommandLine })
  "the tray app started URnetworkUpdate.exe as pid $($helper.Id) after $([int]((Get-Date) - $stepStarted).TotalSeconds) s: $($helperLine -join ' ')"
  "the helper (pid $($helper.Id)): $(Get-WildTokenText $helper.Id)"
  try { Save-UrShot $app.Id '2-installing' } catch { "screenshot 2-installing: not taken ($($_.Exception.Message))" }
  # the installer closes the app while the helper runs, and the app records
  # the helper in its update marker as it exits
  $appExited = $null
  $markerSeen = $false
  $deadline = (Get-Date).AddSeconds(1200)
  while (-not $helper.WaitForExit(100)) {
    if ($null -eq $appExited -and $app.HasExited) {
      $appExited = Get-Date
      $markerSeen = Test-Path -LiteralPath $UrMarkerPath
      "the app exited while the helper ran; its update marker is there: $markerSeen"
    }
    if ((Get-Date) -gt $deadline) { throw "the helper (pid $($helper.Id)) was still running after 1200 s" }
  }
  $helper.WaitForExit()
  $helperEnded = Get-Date
  ("helper pid {0} exit code {1} (0x{1:X8}), {2} s after the click" -f $helper.Id, $helper.ExitCode, [int]($helperEnded - $stepStarted).TotalSeconds)
  Show-UrUpdates
  (Get-UrAppLogLines 'update: ' $from) | ForEach-Object { Write-Host "app log: $_" }
  if ($helper.ExitCode -ne 0) { throw ("the helper exited {0} (0x{0:X8}), want 0" -f $helper.ExitCode) }
  Assert-UrResult (Read-UrResult) $offered.Tag ([string]$offered.Code) 0

  # what the tray app did before it handed over
  $appLines = Get-UrAppLogLines 'update: ' $from
  foreach ($want in @(("update: verified " + [regex]::Escape([string]$offered.Asset) + " \(" + $offered.Digest + "\)"),
      ("update: the update helper is installing v" + $offeredV))) {
    if (@($appLines | Where-Object { $_ -match $want }).Count -ne 1) { throw "the app's log has no single line matching $want" }
  }
  "OK: the tray app downloaded $($offered.Asset) from GitHub, checked it against GitHub's SHA-256 $($offered.Digest) and started the helper"

  # what the helper did: vN's own helper, its own list, GitHub's bytes
  $log = Get-UrHelperLog $offered.Tag
  Assert-UrHelperLogHas $log ("update helper " + [regex]::Escape([string]$vn.version) + " \(code " + $vn.code + "\) for " + [regex]::Escape([string]$offered.Tag) + "$") "it is vN's helper, asked for the offered tag"
  Assert-UrHelperLogHas $log ("release " + [regex]::Escape([string]$offered.Tag) + " is offered; downloading " + [regex]::Escape([string]$offered.Asset) + " from (release-assets|objects)\.githubusercontent\.com$") "its own list offers the release, downloaded from GitHub's storage"
  Assert-UrHelperLogHas $log ("SHA-256 " + $offered.Digest + " matches GitHub's for " + [regex]::Escape([string]$offered.Asset)) "the download is GitHub's SHA-256"
  Assert-UrHelperLogHas $log ("package " + [regex]::Escape([string]$offered.Asset) + " is this product's ProductVersion " + [regex]::Escape([string]$expect.msi_version) + "$") "the package is this product at the release code's ProductVersion"
  Assert-UrHelperLogHas $log 'msiexec started \(process \d+\): .* /passive /norestart /l\*v .* UPDATE_RELAUNCH=1$' 'how it ran msiexec'
  if ($reading.Waiting) {
    Assert-UrHelperLogHas $log ("release " + [regex]::Escape([string]$reading.Waiting.Tag) + " had not been out, unchanged, for 24 h when the server's day began: skipped") 'the newer release is held back by the soak'
  }

  # msiexec's log, under the admin-only install folder
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

  # the repair source: GitHub's bytes, admin-only
  $kept = Join-Path $tagDir $offered.Asset
  if (-not (Test-Path $kept)) { throw "the MSI is not kept at $kept" }
  if ((Get-FileHash -Algorithm SHA256 $kept).Hash.ToLowerInvariant() -ne $offered.Digest) { throw "the kept MSI is not GitHub's bytes" }
  foreach ($path in $UrUpdatesDir, $tagDir, $kept, $msiLog, (Join-Path $tagDir 'update-helper.log'), $UrResultPath) {
    Assert-UrAdminOnly $path
  }
  $moved = Join-Path $tagDir "URnetworkUpdate.$($helper.Id).running"
  if (-not (Test-Path $moved)) { throw "the helper's image is not at $moved" }
  if ((Get-UrFileVersion $moved) -ne $vn.file_version) { throw "$moved is $(Get-UrFileVersion $moved), not vN's helper $($vn.file_version)" }
  "OK: started by the tray app's own Update, vN's helper installed $($offered.Tag): exit 0, the report, one product at $($expect.msi_version), the MSI kept and admin-only"

  # the running app: closed by the installer, having recorded the helper
  if ($null -eq $appExited) { throw "vN's app did not exit while the helper ran: the installer did not close it" }
  if ($traits.CloseApp) {
    if ((Get-UrMsiLogLines $msiLog 'Property\(S\): URNETWORK_APP_CLOSABLE = ').Count -eq 0) { throw "URNETWORK_APP_CLOSABLE was not set: the installer did not take vN's app as closable" }
    $evidence = Get-UrCloseEvidence $msiLog
    $closed = Get-UrAppLogLines 'tray: WM_CLOSE received, exiting' $from
    if ($closed.Count -ne 1) { throw "the app logged $($closed.Count) WM_CLOSE lines, want one" }
    $closedAt = Get-UrAppLogTime $closed[0]
    "Wix4CloseApplications_X64 $($evidence.CloseStart) - $($evidence.CloseEnd); WM_CLOSE received $closedAt"
    if ($null -eq $evidence.CloseStart -or $null -eq $evidence.CloseEnd) { throw "no Wix4CloseApplications_X64 in the log" }
    if ($closedAt -lt $evidence.CloseStart -or $closedAt -gt $evidence.CloseEnd) { throw "the app's WM_CLOSE came outside Wix4CloseApplications_X64" }
    if ($evidence.ShutDownInValidate.Count -gt 0) { $evidence.ShutDownInValidate | ForEach-Object { "OBSERVED: InstallValidate's Restart Manager: $_" } }
    "OK: the release's own CloseApplication closed vN's app at $closedAt, inside its action"
  } else {
    "OBSERVED: $($offered.Tag)'s package has no CloseApplication; vN's app exited while the helper ran"
  }
  if (-not $markerSeen) { throw "the app exited for the installer without recording the helper in its update marker" }
  "OK: the app recorded the helper in its update marker as it exited"
  $rmEvents = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; StartTime = $stepStarted } -ErrorAction SilentlyContinue |
    Where-Object { $_.ProviderName -match 'RestartManager|MsiInstaller' } | Sort-Object TimeCreated)
  $rmEvents | ForEach-Object { "event $($_.ProviderName) $($_.Id) at $($_.TimeCreated.ToString('HH:mm:ss.fff')): $(($_.Message -split "`r?`n")[0])" }

  # the relaunch
  if (-not $traits.Relaunch) { throw "$($offered.Tag)'s package has no RelaunchAfterUpdate" }
  if ((Get-UrMsiLogLines $msiLog 'Action start \d{1,2}:\d{2}:\d{2}: RelaunchAfterUpdate\.').Count -eq 0) { throw "RelaunchAfterUpdate did not run" }
  $seen = Wait-UrRelaunch $from 150
  if ($seen.Processes.Count -eq 0) { throw "no URnetwork.exe --after-update was started" }
  Assert-UrRelaunchWaited $seen $helperEnded
  if ($seen.Report.Count -eq 0) { throw "the relaunched app did not show the helper's report" }
  $relaunched = $seen.Processes[0]
  "OK: $($offered.Tag)'s package started the app again (pid $($relaunched.Id), $($relaunched.CommandLine)), which waited for the helper and read its report"
  "the relaunched app (pid $($relaunched.Id)): $(Get-WildTokenText $relaunched.Id)"
  $relaunchedImage = (Get-Process -Id $relaunched.Id).Path
  if ((Get-UrFileVersion $relaunchedImage) -ne $expect.file_version) { throw "the relaunched app runs $relaunchedImage at $(Get-UrFileVersion $relaunchedImage), want $($expect.file_version)" }
  "OK: the relaunched app runs $relaunchedImage at $($expect.file_version)"

  # nothing of the update is left running
  $left = Wait-WildNoHelper 90
  if ($left.Count -ne 0) { throw "an update helper is still running: $($left -join '; ')" }
  "OK: no URnetworkUpdate.exe is left running"

  # the app after: the relaunched app's own window, as it opens
  Write-Step "3. the app after the update, as it relaunched"
  $texts = Save-WildWindowOf $relaunched.Id '3-after-relaunch' 60
  if ($null -ne $texts) { "OK: the relaunched app shows its window (captured as 3-after-relaunch.png)" }

  # and the report on its banner, in the signed-in shell of the installed release
  Write-Step "4. the installed release's banner about the update"
  try {
    Stop-UrAppProcesses
    Start-Sleep -Seconds 2
    $app = Start-UrPreviewApp ''
    Start-Sleep -Seconds 6
    Save-UrShot $app.Id '4-updated-as-opened'
    Expand-UrWindow $app.Id
    Save-UrShot $app.Id '4-updated'
    $texts = Get-UrTexts (Get-UrMainWindow $app.Id)
    $said = @($texts | Where-Object { $_ -match 'Updated to v|VPN was disconnected for the update' })
    if ($said.Count -gt 0) { "OBSERVED: the installed release's window says: $($said -join ' | ')" }
    else { "NOT VERIFIED on this run: the installed release's report banner. UI Automation reads no such text in its window." }
    if (-not (@($said | Where-Object { $_ -match ('Updated to v' + $offeredV) }).Count)) { "NOT VERIFIED on this run: a banner naming v$($offered.Version) as installed" }
  } catch {
    "NOT VERIFIED on this run: the installed release's report banner ($($_.Exception.Message))"
  }
  "PASS: the published vN updated itself to $($offered.Tag) from its own window"
} catch {
  if ($app -and -not $app.HasExited) {
    try { Save-UrShot $app.Id 'failure' } catch { }
    try { Show-UrUiTree $app.Id 200 } catch { }
  }
  throw
} finally {
  Stop-UrAppProcesses
}
