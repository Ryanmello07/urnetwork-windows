# A2c leg "channel", SPEC-A2c 8.4 legs 2, 3 and 4.
#   2. The switch, through the app (--preview-ui=developer): the helper's
#      question as the tray starts it, Cancel, then OK; what a process without
#      administrator rights can and cannot do; Settings' row.
#   3. A developer build through a stand-in for GitHub (a2c-standin.ps1): the
#      fork's own list with vNext added as an immutable -beta prerelease. The
#      tray offers it under S18 and S19; a flip of the switch while it
#      downloads is refused with S9; it installs, and the marker survives.
#   4. Back to official, from vNext: Settings' button asks nothing and removes
#      the marker while a process without administrator rights holds every
#      handle it can get and the old mutex's name; the helper refuses the
#      developer feed's next tag with the official feed's line, and passes it
#      to the download once the channel is on again; vBack, an official build
#      above vNext, waits out its day and is then offered and installs.
# A process without administrator rights is a2c-medium.ps1 under a filtered
# token (Administrators deny-only, Medium integrity). Nothing is published.
# Throwaway (ci/a2c-legs-* only). Pure ASCII, Windows PowerShell 5.1.
#
# SPDX-License-Identifier: MPL-2.0

. (Join-Path $PSScriptRoot 'a2c-lib.ps1')

$vn = Get-UrExpect 'vn'
$vnext = Get-UrExpect 'vnext'
$vback = Get-UrExpect 'vback'
$app = $null
$state = $null
$holders = New-Object System.Collections.Generic.List[string]

# Stops a filtered holder: its stop file, then what it says it released.
function Stop-A2cHolder($holder, [string]$stop) {
  if (-not $holder) { return $null }
  Set-Content -LiteralPath $stop -Value 'stop' -Encoding ascii
  return Wait-A2cMedium $holder 'released' 30
}

# Waits for a helper start whose command line matches $pattern.
function Wait-A2cHelperStart([string]$pattern, [int]$seconds = 60) {
  $deadline = (Get-Date).AddSeconds($seconds)
  do {
    $found = @(Get-A2cHelperStarts | Where-Object { $_ -match $pattern })
    if ($found.Count -gt 0) { return $found[0] }
    Start-Sleep -Milliseconds 250
  } while ((Get-Date) -lt $deadline)
  return $null
}

