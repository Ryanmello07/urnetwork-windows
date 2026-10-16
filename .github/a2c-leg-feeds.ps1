# A2c leg "feeds", SPEC-A2c 8.4 legs 1, 5 and 7: both feeds as GitHub lists
# them, read by this test's own rules (vpn-feed-lib.ps1 Get-UrFeedReading for
# the official feed, a2c-lib.ps1 Get-A2cDevReading for the developer feed),
# and vN's tray and vN's helper on each channel against those readings. The
# developer channel is set by the helper's own verb, its question answered
# through UI Automation (the switch through the app is leg "channel"). Until
# the fork turns on immutable releases, the developer answer is nothing: not
# immutable. And the arguments the helper refuses. Nothing is published: the
# feeds are GitHub's own, read without a token.
# Throwaway (ci/a2c-legs-* only). Pure ASCII, Windows PowerShell 5.1.
#
# SPDX-License-Identifier: MPL-2.0

. (Join-Path $PSScriptRoot 'a2c-lib.ps1')

$vn = Get-UrExpect 'vn'
$app = $null
try {
  Write-Step "1. both feeds as GitHub lists them, by this test's own rules"
  [void](Wait-UrAnonymousRequests 12 45)
  $official = Get-UrFeedNow (Join-Path $env:RUNNER_TEMP 'releases.json')
  $officialReading = Get-UrFeedReading $official.Releases $official.ServerUnix
  $dev = Get-A2cList $A2cDevListPath (Join-Path $A2cDir 'developer-releases.json')
  $devReading = Get-A2cDevReading $dev.Releases $dev.ServerUnix
  if (-not $officialReading.Offered) { throw "the official feed offers nothing today" }
  $live = $null -ne $devReading.Offered
  Write-Host "the developer feed is $(if ($live) { "LIVE: it offers $($devReading.Offered.Tag)" } else { 'not live: nothing there counts' })"
  # the fork's newest -beta tag, by code, for the helper to be asked about
  $betas = @($devReading.Rows | Where-Object { $_.Tag -match '^v\d{4}\.\d{1,2}\.\d{1,2}-(\d{1,18})-beta$' } |
    Sort-Object { [uint64]([regex]::Match($_.Tag, '-(\d{1,18})-beta$').Groups[1].Value) } -Descending)
  if ($betas.Count -eq 0) { throw "the developer feed lists no -beta tag" }
  $devTag = $betas[0].Tag
  Write-Host "the developer tag the helper is asked about: $devTag ($($betas[0].Why))"
  if (-not $live) {
    $notImmutable = @($devReading.Rows | Where-Object { $_.Tag.EndsWith('-beta') -and $_.Why -eq 'not immutable' })
    Test-A2c ($notImmutable.Count -eq $betas.Count) "every -beta release the fork lists is skipped as not immutable ($($notImmutable.Count) of $($betas.Count))"
  }

  Write-Step "2. the arguments the helper refuses, with no marker (leg 7)"
  foreach ($arguments in @(@('--set-update-channel', 'beta'), @('--set-update-channel', $A2cRepository),
      @('--set-update-channel', $A2cDevRepoId), @('--set-update-channel', 'developer', 'extra'),
      @('--set-update-channel', 'Developer'), @('--apply-update', $devTag, 'developer'))) {
    $run = Invoke-A2cHelper $arguments '' 60
    $asked = Find-A2cDialog 1
    Test-A2c ($run.ExitCode -eq $A2cRefusal.BadArguments -and -not $asked -and -not (Get-A2cMarker).Exists) `
      "'$($arguments -join ' ')' ends BadArguments, asks nothing and makes no marker" ("exit 0x{0:X8}" -f $run.ExitCode)
  }
  $filtered = Invoke-A2cFilteredExe $UrHelperPath '--set-update-channel developer' 60
  Test-A2c ($filtered -eq $A2cRefusal.NotElevated -and -not (Get-A2cMarker).Exists) "started with a filtered token, the channel verb ends NotElevated" ("exit 0x{0:X8}" -f $filtered)
  $outside = New-Item -ItemType Directory -Force (Join-Path $A2cDir 'outside-program-files')
  Copy-Item -LiteralPath $UrHelperPath -Destination $outside
  $copy = Join-Path $outside 'URnetworkUpdate.exe'
  $run = Start-Process -FilePath $copy -ArgumentList @('--set-update-channel', 'developer') -PassThru -Wait
  Test-A2c ($run.ExitCode -eq $A2cRefusal.NotInstalled -and -not (Get-A2cMarker).Exists) "a copy outside Program Files ends NotInstalled" ("exit 0x{0:X8}" -f $run.ExitCode)

  Write-Step "3. vN on the official channel: its tray and its helper agree with this test's reading"
  $from = Get-UrAppLogLength
  $app = Start-A2cApp 'developer'
  $check = Wait-A2cTrayCheck $from
  Test-A2c ($null -ne $check -and $check.Feed -eq 'official') "vN's tray checks the official feed" $(if ($check) { $check.Line })
  if ($check) {
    Test-A2c ($check.OwnCode -eq [string]$vn.code) "its own code is vN's" $check.OwnCode
    Test-A2c ($check.NewestCode -eq [string]$officialReading.Offered.Code) "it names newest the release this test's reading offers, $($officialReading.Offered.Tag)" "$($check.Newest) (code $($check.NewestCode))"
  }
  $facts = Wait-A2cChannel $app.Id { param($f) $f.Switch -and $f.Line } 'the channel line' 30
  Test-A2c ($facts.Line -eq $A2cS.S4) "the Developer screen's channel line is S4" $facts.Line
  Test-A2c ($facts.Switch -and -not $facts.SwitchOn -and $facts.SwitchEnabled) "the switch is shown, off and enabled"
  Save-UrShot $app.Id "1-official-developer-screen-$A2cOs"
  Stop-UrAppProcesses
  $app = $null
  $run = Invoke-A2cHelper @('--apply-update', $devTag) '' 300
  $log = Get-A2cHelperLog $devTag
  $want = "the official feed offers $($officialReading.Offered.Tag) (code $($officialReading.Offered.Code)), not $devTag above this build's $($vn.code)"
  Test-A2c ($run.ExitCode -eq $A2cRefusal.NotOffered) "vN's helper, on the official channel, refuses $devTag NotOffered" ("exit 0x{0:X8}" -f $run.ExitCode)
  Test-A2c (@($log | Where-Object { $_.Contains($want) }).Count -eq 1) "with the official feed's line, which names the release this test's reading offers" (($log | Where-Object { $_ -match 'refused' }) -join ' | ')

  Write-Step "4. the developer channel, by the helper's verb, its question answered OK"
  $set = Invoke-A2cHelper @('--set-update-channel', 'developer') 'ok' 120 "1-question-$A2cOs"
  Test-A2c ($set.ExitCode -eq 0) "the channel verb ends 0 after OK" ("exit 0x{0:X8}" -f $set.ExitCode)
  if ($set.Dialog) {
    Test-A2c (($set.Dialog.Texts -join ' ') -match [regex]::Escape("github.com/$A2cRepository")) "its question names github.com/$A2cRepository"
    Test-A2c ($set.Dialog.Default -eq 2) "its default button is Cancel" "default id $($set.Dialog.Default)"
  }
  Test-A2c (Test-A2cMarkerTrusted) "the marker is there, empty, with exactly the security of SPEC-A2c 2.1" (Get-A2cMarker).Text
  $from = Get-UrAppLogLength
  $app = Start-A2cApp 'developer'
  $check = Wait-A2cTrayCheck $from
  Test-A2c ($null -ne $check -and $check.Feed -eq 'developer') "vN's tray checks the developer feed" $(if ($check) { $check.Line })
  if ($check -and -not $live) {
    Test-A2c ($check.NewestCode -eq '0' -and $check.Newest -eq 'none') "and names nothing newest, as this test's reading of the fork's list gives" "$($check.Newest) (code $($check.NewestCode))"
  } elseif ($check) {
    Test-A2c ($check.NewestCode -eq [string]$devReading.Offered.Code) "and names newest what this test's reading offers, $($devReading.Offered.Tag)" "$($check.Newest) (code $($check.NewestCode))"
  }
  $facts = Wait-A2cChannel $app.Id { param($f) $f.Line -eq $A2cS.S5 -and $f.Check } 'S5' 40
  Test-A2c ($facts.Line -eq $A2cS.S5) "the Developer screen's channel line is S5" $facts.Line
  Test-A2c ($facts.SwitchOn) "the switch is on"
  if (-not $live) { Test-A2c ($facts.Check -match [regex]::Escape($A2cS.S17)) "the check line reads S17, nothing listed that this PC can install" $facts.Check }
  Save-UrShot $app.Id "1-developer-developer-screen-$A2cOs"
  Stop-UrAppProcesses
  $app = $null
  $run = Invoke-A2cHelper @('--apply-update', $devTag) '' 600
  $log = Get-A2cHelperLog $devTag
  if (-not $live) {
    Test-A2c ($run.ExitCode -eq $A2cRefusal.NotOffered) "vN's helper, on the developer channel, refuses $devTag NotOffered (leg 5, before the fork is live)" ("exit 0x{0:X8}" -f $run.ExitCode)
    Test-A2c (@($log | Where-Object { $_.Contains("the developer feed offers nothing (code 0), not $devTag above this build's $($vn.code)") }).Count -eq 1) "with the developer feed's line: it offers nothing" (($log | Where-Object { $_ -match 'refused' }) -join ' | ')
    Test-A2c (@($log | Where-Object { $_ -match "release $([regex]::Escape($devTag)) is not an immutable release: skipped" }).Count -ge 1) "and the helper skipped $devTag as not immutable"
  } else {
    Write-Host "OBSERVED: the fork is live; vN's helper was asked for $devTag (exit 0x$('{0:X8}' -f $run.ExitCode))"
    $log | ForEach-Object { Write-Host "helper log: $_" }
  }

  Write-Step "5. back to official: the verb asks nothing"
  Clear-A2cHelperStarts
  $back = Invoke-A2cHelper @('--set-update-channel', 'official') '' 60
  Test-A2c ($back.ExitCode -eq 0 -and -not (Find-A2cDialog 1) -and -not (Get-A2cMarker).Exists) "the official channel's verb ends 0, asks nothing and removes the marker" ("exit 0x{0:X8}; $((Get-A2cMarker).Text)" -f $back.ExitCode)
  Complete-A2cLeg 'feeds'
} catch {
  if ($app -and -not $app.HasExited) {
    try { Save-UrShot $app.Id "feeds-failure-$A2cOs" } catch { }
    try { Show-UrUiTree $app.Id 250 } catch { }
  }
  throw
} finally {
  Stop-UrAppProcesses
}
