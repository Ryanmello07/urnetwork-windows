# PR F leg "window", defect (iii): where the connect screen's update banner
# puts its Update and Later row. vN (installed, as a user installs it) in its
# signed-in shell (--preview-ui: no account signs in on a runner), offered the
# real feed's newest release by its own launch check. The row's place is read
# through UI Automation and the window captured with PrintWindow:
#   1. as the window opens on this runner's 1024x768 screen (480x728: the
#      480x760 default, cut to the work area above the taskbar);
#   2. laid out whole (480x1500), for each control's uncut height;
#   3. at the default size, 480x760;
#   4. at the smallest size the window allows, 400x480.
# 'before' (upstream main) must show the defect at 1; 'after' (PR F) must
# show the row clear at 1, 3 and 4.
# Throwaway (ci/prf-* only). Pure ASCII, Windows PowerShell 5.1.
#
# SPDX-License-Identifier: MPL-2.0

. (Join-Path $PSScriptRoot 'prf-lib.ps1')

$vn = Get-UrExpect 'vn'
$app = $null
$places = @{}
try {
  Write-Step "1. vN's signed-in shell, as it opens on this screen, offered the feed's newest release by its own launch check"
  [void](Wait-UrAnonymousRequests 6 45)
  $from = Get-UrAppLogLength
  $app = Start-UrPreviewApp ''
  $check = Wait-UrTrayCheck $from
  if ($null -eq $check) { throw "vN's own launch check did not complete" }
  if ($check.OwnCode -ne [string]$vn.code) { throw "the app says its code is $($check.OwnCode), want vN's $($vn.code)" }
  if ([uint64]$check.NewestCode -le [uint64]$vn.code) { throw "the feed's newest release ($($check.Newest)) is not newer than vN" }
  $banner = Wait-PrfBanner $app.Id $PrfIdsConnect { param($b) $b.Open -and $b.Later } 'the offer, with Later' 60
  Show-PrfBanner $banner 'as opened'
  if ($banner.Text -notmatch ('Update available: v' + [regex]::Escape($check.Newest))) { throw "the banner does not offer v$($check.Newest): $($banner.Text)" }
  Start-Sleep -Seconds 2
  Write-Host "window as opened: $(Get-PrfWindowSize $app.Id)"
  Save-UrShot $app.Id "$PrfMode-iii-1-as-opened-$PrfOs"
  $opened = Get-PrfPlace $app.Id $PrfIdsConnect 'as opened'

  Write-Step "2. laid out whole, 480x1500: each control's uncut height"
  Set-PrfWindowSize $app.Id 480 1500
  $natural = Get-PrfPlace $app.Id $PrfIdsConnect 'laid out whole'
  if (-not $natural.Clear) { throw "laid out whole, the row is not clear: $($natural.Why)" }
  Save-UrShot $app.Id "$PrfMode-iii-2-laid-out-480x1500-$PrfOs"
  # the opening size, judged against the uncut heights
  $places['as opened'] = $opened
  $opened = [pscustomobject]$opened
  $why = New-Object System.Collections.Generic.List[string]
  foreach ($part in 'Action', 'Later') {
    $rect = $opened.$part
    if ($null -ne $rect -and $rect.Height -lt $natural.$part.Height - 1) { $why.Add("$part cut to $($rect.Height) of its $($natural.$part.Height) px") }
  }
  if ($why.Count -gt 0) {
    $opened.Clear = $false
    $opened.Why = (@($opened.Why) + $why | Where-Object { $_ }) -join '; '
  }
  Write-Host "as opened, against the uncut heights: row clear: $($opened.Clear)$(if ($opened.Why) { " ($($opened.Why))" })"

  Write-Step "3. the default size, 480x760"
  Set-PrfWindowSize $app.Id 480 760
  Save-UrShot $app.Id "$PrfMode-iii-3-default-480x760-$PrfOs"
  $places['default 480x760'] = Get-PrfPlace $app.Id $PrfIdsConnect 'at the default size, 480x760' $natural

  Write-Step "4. the smallest window, 400x480"
  Set-PrfWindowSize $app.Id 400 480
  Save-UrShot $app.Id "$PrfMode-iii-4-minimum-400x480-$PrfOs"
  $places['minimum 400x480'] = Get-PrfPlace $app.Id $PrfIdsConnect 'at the smallest size, 400x480' $natural

  Write-Step "5. back to the size it opened with, 480x728"
  Set-PrfWindowSize $app.Id 480 728
  Save-UrShot $app.Id "$PrfMode-iii-5-as-opened-again-$PrfOs"
  $places['as opened, again'] = Get-PrfPlace $app.Id $PrfIdsConnect 'at 480x728 again' $natural
  $places['as opened'] = $opened

  Write-Step "verdict ($PrfMode)"
  foreach ($key in 'as opened', 'as opened, again', 'default 480x760', 'minimum 400x480') {
    $p = $places[$key]
    Write-Host ("row {0}: clear {1}{2}" -f $key, $p.Clear, $(if ($p.Why) { " ($($p.Why))" } else { '' }))
  }
  if ($PrfMode -eq 'before') {
    if ($opened.Clear) {
      Write-Host "FINDING: upstream main's row is clear as the window opens on this screen; the defect did not reproduce"
    } else {
      Write-Host "REPRODUCED (upstream main): as the window opens on a 1024x768 screen, the update banner's row is not clear: $($opened.Why)"
    }
  } else {
    $bad = @('as opened', 'as opened, again', 'default 480x760', 'minimum 400x480' | Where-Object { -not $places[$_].Clear })
    if ($bad.Count -gt 0) { throw "the update banner's row is not clear at: $($bad -join ', ')" }
    Write-Host "PASS: the update banner's Update and Later row is clear as the window opens on a 1024x768 screen, at the default size and at the smallest"
  }
} catch {
  if ($app -and -not $app.HasExited) {
    try { Save-UrShot $app.Id "$PrfMode-iii-failure-$PrfOs" } catch { }
    try { Show-UrUiTree $app.Id 250 } catch { }
  }
  throw
} finally {
  Stop-UrAppProcesses
}
