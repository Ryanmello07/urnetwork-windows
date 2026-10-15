# PR F leg "signed-out", defect (i): is a signed-out copy offered the update?
# vN, installed as a user installs it, started as a user starts it (no
# arguments) on a fresh profile, so it opens signed out on the sign-in screen;
# its own launch check finds the real feed's newest release. Read through UI
# Automation, captured with PrintWindow, as the window opens on this runner's
# 1024x768 screen (480x728), at the default size (480x760) and at the smallest
# (400x480).
#   before (upstream main, the control): the window offers nothing; no
#     acceptance.update.* control is on the screen.
#   after (PR F): the sign-in screen's banner offers the release with Update
#     and Later, its row clear at every size; Later closes it and the sign-in
#     column is as it was.
# Throwaway (ci/prf-* only). Pure ASCII, Windows PowerShell 5.1.
#
# SPDX-License-Identifier: MPL-2.0

. (Join-Path $PSScriptRoot 'prf-lib.ps1')

$vn = Get-UrExpect 'vn'
$app = $null
try {
  Write-Step "1. vN started as a user starts it, signed out on a fresh profile; its own launch check"
  [void](Wait-UrAnonymousRequests 6 45)
  $from = Get-UrAppLogLength
  $app = Start-UrTrayApp
  $texts = Save-WildWindowOf $app.Id "$PrfMode-i-0-as-started-$PrfOs" 60
  if ($null -eq $texts) { throw "vN showed no window" }
  $check = Wait-UrTrayCheck $from
  if ($null -eq $check) { throw "vN's own launch check did not complete" }
  if ($check.OwnCode -ne [string]$vn.code) { throw "the app says its code is $($check.OwnCode), want vN's $($vn.code)" }
  if ([uint64]$check.NewestCode -le [uint64]$vn.code) { throw "the feed's newest release ($($check.Newest)) is not newer than vN" }
  $signIn = Find-UrId (Get-UrMainWindow $app.Id) 'acceptance.password.user'
  if (-not (Test-UrShown $signIn)) { throw "the window is not on the sign-in screen (acceptance.password.user is not shown)" }
  Write-Host "OK: the window is on the sign-in screen, and the app's own check found v$($check.Newest) newer than vN"

  if ($PrfMode -eq 'before') {
    Write-Step "2. the control: upstream main's sign-in screen, once the check has found the release"
    Start-Sleep -Seconds 15
    Write-Host "window: $(Get-PrfWindowSize $app.Id)"
    Save-UrShot $app.Id "$PrfMode-i-1-as-opened-$PrfOs"
    $texts = Show-PrfTexts $app.Id 'on the sign-in screen'
    $update = Get-PrfUpdateElements $app.Id
    Write-Host "acceptance.update.* elements in the window: $($update.Count)$(if ($update.Count) { ': ' + ($update -join '; ') })"
    $offered = @($texts | Where-Object { $_ -match 'Update available' })
    if ($offered.Count -ne 0) { throw "FINDING: upstream main's sign-in screen says '$($offered -join ' | ')'" }
    $shown = @($update | Where-Object { $_ -match 'offscreen=False' })
    if ($shown.Count -ne 0) { throw "FINDING: upstream main shows update controls while signed out: $($shown -join '; ')" }
    Set-PrfWindowSize $app.Id 480 760
    Save-UrShot $app.Id "$PrfMode-i-2-default-480x760-$PrfOs"
    Write-Host "REPRODUCED (upstream main): signed out, the app's own check found v$($check.Newest), and the window offers nothing: no update text and no acceptance.update.* control on the screen"
    return
  }

  Write-Step "2. the sign-in screen's banner, as the window opens on this screen"
  $banner = Wait-PrfBanner $app.Id $PrfIdsSignedOut { param($b) $b.Open -and $b.Later } 'the offer, with Later' 60
  Show-PrfBanner $banner 'as opened'
  if ($banner.Text -notmatch ('Update available: v' + [regex]::Escape($check.Newest))) { throw "the banner does not offer v$($check.Newest): $($banner.Text)" }
  if ($banner.Action -ne 'Update' -or -not $banner.ActionEnabled) { throw "the banner's action is '$($banner.Action)' (enabled $($banner.ActionEnabled)), want Update" }
  if ($banner.LaterName -ne 'Later') { throw "the banner's Later is '$($banner.LaterName)'" }
  $connect = Get-PrfBanner $app.Id $PrfIdsConnect
  if ($connect.Open) { throw "the connect screen's banner is on the screen while signed out" }
  Start-Sleep -Seconds 2
  Write-Host "window: $(Get-PrfWindowSize $app.Id)"
  Save-UrShot $app.Id "$PrfMode-i-1-as-opened-$PrfOs"
  [void](Show-PrfTexts $app.Id 'on the sign-in screen')
  $places = [ordered]@{}
  $places['as opened'] = Get-PrfPlace $app.Id $PrfIdsSignedOut 'as opened'

  Write-Step "3. the default size, 480x760, and the smallest, 400x480"
  Set-PrfWindowSize $app.Id 480 760
  Save-UrShot $app.Id "$PrfMode-i-2-default-480x760-$PrfOs"
  $places['default 480x760'] = Get-PrfPlace $app.Id $PrfIdsSignedOut 'at the default size, 480x760'
  Set-PrfWindowSize $app.Id 400 480
  Save-UrShot $app.Id "$PrfMode-i-3-minimum-400x480-$PrfOs"
  $places['minimum 400x480'] = Get-PrfPlace $app.Id $PrfIdsSignedOut 'at the smallest size, 400x480'
  Set-PrfWindowSize $app.Id 480 728
  $bad = @($places.Keys | Where-Object { -not $places[$_].Clear })
  if ($bad.Count -gt 0) { throw "the sign-in screen's banner row is not clear at: $($bad -join ', ')" }
  Write-Host "OK: the sign-in screen's banner row is clear as the window opens, at the default size and at the smallest"

  Write-Step "4. Later, pressed through UI Automation: the banner closes, and the sign-in column is whole"
  Invoke-PrfId $app.Id $PrfIdsSignedOut.Later
  Start-Sleep -Seconds 3
  $after = Get-PrfBanner $app.Id $PrfIdsSignedOut
  Show-PrfBanner $after 'after Later'
  if ($after.Open) { throw "the banner is still on the screen after Later" }
  Save-UrShot $app.Id "$PrfMode-i-4-after-later-$PrfOs"
  foreach ($id in 'acceptance.password.user', 'acceptance.password.next') {
    if (-not (Test-UrShown (Find-UrId (Get-UrMainWindow $app.Id) $id))) { throw "$id is not on the screen after Later" }
  }
  Write-Host "PASS: signed out, vN's sign-in screen offered v$($check.Newest) with Update and Later, its row clear at every size, and Later closed it"
} catch {
  if ($app -and -not $app.HasExited) {
    try { Save-UrShot $app.Id "$PrfMode-i-failure-$PrfOs" } catch { }
    try { Show-UrUiTree $app.Id 250 } catch { }
  }
  throw
} finally {
  Stop-UrAppProcesses
}
