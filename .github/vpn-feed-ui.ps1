# The "ui" job of .github/workflows/vpn-feed.yml: vN's tray app, shown with
# --preview-ui on a disposable runner, against the official feed's own list
# served by the stand-in on a running clock that starts ten minutes before the
# feed's newest release counts. Everything is done to the app as a user does
# it, through its window: the developer screen's update line, the banner with
# Later, Later itself, a timed check and a check the user asks for after it, a
# restart, a click on a banner whose offer GitHub's day has overtaken, and the
# tray app's own update: its check before the click is acted on, its download,
# its hand-off to the update helper, and what the installer then does to it.
# The window is captured at each stage into %RUNNER_TEMP%\ui-shots.
#
# Reads what the job's earlier steps left in %RUNNER_TEMP%: releases.json (the
# feed's list, GitHub's bytes), boundary.json (the release that counts from
# the line), offered-before.json (the release the list offers a second before
# it), official-traits.json and vn.productcode; and msis\official.msi, the
# boundary release's own MSI.
# Pure ASCII, Windows PowerShell 5.1.
#
# SPDX-License-Identifier: MPL-2.0

. (Join-Path $PSScriptRoot 'vpn-feed-ui-lib.ps1')

$boundary = Get-Content -Raw (Join-Path $env:RUNNER_TEMP 'boundary.json') | ConvertFrom-Json
$traits = Get-Content -Raw (Join-Path $env:RUNNER_TEMP 'official-traits.json') | ConvertFrom-Json
$expect = Get-UrExpect 'official'
$vn = Get-UrExpect 'vn'
$productBefore = Get-Content (Join-Path $env:RUNNER_TEMP 'vn.productcode')
$earlierFile = Join-Path $env:RUNNER_TEMP 'offered-before.json'
$earlier = $null
if (Test-Path $earlierFile) { $earlier = Get-Content -Raw $earlierFile | ConvertFrom-Json }
if ($null -eq $earlier -or [uint64]$earlier.Code -le [uint64]$vn.code) {
  "NOT VERIFIED on this run: the tray app's window. A second before $($boundary.Tag) counts, the list offers nothing newer than vN, so there is no banner to show."
  exit 0
}
$line = [long]$boundary.CountsFrom
$earlierV = [regex]::Escape($earlier.Version)
$boundaryV = [regex]::Escape($boundary.Version)
"vN $($vn.version); before the line the list offers $($earlier.Tag); from $(Format-UrUnix $line) it offers $($boundary.Tag)"

# The banner as it must look while it offers $release.
function Assert-UrOffer($banner, $release, [string]$when) {
  Show-UrBanner $banner $when
  if (-not $banner.Open) { throw "$when`: the banner is not open" }
  if ($banner.Text -notmatch ('Update available: v' + [regex]::Escape($release.Version))) { throw "$when`: the banner does not offer v$($release.Version): $($banner.Text)" }
  if ($banner.Action -ne 'Update' -or -not $banner.ActionEnabled) { throw "$when`: the banner's action is '$($banner.Action)' (enabled $($banner.ActionEnabled)), want Update" }
  if (-not $banner.Later -or $banner.LaterName -ne 'Later') { throw "$when`: the banner does not offer Later ('$($banner.LaterName)')" }
}

