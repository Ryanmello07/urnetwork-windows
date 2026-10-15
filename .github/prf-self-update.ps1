# PR F leg "self-update", defect (ii): what the report says after a
# self-update applied with no VPN connected (no account signs in on a runner,
# so no VPN can be). vN updates itself to vNext, a build of the same source at
# a newer code, through the app's own banner, its own download and check, the
# update helper, the install, the close and the relaunch. GitHub is the
# stand-in on this runner (vpn-feed-standin.ps1): the feed's own list, as
# GitHub returns it now, with vNext put in front as a release that has been
# out for a day (prf-lib.ps1 New-PrfStandInList), and vNext's MSI at its URL.
# Nothing is published.
#   before (upstream main, the control): the update is started from the
#     connect screen's banner (--preview-ui), as #8's runner did; the
#     relaunched app is signed out and shows no banner, so the report is read
#     on the connect screen of the next start (--preview-ui): it carries the
#     reconnect line, which is false here.
#   after (PR F): the update is started from the sign-in screen's banner,
#     signed out (defect i's banner, end to end); the app records, as the
#     installer closes it, that no VPN was connected; the relaunched app reads
#     that record and shows the report on the sign-in screen without the line.
#     The next start has no record left (the relaunch read it), so it does not
#     know whether a VPN was connected, and keeps the line (the lead's G8:
#     what is not known says it).
# Throwaway (ci/prf-* only). Pure ASCII, Windows PowerShell 5.1.
#
# SPDX-License-Identifier: MPL-2.0

. (Join-Path $PSScriptRoot 'prf-lib.ps1')