try {
  $uac = Get-A2cUac
  Start-A2cHelperWatch

  Write-Step "2.1 vN's Developer screen before the switch"
  $from = Get-UrAppLogLength
  $app = Start-A2cApp 'developer'
  [void](Wait-A2cTrayCheck $from)
  $facts = Wait-A2cChannel $app.Id { param($f) $f.Switch -and $f.Line } 'the channel line' 30
  Test-A2c ($facts.Line -eq $A2cS.S4) "before the switch the channel line is S4" $facts.Line
  Test-A2c ($facts.Switch -and -not $facts.SwitchOn -and $facts.SwitchEnabled) "the switch is shown, off and enabled"
  Test-A2c ($facts.SwitchName -eq 'Developer builds') "the switch is labelled Developer builds" $facts.SwitchName
  [void](Show-A2cId $app.Id $A2cIdSwitch)
  Save-UrShot $app.Id "2-1-before-$A2cOs"

  Write-Step "2.2 the helper started hidden: the question shows, or the helper ends Declined"
  $hidden = Start-Process -FilePath $UrHelperPath -ArgumentList @('--set-update-channel', 'developer') -WindowStyle Hidden -PassThru
  $null = $hidden.Handle
  $dialog = Find-A2cDialog 30
  if ($dialog) {
    Save-A2cDialogShot $dialog "2-2-hidden-start-$A2cOs"
    Test-A2c $dialog.Shown "started hidden, the helper's question shows: visible, not minimized and on the screen" $dialog.Placement
    Invoke-A2cDialogButton $dialog '2'
  }
  if ($hidden.WaitForExit(60000)) {
    Test-A2c ($hidden.ExitCode -eq $A2cRefusal.Declined -and -not (Get-A2cMarker).Exists) "the hidden helper ends Declined and makes no marker" ("exit 0x{0:X8}" -f $hidden.ExitCode)
  } else {
    Test-A2c $false "started hidden, the helper shows its question or ends Declined" "it still waits, with no question on the screen"
    Stop-Process -Id $hidden.Id -Force -ErrorAction SilentlyContinue
  }
  Start-Sleep -Seconds 2
  Clear-A2cHelperStarts

  Write-Step "2.3 the switch on, through the app: the question, then Cancel"
  Invoke-A2cToggle $app.Id
  $dialog = Find-A2cDialog 60
  if (-not $dialog) { Save-UrShot $app.Id "2-3-no-question-$A2cOs"; throw "turning the switch on started no question" }
  Save-A2cDialogShot $dialog "2-3-question-$A2cOs"
  Save-UrShot $app.Id "2-3-asking-$A2cOs"
  $helper = Get-Process -Id $dialog.ProcessId
  $null = $helper.Handle
  Test-A2c ($dialog.Process -eq 'URnetworkUpdate.exe' -and $dialog.CommandLine -match '--set-update-channel developer\s*$') "the question is the helper's, started with --set-update-channel developer" $dialog.CommandLine
  Test-A2c $dialog.Shown "it is visible, not minimized and on the screen" $dialog.Placement
  Test-A2c ($dialog.Above.Count -eq 0) "no window above it covers it" ($dialog.Above -join '; ')
  Test-A2c ($dialog.Placement -match 'topmost=True') "it is topmost" $dialog.Placement
  $text = $dialog.Texts -join ' '
  Test-A2c ($text -match [regex]::Escape("github.com/$A2cRepository")) "its text names the row's repository, github.com/$A2cRepository" $text
  Test-A2c ($text -match 'Developer builds leave out the split-tunnel driver') "it says the driver is left out"
  Test-A2c ($dialog.Default -eq 2) "Cancel is its default button" "default id $($dialog.Default)"
  $asking = Wait-A2cChannel $app.Id { param($f) $f.Line -match [regex]::Escape($A2cS.S6) } 'S6' 20
  Test-A2c ($asking.Line -match [regex]::Escape($A2cS.S6)) "while it asks, the channel line reads S6" $asking.Line
  Test-A2c (-not $asking.SwitchEnabled) "and the switch is disabled"
  $starts = Get-A2cHelperStarts
  Test-A2c ($starts.Count -eq 1) "one helper start for the toggle" ($starts -join '; ')
  Invoke-A2cDialogButton $dialog '2'
  if (-not $helper.WaitForExit(30000)) { throw "the helper did not end after Cancel" }
  Test-A2c ($helper.ExitCode -eq $A2cRefusal.Declined) "Cancel ends the helper Declined" ("exit 0x{0:X8}" -f $helper.ExitCode)
  Test-A2c (-not (Get-A2cMarker).Exists) "and leaves no marker" (Get-A2cMarker).Text
  $after = Wait-A2cChannel $app.Id { param($f) $f.Line -match [regex]::Escape($A2cS.S7) -and -not $f.SwitchOn } 'S7' 30
  Test-A2c ($after.Line -match [regex]::Escape($A2cS.S7)) "the channel line says S7" $after.Line
  Test-A2c (-not $after.SwitchOn -and $after.SwitchEnabled) "the switch is back off, and enabled"
  Start-Sleep -Seconds 8
  $starts = Get-A2cHelperStarts
  Test-A2c ($starts.Count -eq 1) "and no second helper start" ($starts -join '; ')
  Save-UrShot $app.Id "2-3-cancelled-$A2cOs"
  Clear-A2cHelperStarts

  Write-Step "2.4 on again: a process without administrator rights presses OK; then the runner's own press"
  Invoke-A2cToggle $app.Id
  $dialog = Find-A2cDialog 60
  if (-not $dialog) { throw "turning the switch on again started no question" }
  $helper = Get-Process -Id $dialog.ProcessId
  $null = $helper.Handle
  $press = Invoke-A2cMedium 'press' @($A2cCaption, '1') 90
  Start-Sleep -Seconds 3
  $still = Find-A2cDialog 2
  $made = (Get-A2cMarker).Exists
  Write-Host "after the filtered press: the question is $(if ($still) { 'still open' } else { 'gone' }); marker $((Get-A2cMarker).Text); the child says: UI Automation '$($press['uia'])', BN_CLICKED '$($press['command'])'"
  $tried = $press['dialog'] -match '^found' -and $press['done'] -eq 'yes' -and $press['uia']
  Test-A2c $tried "the Medium-integrity process ran, found the question and pressed OK" "dialog '$($press['dialog'])', done '$($press['done'])'"
  if (-not $tried) {
    Write-Host "the filtered press was not made, so it shows nothing"
  } elseif ($still -and -not $made) {
    Test-A2c $true "a Medium-integrity process's press of OK is refused: the question stays and no marker is made"
  } elseif ($uac.EnableLua -eq 0) {
    Write-Host "OBSERVED: this runner does not enforce it (EnableLUA=0): a Medium-integrity process's press went through; the case joins the look on a real desktop"
  } else {
    Test-A2c $false "a Medium-integrity process's press of OK is refused" "the question is $(if ($still) { 'open' } else { 'gone' }), marker $made, EnableLUA $($uac.EnableLua)"
  }
  if ($still) { Invoke-A2cDialogButton $still '1' }
  if (-not $helper.WaitForExit(60000)) { throw "the helper did not end after OK" }
  Test-A2c ($helper.ExitCode -eq 0) "OK ends the helper 0" ("exit 0x{0:X8}" -f $helper.ExitCode)
  Test-A2c (Test-A2cMarkerTrusted) "the marker is there, empty, with exactly the security of SPEC-A2c 2.1" (Get-A2cMarker).Text
  $on = Wait-A2cChannel $app.Id { param($f) $f.Line -eq $A2cS.S5 -and $f.SwitchOn } 'S5' 60
  Test-A2c ($on.Line -eq $A2cS.S5) "the channel line reads S5" $on.Line
  Test-A2c ($on.SwitchOn) "and the switch is on"
  $on = Wait-A2cChannel $app.Id { param($f) $f.Check -match [regex]::Escape($A2cS.S17) } 'S17' 90
  Test-A2c ($on.Check -match [regex]::Escape($A2cS.S17)) "until the fork is live, the check line reads S17" $on.Check
  Save-UrShot $app.Id "2-4-on-$A2cOs"
  Test-A2c ((Get-A2cHelperStarts).Count -eq 1) "one helper start for the second toggle" ((Get-A2cHelperStarts) -join '; ')

  Write-Step "2.5 Settings: S21 and S22"
  if (-not (Select-UrNav $app.Id @('acceptance.nav.settings'))) { throw "no Settings in the navigation" }
  Start-Sleep -Seconds 2
  [void](Show-A2cId $app.Id $A2cIdRow)
  $settings = Wait-A2cChannel $app.Id { param($f) $f.Row -and $f.Note } 'the channel row' 20
  Test-A2c ($settings.Note -eq $A2cS.S21) "Settings' channel row reads S21" $settings.Note
  Test-A2c ($settings.Official -and $settings.OfficialEnabled) "with its Switch to official releases button, enabled"
  Save-UrShot $app.Id "2-5-settings-$A2cOs"

  Write-Step "2.6 what a process without administrator rights can do to the marker and the lock"
  $probe = Invoke-A2cMedium 'probe' @("$A2cMarkerPath|$A2cLockPath") 60
  Test-A2c ($probe['token'] -match '^Medium integrity') "the child runs at Medium integrity" $probe['token']
  Test-A2c ($probe['developer-channel descriptor'] -eq $A2cMarkerSddl) "it sees the marker and reads its descriptor" $probe['developer-channel descriptor']
  foreach ($right in 'FILE_READ_DATA', 'FILE_WRITE_DATA', 'FILE_APPEND_DATA', 'DELETE', 'WRITE_DAC', 'WRITE_OWNER', 'GENERIC_READ', 'GENERIC_WRITE') {
    Test-A2c ($probe["developer-channel $right"] -match '^refused') "it cannot open the marker for $right" $probe["developer-channel $right"]
    Test-A2c ($probe["helper.lock $right"] -match '^refused') "nor the lock" $probe["helper.lock $right"]
  }

  Write-Step "3.1 the stand-in: the fork's own list with vNext ($($vnext.version)) added, immutable"
  [void](Wait-UrAnonymousRequests 4 45)
  $official = Get-A2cList $A2cOfficialListPath (Join-Path $A2cDir 'official-releases.json')
  $fork = Get-A2cList $A2cDevListPath (Join-Path $A2cDir 'developer-releases.json')
  $now = [long]$official.ServerUnix
  $vnextTag = "v$($vnext.version)"
  $vnextAsset = "URnetwork-$($vnext.version)-x64.msi"
  $devList = Add-A2cRelease $fork.File $A2cDevOwner $A2cDevRepo $vnextTag $vnextAsset (Get-UrMsiPath 'vnext') ($now - 60) $true $true (Join-Path $A2cDir 'standin-developer-1.json')
  $reading = Get-A2cDevReading $devList.Releases $now
  if (-not $reading.Offered -or $reading.Offered.Tag -ne $vnextTag) { throw "the stand-in's developer list does not offer vNext by this test's reading" }
  $routes = @((New-A2cRoute 'api.github.com' $A2cOfficialListPath 200 $official.File),
    (New-A2cRoute 'api.github.com' $A2cDevListPath 200 $devList.File)) +
    (New-A2cAssetRoutes $A2cDevOwner $A2cDevRepo $vnextTag $vnextAsset (Get-UrMsiPath 'vnext') 30)
  $state = Start-A2cStandIn $routes $now
  $stop1 = Join-Path $A2cDir 'stop-holder-1'
  $mutexHolder = Start-A2cMedium 'hold' @('', $A2cMutexName, $stop1)
  $held = Wait-A2cMedium $mutexHolder 'holding' 60
  Test-A2c ($held['mutex'] -match 'created and owned') "a Medium-integrity process holds a mutex named $A2cMutexName throughout leg 3" $held['mutex']

  Write-Step "3.2 the offer, under S18 and S19"
  Stop-UrAppProcesses
  Start-Sleep -Seconds 2
  $from = Get-UrAppLogLength
  # started at the Developer screen, which turns Advanced Mode on for the run,
  # so that screen is in the navigation while the update downloads
  $app = Start-A2cApp 'developer'
  $check = Wait-A2cTrayCheck $from
  Test-A2c ($null -ne $check -and $check.Feed -eq 'developer' -and $check.Newest -eq $vnext.version) "vN's check of the developer feed finds vNext newest" $(if ($check) { $check.Line })
  if (-not (Select-UrNav $app.Id @($UrIdNavConnect))) { throw "no connect screen in the navigation" }
  $banner = Wait-PrfBanner $app.Id $PrfIdsConnect { param($b) $b.Open -and $b.Later } 'the offer' 90
  Show-PrfBanner $banner 'the developer offer'
  Test-A2c ($banner.Text -match [regex]::Escape("$($A2cS.S18)$($vnext.version)")) "the banner's title is S18 with vNext's version" $banner.Text
  Test-A2c ($banner.Text -match [regex]::Escape($A2cS.S19)) "its message says S19 first" $banner.Text
  Test-A2c ($banner.Text -notmatch 'Update available: v') "it is not under the official title" $banner.Text
  Save-UrShot $app.Id "3-2-offer-$A2cOs"

  Write-Step "3.3 Update; a flip of the switch while it downloads is refused with S9"
  Clear-A2cHelperStarts
  $from = Get-UrAppLogLength
  Invoke-PrfId $app.Id $PrfIdsConnect.Action
  [void](Wait-UrAppLog ("update: applying v" + [regex]::Escape($vnext.version)) $from 'that it applies vNext' 60)
  $deadline = (Get-Date).AddSeconds(30)
  while ((Get-Date) -lt $deadline -and (Get-A2cStandInLines $state 'waiting \d+ s before answering release-assets').Count -eq 0) { Start-Sleep -Milliseconds 250 }
  Test-A2c ((Get-A2cStandInLines $state 'waiting \d+ s before answering release-assets').Count -ge 1) "the tray's download of vNext is under way (the stand-in holds it 30 s)"
  if (-not (Select-UrNav $app.Id @($A2cIdNavDeveloper))) { throw "no Developer screen in the navigation" }
  Start-Sleep -Seconds 1
  Invoke-A2cToggle $app.Id
  $busy = Wait-A2cChannel $app.Id { param($f) $f.Line -match [regex]::Escape($A2cS.S9) } 'S9' 15
  Test-A2c ($busy.Line -match [regex]::Escape($A2cS.S9)) "the flip while the update downloads says S9" $busy.Line
  Save-UrShot $app.Id "3-3-busy-$A2cOs"
  $channelStarts = @(Get-A2cHelperStarts | Where-Object { $_ -match 'set-update-channel' })
  Test-A2c ($channelStarts.Count -eq 0) "and starts no channel helper" ($channelStarts -join '; ')

  Write-Step "3.4 vNext installs; the marker survives"
  $start = Wait-A2cHelperStart '--apply-update' 180
  if (-not $start) { (Get-UrAppLogLines 'update: ' $from) | ForEach-Object { Write-Host "app log: $_" }; throw "the app's Update started no helper" }
  $helperId = [int]([regex]::Match($start, '^pid (\d+)').Groups[1].Value)
  $helper = Get-Process -Id $helperId -ErrorAction SilentlyContinue
  if ($helper) { $null = $helper.Handle; [void]$helper.WaitForExit(1500000) }
  $helperEnded = Get-Date
  Assert-UrResult (Read-UrResult) $vnextTag ([string]$vnext.code) 0
  $product = Assert-UrInstalled $vnext
  Assert-WildExeVersions $vnext
  Test-A2c $true "vNext is installed: $($product.ProductCode) ProductVersion $($product.Version)"
  Test-A2c (Test-A2cMarkerTrusted) "the marker has survived the upgrade" (Get-A2cMarker).Text
  $seen = Wait-UrRelaunch $from 150
  if ($seen.Processes.Count -gt 0) {
    $texts = Save-WildWindowOf $seen.Processes[0].Id "3-4-relaunched-$A2cOs" 60
    $report = Get-PrfBanner $seen.Processes[0].Id $PrfIdsSignedOut
    Show-PrfBanner $report 'the relaunched vNext'
    Test-A2c ($report.Open -and $report.Text -match [regex]::Escape("Updated to v$($vnext.version)")) "vNext starts and reports Updated to v$($vnext.version)" $report.Text
  } else {
    Test-A2c $false "the installer relaunched the app"
  }
  $released = Stop-A2cHolder $mutexHolder $stop1
  $mutexHolder = $null
  Stop-UrAppProcesses
  Start-Sleep -Seconds 2
  $from = Get-UrAppLogLength
  $app = Start-A2cApp 'developer'
  $check = Wait-A2cTrayCheck $from
  $facts = Wait-A2cChannel $app.Id { param($f) $f.Line -eq $A2cS.S5 -and $f.Check -match [regex]::Escape($A2cS.S24) } 'S5 and S24' 60
  Test-A2c ($facts.Line -eq $A2cS.S5) "vNext's channel line reads S5" $facts.Line
  Test-A2c ($facts.Check -match [regex]::Escape($A2cS.S24)) "and its check line S24: nothing on the developer feed is newer" $facts.Check
  Save-UrShot $app.Id "3-4-vnext-developer-$A2cOs"

  Write-Step "4.1 back to official from vNext: Settings' button asks nothing, while a filtered process holds what it can"
  $vnext2Code = [uint64]$vnext.code + 3000
  $vnext2Version = ($vnext.version -replace '-\d+-beta$', "-$vnext2Code-beta")
  $vnext2Tag = "v$vnext2Version"
  $devList2 = Add-A2cRelease $devList.File $A2cDevOwner $A2cDevRepo $vnext2Tag "URnetwork-$vnext2Version-x64.msi" '' ($now - 30) $true $true (Join-Path $A2cDir 'standin-developer-2.json')
  $routes = @((New-A2cRoute 'api.github.com' $A2cOfficialListPath 200 $official.File),
    (New-A2cRoute 'api.github.com' $A2cDevListPath 200 $devList2.File))
  Set-A2cRoutes $state $routes (Get-UrStandInClock $state)
  $stop2 = Join-Path $A2cDir 'stop-holder-2'
  $holder = Start-A2cMedium 'hold' @("$A2cMarkerPath|$A2cLockPath", $A2cMutexName, $stop2)
  $held = Wait-A2cMedium $holder 'holding' 60
  if (-not (Select-UrNav $app.Id @('acceptance.nav.settings'))) { throw "no Settings in the navigation" }
  Start-Sleep -Seconds 2
  Clear-A2cHelperStarts
  Invoke-A2cId $app.Id $A2cIdOfficial
  $start = Wait-A2cHelperStart '--set-update-channel official' 60
  Test-A2c ($null -ne $start) "Settings' button starts the helper with --set-update-channel official" $start
  $asked = Find-A2cDialog 8
  Test-A2c (-not $asked) "and no question is asked"
  $deadline = (Get-Date).AddSeconds(60)
  while ((Get-Date) -lt $deadline -and (Get-A2cMarker).Exists) { Start-Sleep -Milliseconds 500 }
  Test-A2c (-not (Get-A2cMarker).Exists) "the marker is gone, though a Medium-integrity process holds $($held['holding']) on it and the lock, and a mutex named $A2cMutexName" (Get-A2cMarker).Text
  $released = Stop-A2cHolder $holder $stop2
  $holder = $null
  $row = Wait-A2cChannel $app.Id { param($f) $f.Note -match [regex]::Escape($A2cS.S23) } 'S23' 60
  Test-A2c ($row.Note -match [regex]::Escape($A2cS.S23) -and $row.Note -match [regex]::Escape($A2cS.S15) -and $row.Note -match [regex]::Escape($A2cS.S13)) "Settings reads S23, S15 and S13" $row.Note
  Save-UrShot $app.Id "4-1-settings-official-$A2cOs"
  if (-not (Select-UrNav $app.Id @($A2cIdNavDeveloper))) { throw "no Developer screen in the navigation" }
  $dev = Wait-A2cChannel $app.Id { param($f) $f.Check -match [regex]::Escape($A2cS.S16) } 'S16' 120
  Test-A2c ($dev.Check -match [regex]::Escape($A2cS.S16)) "the check line reads S16" $dev.Check
  Test-A2c ($dev.Line -match [regex]::Escape($A2cS.S4) -and $dev.Line -match [regex]::Escape($A2cS.S15)) "the channel line reads S4 and S15" $dev.Line
  Save-UrShot $app.Id "4-1-developer-official-$A2cOs"

  Write-Step "4.2 the helper, asked for $vnext2Tag on the official channel, refuses it with the official feed's line"
  Stop-UrAppProcesses
  $run = Invoke-A2cHelper @('--apply-update', $vnext2Tag) '' 300
  $log = Get-A2cHelperLog $vnext2Tag
  Test-A2c ($run.ExitCode -eq $A2cRefusal.NotOffered) "it ends NotOffered" ("exit 0x{0:X8}" -f $run.ExitCode)
  Test-A2c (@($log | Where-Object { $_ -match "refused: the official feed offers .*, not $([regex]::Escape($vnext2Tag)) above" }).Count -eq 1) "with the official feed's line" (($log | Where-Object { $_ -match 'refused' }) -join ' | ')
  Test-A2c ((Get-A2cStandInLines $state ([regex]::Escape("/releases/download/$vnext2Tag/"))).Count -eq 0) "and downloads nothing"
  Write-Step "4.3 control: with the channel on again, the same tag passes the offer and ends at the download"
  $set = Invoke-A2cHelper @('--set-update-channel', 'developer') 'ok' 120
  Test-A2c ($set.ExitCode -eq 0 -and (Test-A2cMarkerTrusted)) "the channel is on again" ("exit 0x{0:X8}" -f $set.ExitCode)
  $run = Invoke-A2cHelper @('--apply-update', $vnext2Tag) '' 300
  $log = Get-A2cHelperLog $vnext2Tag
  Test-A2c ($run.ExitCode -eq $A2cRefusal.Download) "it passes the offer and ends at the download (Refusal::Download)" ("exit 0x{0:X8}: $(($log | Where-Object { $_ -match 'refused' }) -join ' | ')" -f $run.ExitCode)
  Test-A2c ((Get-A2cStandInLines $state ([regex]::Escape("/releases/download/$vnext2Tag/"))).Count -ge 1) "it asked the stand-in for $vnext2Tag's MSI"
  $back = Invoke-A2cHelper @('--set-update-channel', 'official') '' 60
  Test-A2c ($back.ExitCode -eq 0 -and -not (Get-A2cMarker).Exists) "and back to official"

  Write-Step "4.4 vBack ($($vback.version)) listed as an official release: inside its day it waits, then it is offered and installs"
  $vbackTag = "v$($vback.version)"
  $vbackAsset = "URnetwork-$($vback.version)-x64.msi"
  $clock = Get-UrStandInClock $state
  $officialList = Add-A2cRelease $official.File $UrOfficialOwner $UrOfficialRepo $vbackTag $vbackAsset (Get-UrMsiPath 'vback') ($clock - 60) $false $true (Join-Path $A2cDir 'standin-official-vback.json')
  $routes = @((New-A2cRoute 'api.github.com' $A2cOfficialListPath 200 $officialList.File),
    (New-A2cRoute 'api.github.com' $A2cDevListPath 200 $devList2.File)) +
    (New-A2cAssetRoutes $UrOfficialOwner $UrOfficialRepo $vbackTag $vbackAsset (Get-UrMsiPath 'vback') 0)
  Set-A2cRoutes $state $routes $clock
  $from = Get-UrAppLogLength
  $app = Start-A2cApp 'developer'
  $check = Wait-A2cTrayCheck $from
  Test-A2c ($null -ne $check -and $check.Feed -eq 'official' -and $check.Waiting -eq $vback.version) "vNext's check of the official feed holds vBack back for its day" $(if ($check) { $check.Line })
  $wait = Wait-A2cChannel $app.Id { param($f) $f.Check -match [regex]::Escape($A2cS.S16) -and $f.Check -match [regex]::Escape($vback.version) } 'S16 and the waiting line' 60
  Test-A2c ($wait.Check -match [regex]::Escape($A2cS.S16) -and $wait.Check -match [regex]::Escape($vback.version)) "the check line reads S16 and names vBack waiting" $wait.Check
  Save-UrShot $app.Id "4-4-waiting-$A2cOs"
  # two days on: vBack has been out, unchanged, for a day when the stand-in's day began
  Set-A2cRoutes $state $routes ($clock + 2 * 86400)
  $from = Get-UrAppLogLength
  Invoke-A2cId $app.Id $UrIdCheckNow
  $check = Wait-A2cTrayCheck $from
  Test-A2c ($null -ne $check -and $check.Newest -eq $vback.version) "two days on, the check finds vBack newest" $(if ($check) { $check.Line })
  if (-not (Select-UrNav $app.Id @($UrIdNavConnect))) { throw "no connect screen in the navigation" }
  $banner = Wait-PrfBanner $app.Id $PrfIdsConnect { param($b) $b.Open -and $b.Later } 'the offer of vBack' 60
  Show-PrfBanner $banner 'the official offer'
  Test-A2c ($banner.Text -match [regex]::Escape("Update available: v$($vback.version)") -and $banner.Text -notmatch [regex]::Escape($A2cS.S19)) "vBack is offered under the official title, without S19" $banner.Text
  Save-UrShot $app.Id "4-4-offer-$A2cOs"
  Clear-A2cHelperStarts
  $from = Get-UrAppLogLength
  Invoke-PrfId $app.Id $PrfIdsConnect.Action
  $start = Wait-A2cHelperStart '--apply-update' 300
  if (-not $start) { (Get-UrAppLogLines 'update: ' $from) | ForEach-Object { Write-Host "app log: $_" }; throw "the app's Update of vBack started no helper" }
  $helper = Get-Process -Id ([int]([regex]::Match($start, '^pid (\d+)').Groups[1].Value)) -ErrorAction SilentlyContinue
  if ($helper) { $null = $helper.Handle; [void]$helper.WaitForExit(1500000) }
  Assert-UrResult (Read-UrResult) $vbackTag ([string]$vback.code) 0
  $product = Assert-UrInstalled $vback
  Test-A2c $true "vBack is installed: $($product.ProductCode) ProductVersion $($product.Version)"
  Test-A2c (-not (Get-A2cMarker).Exists) "and the PC is on official releases"
  $seen = Wait-UrRelaunch $from 150
  if ($seen.Processes.Count -gt 0) { [void](Save-WildWindowOf $seen.Processes[0].Id "4-4-relaunched-$A2cOs" 60) }
  Complete-A2cLeg 'channel'
} catch {
  if ($app -and -not $app.HasExited) {
    try { Save-UrShot $app.Id "channel-failure-$A2cOs" } catch { }
    try { Show-UrUiTree $app.Id 300 } catch { }
  }
  foreach ($line in $A2cFailures) { Write-Host "FAILED so far: $line" }
  throw
} finally {
  foreach ($stop in (Join-Path $A2cDir 'stop-holder-1'), (Join-Path $A2cDir 'stop-holder-2')) { Set-Content -LiteralPath $stop -Value 'stop' -Encoding ascii }
  Stop-UrAppProcesses
  Remove-UrTestNetwork $state
}