$state = $null
$app = $null
try {
  Write-Step "the stand-in: GitHub's own list, on a clock that starts ten minutes before $($boundary.Tag) counts"
  $state = Set-UrFeedStandIn (Join-Path $env:RUNNER_TEMP 'releases.json') ($line - 600) $boundary (Get-UrMsiPath 'official') 'feed-standin' -Running

  # ---- 1. the developer screen's update line ---------------------------------
  Write-Step "1. the developer screen, after vN's launch check"
  $from = Get-UrAppLogLength
  $app = Start-UrPreviewApp 'developer'
  $check = Wait-UrTrayCheck $from
  if ($null -eq $check) { throw "the tray app's check of the stand-in's list failed" }
  if ($check.OwnCode -ne $vn.code) { throw "the app says its code is $($check.OwnCode), want $($vn.code)" }
  if ($check.Newest -ne $earlier.Version -or $check.Waiting -ne $boundary.Version -or $check.WaitingFrom -ne (Format-UrUnix $line)) {
    throw "the app found '$($check.Newest)' newest and holds '$($check.Waiting)' back until '$($check.WaitingFrom)', want $($earlier.Version), and $($boundary.Version) until $(Format-UrUnix $line)"
  }
  Start-Sleep -Seconds 3
  Save-UrShot $app.Id '1-developer'
  $checkLine = Get-UrCheckLine $app.Id
  "developer line: $($checkLine -replace "`r?`n", ' // ')"
  if (-not $checkLine) { Show-UrUiTree $app.Id; throw "the developer screen shows no update line" }
  if ($checkLine -notmatch ('Update found: v' + $earlierV)) { throw "the developer line does not say the update it found" }
  if ($checkLine -notmatch ('v' + $boundaryV + ' is published and is offered no earlier than \S')) { throw "the developer line does not name the release held back and its earliest time" }
  if ($checkLine -match 'is offered from') { throw "the developer line promises a time" }
  "OK: the developer line names the update found, and the release held back with the earliest time it can be offered"

  # ---- 2. the banner ----------------------------------------------------------
  Write-Step "2. the connect screen's banner"
  if (-not (Select-UrNav $app.Id @($UrIdNavConnect))) { Show-UrUiTree $app.Id; throw "the connect screen's navigation item is not in the window" }
  # first as the window opens, then tall enough to lay the whole column out
  Save-UrShot $app.Id '2-connect-as-opened'
  $asOpened = Get-UrBanner $app.Id
  Show-UrBanner $asOpened 'at the size the window opens with'
  "OBSERVED: at the size the window opens with, the update banner is within the visible part of the connect screen: $($asOpened.Open)"
  Expand-UrWindow $app.Id
  Show-UrSubtree $app.Id $UrIdBanner
  $banner = Wait-UrBanner $app.Id { param($b) $b.Open -and $b.Later } 'the offer, with Later' 30
  Assert-UrOffer $banner $earlier 'the offer'
  if ($banner.Text -match 'replaced') { throw "the banner says its release replaced another, and none did" }
  Save-UrShot $app.Id '2-offer'
  "OK: the banner offers v$($earlier.Version) with Update and Later"

  # ---- 3. Later ---------------------------------------------------------------
  Write-Step "3. Later"
  $from = Get-UrAppLogLength
  Invoke-UrId $app.Id $UrIdLater
  [void](Wait-UrAppLog ("update: Later: v" + $earlierV + " is not shown again until the next launch") $from 'that Later hid the release' 20)
  $banner = Wait-UrBanner $app.Id { param($b) -not $b.Open -and -not $b.Later -and -not $b.Action } 'no banner' 20
  Show-UrBanner $banner 'after Later'
  Save-UrShot $app.Id '3-after-later'
  $prefs = Get-UrPrefs
  "app_prefs.json after Later: $($prefs.Text)"
  $saved = @($prefs.Keys | Where-Object { $_ -match 'later|offer|hidden|skip' })
  if ($saved.Count -ne 0) { throw "Later was saved: $($saved -join ', ')" }
  "OK: Later closed the banner and saved nothing (preferences: $($prefs.Keys -join ', '))"

  # ---- 4. a timed check leaves the release hidden ---------------------------------
  # Turning "Check for updates automatically" off and on schedules a check
  # at once, by the timer and not by the user asking for one.
  Write-Step "4. a check by the timer, after Later"
  $timed = $false
  if (Select-UrNav $app.Id @($UrIdNavSettings)) {
    $window = Get-UrMainWindow $app.Id
    $toggle = $null
    foreach ($element in $window.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)) {
      try {
        $pattern = $null
        if ($element.Current.Name -match 'Check for updates automatically' -and
            $element.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$pattern)) { $toggle = $element; break }
      } catch { }
    }
    if ($toggle) {
      $from = Get-UrAppLogLength
      $pattern = $toggle.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
      "the toggle '$($toggle.Current.Name)' is $($pattern.Current.ToggleState)"
      $pattern.Toggle()
      Start-Sleep -Seconds 2
      $pattern.Toggle()
      "the toggle is $($pattern.Current.ToggleState) again"
      [void](Wait-UrAppLog 'update: automatic checking enabled' $from 'that automatic checks are on again' 20)
      [void](Wait-UrAppLog ("update: v" + $earlierV + " is offered, and Later keeps it off the banner until the next launch") $from 'that the timed check left the release hidden' 60)
      $timed = $true
    } else {
      "NOT VERIFIED on this run: a timed check after Later. UI Automation finds no toggle named 'Check for updates automatically' on the settings screen."
    }
  } else {
    "NOT VERIFIED on this run: a timed check after Later. The settings screen's navigation item is not in the window."
  }
  if (-not (Select-UrNav $app.Id @($UrIdNavConnect))) { throw "the connect screen's navigation item is not in the window" }
  $banner = Get-UrBanner $app.Id
  Show-UrBanner $banner 'after the timed check'
  if ($banner.Open -or $banner.Later) { throw "the banner is back after Later, with no check the user asked for" }
  if ($timed) { "OK: a check by the timer found v$($earlier.Version) again and left the banner closed" }

  # ---- 5. a check the user asks for shows it again ---------------------------------
  Write-Step "5. the developer screen's Check for updates, after Later"
  $asked = $false
  if (Select-UrNav $app.Id @('DeveloperNavItem') 10) {
    $from = Get-UrAppLogLength
    Invoke-UrId $app.Id $UrIdCheckNow
    $check = Wait-UrTrayCheck $from 60
    if ($null -eq $check) { throw "the check the user asked for failed" }
    if (-not (Select-UrNav $app.Id @($UrIdNavConnect))) { throw "the connect screen's navigation item is not in the window" }
    $banner = Wait-UrBanner $app.Id { param($b) $b.Open -and $b.Later } 'the offer again' 30
    Assert-UrOffer $banner $earlier 'after the check the user asked for'
    Save-UrShot $app.Id '5-after-asked-check'
    $asked = $true
    "OK: a check the user asked for put v$($earlier.Version) back on the banner"
  } else {
    Show-UrUiTree $app.Id 120
    "NOT VERIFIED on this run: a check the user asks for after Later. UI Automation finds no navigation item for the developer screen."
  }

  # ---- 6. a restart forgets Later -------------------------------------------------
  Write-Step "6. Later, then a restart"
  if ($asked) {
    $from = Get-UrAppLogLength
    Invoke-UrId $app.Id $UrIdLater
    [void](Wait-UrAppLog ("update: Later: v" + $earlierV + " is not shown again until the next launch") $from 'that Later hid the release' 20)
    [void](Wait-UrBanner $app.Id { param($b) -not $b.Open -and -not $b.Later } 'no banner' 20)
  }
  "stopping the app (pid $($app.Id)) with Later standing"
  Stop-UrAppProcesses
  $app.WaitForExit(20000) | Out-Null
  $from = Get-UrAppLogLength
  $app = Start-UrPreviewApp ''
  $check = Wait-UrTrayCheck $from
  if ($null -eq $check) { throw "the restarted app's check failed" }
  if ($check.Newest -ne $earlier.Version) { throw "the restarted app found '$($check.Newest)' newest, want $($earlier.Version)" }
  Start-Sleep -Seconds 2
  # the connect screen as a launch shows it, with no developer notice over it
  Save-UrShot $app.Id '6-after-restart-as-opened'
  $asOpened = Get-UrBanner $app.Id
  Show-UrBanner $asOpened 'after a restart, at the size the window opens with'
  "OBSERVED: at the size the window opens with, the connect screen shows the banner: $($asOpened.Open); its action without scrolling: $([bool]$asOpened.Action); Later without scrolling: $($asOpened.Later)"
  Expand-UrWindow $app.Id
  $banner = Wait-UrBanner $app.Id { param($b) $b.Open -and $b.Later } 'the offer after a restart' 30
  Assert-UrOffer $banner $earlier 'after a restart'
  Save-UrShot $app.Id '6-after-restart'
  "OK: the next launch offers v$($earlier.Version) again"

  # ---- 7. the day changes under a standing banner -------------------------------
  Write-Step "7. a click on the banner after GitHub's day has changed"
  Wait-UrStandInClock $state $line 8
  $from = Get-UrAppLogLength
  $servedBefore = @(Get-Content $state.Log | Where-Object { $_ -match '^(redirect|served): ' }).Count
  if ($servedBefore -ne 0) { throw "a download was asked for before any click" }
  Invoke-UrId $app.Id $UrIdAction
  [void](Wait-UrAppLog ("update: the offer of v" + $earlierV + " is no longer fresh; asking GitHub whether it stands") $from 'that it checks again before it acts on the click' 30)
  $check = Wait-UrTrayCheck $from 60
  if ($null -eq $check) { throw "the check before the click was acted on failed" }
  if ($check.Newest -ne $boundary.Version) { throw "the check before the click found '$($check.Newest)' newest, want $($boundary.Version)" }
  [void](Wait-UrAppLog ("update: the feed offers v" + $boundaryV + " now, not v" + $earlierV + "; nothing was started") $from 'that the click started nothing' 30)
  $banner = Wait-UrBanner $app.Id { param($b) $b.Open -and $b.Later -and $b.ActionEnabled } 'the release offered now' 30
  Assert-UrOffer $banner $boundary 'after the day changed'
  if ($banner.Text -notmatch ('This release replaced v' + $earlierV + ', which was not installed\.')) { throw "the banner does not say which release this one replaced: $($banner.Text)" }
  Save-UrShot $app.Id '7-replaced'
  Start-Sleep -Seconds 2
  $served = @(Get-Content $state.Log | Where-Object { $_ -match '^(redirect|served): ' })
  if ($served.Count -ne 0) { throw "the click on v$($earlier.Version) asked for a download: $($served -join '; ')" }
  # (Get-UrAppLogLines returns its lines as one array: counted as it is, since
  # @() around it would count 1 whatever it holds.)
  $applied = Get-UrAppLogLines 'update: applying v' $from
  if ($applied.Count -ne 0) { throw "the click on v$($earlier.Version) started an update: $($applied -join '; ')" }
  if (@(Get-Process -Name URnetworkUpdate -ErrorAction SilentlyContinue).Count -ne 0) { throw "the update helper was started" }
  $product = Assert-UrInstalled $vn
  if ($product.ProductCode -ne $productBefore) { throw "a different product is registered" }
  "OK: the click on v$($earlier.Version), made after GitHub's day changed, started nothing: the tray app checked first, and the banner offers v$($boundary.Version) and says what it replaced"

  # ---- 8. the tray app's own update ---------------------------------------------
  Write-Step "8. Update, on the release offered now"
  $stepStarted = Get-Date
  $from = Get-UrAppLogLength
  Invoke-UrId $app.Id $UrIdAction
  [void](Wait-UrAppLog ("update: applying v" + $boundaryV + " \(code " + $boundary.Code + "\)$") $from 'that it applies the release on the banner' 30)
  $again = Get-UrAppLogLines 'is no longer fresh' $from
  if ($again.Count -ne 0) { throw "a fresh offer was checked again: $($again -join '; ')" }
  # the helper, the moment the tray app starts it
  $helper = $null
  $deadline = (Get-Date).AddSeconds(180)
  while ($null -eq $helper -and (Get-Date) -lt $deadline) {
    $helper = Get-Process -Name URnetworkUpdate -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $helper) {
      # a helper that came and went between two looks, or an apply that
      # stopped before it: the app's log says which
      $over = Get-UrAppLogLines 'update: the update helper ended with|update: the update helper did not start|update: download failed|update: checksum mismatch|update: GitHub asked for no request yet' $from
      if ($over.Count -gt 0) { break }
      Start-Sleep -Milliseconds 100
    }
  }
  if ($null -eq $helper) {
    (Get-UrAppLogLines 'update: ' $from) | ForEach-Object { Write-Host "app log: $_" }
    Show-UrBanner (Get-UrBanner $app.Id) 'with no helper running'
    Save-UrShot $app.Id '8-no-helper'
    Show-UrUpdates
    throw "the tray app's update did not leave a running update helper to watch"
  }
  $null = $helper.Handle
  "the tray app started URnetworkUpdate.exe as pid $($helper.Id)"
  try { Save-UrShot $app.Id '8-installing' } catch { "screenshot 8-installing: not taken ($($_.Exception.Message))" }
  # the app is closed by the installer while the helper runs, and records the
  # helper itself as it exits
  $appExited = $null
  $markerSeen = $false
  $deadline = (Get-Date).AddSeconds(900)
  while (-not $helper.WaitForExit(100)) {
    if ($null -eq $appExited -and $app.HasExited) {
      $appExited = Get-Date
      $markerSeen = Test-Path -LiteralPath $UrMarkerPath
      "the app exited while the helper ran; its update marker is there: $markerSeen"
    }
    if ((Get-Date) -gt $deadline) { throw "the helper (pid $($helper.Id)) was still running after 900 s" }
  }
  $helper.WaitForExit()
  $helperEnded = Get-Date
  ("helper pid {0} exit code {1} (0x{1:X8})" -f $helper.Id, $helper.ExitCode)
  Show-UrUpdates
  (Get-UrAppLogLines 'update: ' $from) | ForEach-Object { Write-Host "app log: $_" }
  if ($helper.ExitCode -ne 0) { throw ("the helper exited {0} (0x{0:X8}), want 0" -f $helper.ExitCode) }
  Assert-UrResult (Read-UrResult) $boundary.Tag ([string]$boundary.Code) 0

  # what the tray app did before it handed over
  $appLines = Get-UrAppLogLines 'update: ' $from
  foreach ($want in @(("update: verified " + [regex]::Escape($boundary.Asset) + " \(" + $boundary.Digest + "\)"),
      ("update: the update helper is installing v" + $boundaryV))) {
    if (@($appLines | Where-Object { $_ -match $want }).Count -ne 1) { throw "the app's log has no single line matching $want" }
  }
  "OK: the tray app downloaded $($boundary.Asset), checked it against GitHub's SHA-256 and started the helper"
  $served = @(Get-Content $state.Log)
  foreach ($who in @(@{ Name = 'the tray app'; Agent = 'URnetwork-Windows/' }, @{ Name = 'the helper'; Agent = 'URnetwork-Windows-Update/' })) {
    foreach ($kind in 'redirect', 'served') {
      $count = @($served | Where-Object { $_ -match ("^${kind}: .* \[" + [regex]::Escape($who.Agent)) }).Count
      if ($count -ne 1) { throw "the stand-in answered $($who.Name)'s '$kind' $count times, want once" }
    }
  }
  $helperLists = @($served | Where-Object { $_ -match '^list: .* \[URnetwork-Windows-Update/' })
  if ($helperLists.Count -ne 1) { throw "the stand-in served the helper the release list $($helperLists.Count) times, want once" }
  "OK: the tray app and the helper each downloaded the release once, and the helper asked for the list itself"

  # what the helper did
  $log = Get-UrHelperLog $boundary.Tag
  Assert-UrHelperLogHas $log ("release " + [regex]::Escape($boundary.Tag) + " is offered; downloading " + [regex]::Escape($boundary.Asset) + " from release-assets\.githubusercontent\.com$") 'it took the release its own list offers'
  Assert-UrHelperLogHas $log ("SHA-256 " + $boundary.Digest + " matches GitHub's for " + [regex]::Escape($boundary.Asset)) "the download is GitHub's SHA-256"
  Assert-UrHelperLogHas $log ("package " + [regex]::Escape($boundary.Asset) + " is this product's ProductVersion " + [regex]::Escape($expect.msi_version) + "$") "the package is this product at the release code's ProductVersion"
  $tagDir = Join-Path $UrUpdatesDir $boundary.Tag
  $msiLog = Join-Path $tagDir 'install.log'
  if (-not (Test-Path $msiLog)) { throw "no install.log in $tagDir" }
  Copy-Item $msiLog (Join-Path $UrLogDir 'official-apply.log')
  $product = Assert-UrInstalled $expect 'official-apply.log'
  if ($product.ProductCode -eq $productBefore) { throw "vN's product is still registered" }
  $kept = Join-Path $tagDir $boundary.Asset
  if (-not (Test-Path $kept)) { throw "the MSI is not kept at $kept" }
  if ((Get-FileHash -Algorithm SHA256 $kept).Hash.ToLowerInvariant() -ne $boundary.Digest) { throw "the kept MSI is not GitHub's bytes" }
  foreach ($path in $UrUpdatesDir, $tagDir, $kept, $msiLog, (Join-Path $tagDir 'update-helper.log'), $UrResultPath) {
    Assert-UrAdminOnly $path
  }
  $moved = Join-Path $tagDir "URnetworkUpdate.$($helper.Id).running"
  if (-not (Test-Path $moved)) { throw "the helper's image is not at $moved" }
  "OK: started by the tray app's own Update, the helper installed $($boundary.Tag): exit 0, the report, one product at $($expect.msi_version), the MSI kept and admin-only"

  # the running app: closed by the installer, having recorded the helper
  if ($null -eq $appExited) { throw "vN's app did not exit while the helper ran: the installer did not close it" }
  if ($traits.CloseApp) {
    $evidence = Get-UrCloseEvidence $msiLog
    $closed = Get-UrAppLogLines 'tray: WM_CLOSE received, exiting' $from
    if ($closed.Count -ne 1) { throw "the app logged $($closed.Count) WM_CLOSE lines, want one" }
    $closedAt = Get-UrAppLogTime $closed[0]
    "Wix4CloseApplications_X64 $($evidence.CloseStart) - $($evidence.CloseEnd); WM_CLOSE received $closedAt"
    if ($null -eq $evidence.CloseStart -or $null -eq $evidence.CloseEnd) { throw "no Wix4CloseApplications_X64 in the log" }
    if ($closedAt -lt $evidence.CloseStart -or $closedAt -gt $evidence.CloseEnd) { throw "the app's WM_CLOSE came outside Wix4CloseApplications_X64" }
    "OK: the release's own CloseApplication closed vN's app at $closedAt, inside its action"
  } else {
    "OBSERVED: $($boundary.Tag)'s package has no CloseApplication; vN's app exited while the helper ran"
  }
  if (-not $markerSeen) { throw "the app exited for the installer without recording the helper in its update marker" }
  "OK: the app recorded the helper in its update marker as it exited"

  # the relaunch, when the release's package has one
  if ($traits.Relaunch) {
    if ((Get-UrMsiLogLines $msiLog 'Action start \d{1,2}:\d{2}:\d{2}: RelaunchAfterUpdate\.').Count -eq 0) { throw "RelaunchAfterUpdate did not run" }
    $seen = Wait-UrRelaunch $from 150
    if ($seen.Processes.Count -eq 0) { throw "no URnetwork.exe --after-update was started" }
    Assert-UrRelaunchWaited $seen $helperEnded
    if ($seen.Report.Count -eq 0) { throw "the relaunched app did not show the helper's report" }
    "OK: $($boundary.Tag)'s package relaunched the app (URnetwork.exe $(Get-UrFileVersion (Join-Path $UrInstallDir 'URnetwork.exe'))), which waited for the helper and showed its report"
  } else {
    "NOT VERIFIED on this run: the relaunch. $($boundary.Tag)'s package has no RelaunchAfterUpdate action."
  }

  # ---- 9. the release's own report, in its own window ---------------------------
  # The app that runs now is the release's own build, not this branch's: its
  # controls carry none of this branch's automation ids, so its banner is
  # captured and read by its texts.
  Write-Step "9. the installed release's banner about the update"
  try {
    Stop-UrAppProcesses
    Start-Sleep -Seconds 2
    $app = Start-UrPreviewApp ''
    Start-Sleep -Seconds 6
    Save-UrShot $app.Id '9-updated-as-opened'
    Expand-UrWindow $app.Id
    Save-UrShot $app.Id '9-updated'
    $texts = Get-UrTexts (Get-UrMainWindow $app.Id)
    $said = @($texts | Where-Object { $_ -match 'Updated to v|VPN was disconnected for the update' })
    if ($said.Count -gt 0) { "OBSERVED: the installed release's window says: $($said -join ' | ')" }
    else { "NOT VERIFIED on this run: the installed release's report banner. UI Automation reads no such text in its window." }
  } catch {
    "NOT VERIFIED on this run: the installed release's report banner ($($_.Exception.Message))"
  }
} catch {
  if ($app -and -not $app.HasExited) {
    try { Save-UrShot $app.Id 'failure' } catch { }
    try { Show-UrUiTree $app.Id 200 } catch { }
  }
  throw
} finally {
  Stop-UrAppProcesses
  Remove-UrTestNetwork $state
}