$vn = Get-UrExpect 'vn'
$vnext = Get-UrExpect 'vnext'
$vnextV = [regex]::Escape([string]$vnext.version)
$app = $null
$state = $null
try {
  Write-Step "0. the stand-in: GitHub's own list now, with vNext ($($vnext.version)) in front, out for a day"
  [void](Wait-UrAnonymousRequests 4 45)
  $list = Get-UrFeedList (Join-Path $env:RUNNER_TEMP 'releases.json')
  $now = [long]$list.ServerUnix
  $dayStart = $now - ($now % $UrFeedDaySeconds)
  # out, unchanged, since a minute before the previous UTC day began: it has
  # been out for a day when today began (ReleaseSelection.h HasSoaked), and it
  # is not more than 48 h older than its own code
  $published = $dayStart - $UrFeedDaySeconds - 60
  $standinList = Join-Path $env:RUNNER_TEMP 'standin-releases.json'
  $reading = New-PrfStandInList $list.File $vnext (Get-UrMsiPath 'vnext') $published $now $standinList
  if (-not $reading.Offered -or $reading.Offered.Tag -ne "v$($vnext.version)") { throw "the stand-in's list does not offer vNext by this test's reading" }
  if ([uint64]$reading.Offered.Code -le [uint64]$vn.code) { throw "vNext is not newer than vN" }
  $state = Set-UrFeedStandIn $standinList $now $reading.Offered (Get-UrMsiPath 'vnext') -Running

  $ids = $PrfIdsSignedOut
  Write-Step "1. vN's own launch check, and its banner ($PrfMode)"
  $from = Get-UrAppLogLength
  if ($PrfMode -eq 'after') {
    $app = Start-UrTrayApp
    [void](Save-WildWindowOf $app.Id "$PrfMode-ii-0-as-started-$PrfOs" 60)
  } else {
    $ids = $PrfIdsConnect
    $app = Start-UrPreviewApp ''
  }
  $check = Wait-UrTrayCheck $from
  if ($null -eq $check) { throw "vN's own launch check of the stand-in's list failed" }
  if ($check.Newest -ne $vnext.version) { throw "vN's check found $($check.Newest) newest, want vNext $($vnext.version)" }
  if ($PrfMode -eq 'before') { Expand-UrWindow $app.Id 1500 }
  $banner = Wait-PrfBanner $app.Id $ids { param($b) $b.Open -and $b.Later } 'the offer, with Later' 60
  Show-PrfBanner $banner 'before the click'
  if ($banner.Text -notmatch ('Update available: v' + $vnextV)) { throw "the banner does not offer vNext: $($banner.Text)" }
  Save-UrShot $app.Id "$PrfMode-ii-1-offer-$PrfOs"

  Write-Step "2. Update, invoked as a click does; the helper, the install, the close and the relaunch"
  $stepStarted = Get-Date
  $from = Get-UrAppLogLength
  Invoke-PrfId $app.Id $ids.Action
  [void](Wait-UrAppLog ("update: applying v" + $vnextV + " \(code " + $vnext.code + "\)$") $from 'that it applies vNext' 60)
  $helper = $null
  $deadline = (Get-Date).AddSeconds(900)
  while ($null -eq $helper -and (Get-Date) -lt $deadline) {
    $helper = Get-Process -Name URnetworkUpdate -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $helper) {
      $over = Get-UrAppLogLines 'update: the update helper ended with|update: the update helper did not start|update: download failed|update: checksum mismatch|nothing was started' $from
      if ($over.Count -gt 0) { break }
      Start-Sleep -Milliseconds 100
    }
  }
  if ($null -eq $helper) {
    (Get-UrAppLogLines 'update: ' $from) | ForEach-Object { Write-Host "app log: $_" }
    Save-UrShot $app.Id "$PrfMode-ii-no-helper-$PrfOs"
    throw "the app's Update did not leave a running update helper"
  }
  $null = $helper.Handle
  Write-Host "the app started URnetworkUpdate.exe as pid $($helper.Id) after $([int]((Get-Date) - $stepStarted).TotalSeconds) s"
  # the record the app leaves as the installer closes it (UpdateMarker.h)
  $marker = ''
  $appExited = $null
  while (-not $helper.WaitForExit(100)) {
    if ($null -eq $appExited -and $app.HasExited) { $appExited = Get-Date }
    if (-not $marker -and (Test-Path -LiteralPath $UrMarkerPath)) {
      try { $marker = ([IO.File]::ReadAllText($UrMarkerPath)).Trim() } catch { }
    }
    if ((Get-Date) -gt $stepStarted.AddSeconds(1500)) { throw "the helper (pid $($helper.Id)) was still running after 1500 s" }
  }
  $helper.WaitForExit()
  $helperEnded = Get-Date
  if (-not $marker -and (Test-Path -LiteralPath $UrMarkerPath)) { try { $marker = ([IO.File]::ReadAllText($UrMarkerPath)).Trim() } catch { } }
  ("helper pid {0} exit code {1} (0x{1:X8})" -f $helper.Id, $helper.ExitCode)
  Write-Host "the app exited while the helper ran: $([bool]$appExited); the update-in-progress record it left: '$marker'"
  (Get-UrAppLogLines 'update: ' $from) | ForEach-Object { Write-Host "app log: $_" }
  if ($helper.ExitCode -ne 0) { Show-UrUpdates; throw ("the helper exited {0} (0x{0:X8}), want 0" -f $helper.ExitCode) }
  Assert-UrResult (Read-UrResult) "v$($vnext.version)" ([string]$vnext.code) 0
  $product = Assert-UrInstalled $vnext
  Assert-WildExeVersions $vnext
  Write-Host "OK: vNext is installed: $($product.ProductCode) ProductVersion $($product.Version)"
  if (-not $marker) { throw "no update-in-progress record was seen while the helper ran" }
  $fields = @($marker -split ' ')
  Write-Host "the record has $($fields.Count) fields: $($fields -join ', ')"
  if ($PrfMode -eq 'after') {
    if ($fields.Count -ne 4 -or $fields[3] -ne '0') { throw "the record does not say that no VPN was connected: '$marker'" }
    Write-Host "OK: the app recorded, as the installer closed it, that no VPN was connected (fourth field 0)"
  }
  $seen = Wait-UrRelaunch $from 150
  if ($seen.Processes.Count -eq 0) { throw "no URnetwork.exe --after-update was started" }
  Assert-UrRelaunchWaited $seen $helperEnded
  if ($seen.Report.Count -eq 0) { throw "the relaunched app did not read the helper's report" }
  $relaunched = $seen.Processes[0]
  (Get-UrAppLogLines 'update: ' $from) | Select-Object -Last 12 | ForEach-Object { Write-Host "app log: $_" }

  Write-Step "3. the relaunched app's own window, as it opens (signed out)"
  $texts = Save-WildWindowOf $relaunched.Id "$PrfMode-ii-2-relaunched-$PrfOs" 60
  if ($null -eq $texts) { throw "the relaunched app showed no window" }
  $report = Get-PrfBanner $relaunched.Id $PrfIdsSignedOut
  Show-PrfBanner $report 'in the relaunched app'
  $verdict = [ordered]@{}
  if ($PrfMode -eq 'after') {
    if (-not $report.Open) { throw "the relaunched app's sign-in screen shows no report" }
    if ($report.Text -notmatch ('Updated to v' + $vnextV)) { throw "the sign-in screen's report does not name vNext: $($report.Text)" }
    $verdict['relaunched, sign-in screen'] = ($report.Text -match [regex]::Escape($PrfReconnect))
  } else {
    $said = @($texts | Where-Object { $_ -match 'Updated to v|VPN was disconnected for the update' })
    Write-Host "the relaunched app's sign-in screen says about the update: $(if ($said.Count) { $said -join ' | ' } else { 'nothing' })"
  }

  Write-Step "4. the connect screen at the next start (--preview-ui)"
  Stop-UrAppProcesses
  Start-Sleep -Seconds 2
  $app = Start-UrPreviewApp ''
  Start-Sleep -Seconds 6
  Expand-UrWindow $app.Id 1500
  $connect = Wait-PrfBanner $app.Id $PrfIdsConnect { param($b) $b.Open } 'the report' 30
  Show-PrfBanner $connect 'at the next start'
  Save-UrShot $app.Id "$PrfMode-ii-3-next-start-connect-$PrfOs"
  if ($connect.Text -notmatch ('Updated to v' + $vnextV)) { throw "the connect screen's report does not name vNext: $($connect.Text)" }
  $verdict['next start, connect screen'] = ($connect.Text -match [regex]::Escape($PrfReconnect))
  # what each launch made of the record, in its own words
  (Get-UrAppLogLines 'the line about the VPN' 0) | ForEach-Object { Write-Host "app log: $_" }

  Write-Step "verdict ($PrfMode): no VPN was connected, so the reconnect line is false"
  foreach ($key in $verdict.Keys) { Write-Host "the reconnect line on $key`: $(if ($verdict[$key]) { 'SHOWN' } else { 'not shown' })" }
  if ($PrfMode -eq 'before') {
    if (-not $verdict['next start, connect screen']) { throw "FINDING: upstream main's report does not carry the reconnect line; the control did not reproduce the defect" }
    Write-Host "REPRODUCED (upstream main): after a self-update with no VPN connected, the report says '$PrfReconnect'"
  } else {
    if ($verdict['relaunched, sign-in screen']) {
      throw "the relaunched app shows the reconnect line, though the record of its update says no VPN was connected"
    }
    if (-not $verdict['next start, connect screen']) {
      throw "the next start shows no reconnect line, though it has no record of the update left: what is not known must keep the line (G8)"
    }
    Write-Host "PASS: after a self-update started from the sign-in screen with no VPN connected, the relaunched app, which read the record of its update, says nothing about reconnecting; the next start, with no record left, keeps the line (G8)"
  }
} catch {
  if ($app -and -not $app.HasExited) {
    try { Save-UrShot $app.Id "$PrfMode-ii-failure-$PrfOs" } catch { }
    try { Show-UrUiTree $app.Id 250 } catch { }
  }
  throw
} finally {
  Stop-UrAppProcesses
  Remove-UrTestNetwork $state
}
