# A2c leg "markers", SPEC-A2c 8.4 legs 6 and 8.
#   6. Markers that must not be trusted, written by the elevated test, since
#      nothing else could. For each: both sides say official (the Developer
#      screen's S14 and its button; the helper refuses a developer tag above
#      vN with the official feed's line), and the button removes what is
#      there, after which the switch turns the channel on. The control, in the
#      same leg: under a trusted marker the same tag passes the offer and ends
#      at the download. Then an updates\ that is a junction, its target and
#      the marker in it with the helper's own security, so the junction alone
#      is what is not trusted: the tray says what the official channel says
#      (S4, and no button, since switching to official cannot remove what lies
#      through a junction: the lead's G17), its log names the reason, and both
#      verbs end in Staging and write nothing.
#   8. Uninstalling removes the marker and the lock; the real, older official
#      MSI then installs, on the official channel.
# The developer tag is listed only, by a stand-in for GitHub (a2c-standin.ps1)
# serving the fork's own list with it added; its MSI is not served, so a
# helper that passes the offer ends at the download. Nothing is published.
# Throwaway (ci/a2c-legs-* only). Pure ASCII, Windows PowerShell 5.1.
#
# SPDX-License-Identifier: MPL-2.0

. (Join-Path $PSScriptRoot 'a2c-lib.ps1')

$vn = Get-UrExpect 'vn'
$app = $null
$state = $null
$junction = $false
$saved = Join-Path 'C:\Program Files\URnetwork' 'updates-a2c-saved'
try {
  Write-Step "0. the stand-in: the fork's own list with a developer tag above vN, listed only"
  [void](Wait-UrAnonymousRequests 6 45)
  $official = Get-UrFeedNow (Join-Path $env:RUNNER_TEMP 'releases.json')
  $officialReading = Get-UrFeedReading $official.Releases $official.ServerUnix
  $fork = Get-A2cList $A2cDevListPath (Join-Path $A2cDir 'developer-releases.json')
  $now = [long]$official.ServerUnix
  # the real official MSI for leg 8, from GitHub itself, before the stand-in
  # takes GitHub's names
  # not under the logs' folder: the logs are uploaded
  $officialMsi = Join-Path (New-Item -ItemType Directory -Force (Join-Path $env:RUNNER_TEMP 'a2c-official')) "official-$($officialReading.Offered.Version)-x64.msi"
  [void](Save-UrReleaseMsi $officialReading.Offered $officialMsi)
  $devCode = [uint64][math]::Floor($now - $UrFoundedUnix) * 10 - 600
  $instant = [DateTimeOffset]::FromUnixTimeSeconds($UrFoundedUnix + [long][math]::Floor([decimal]$devCode / 10)).UtcDateTime
  $devVersion = '{0}.{1}.{2}-{3}-beta' -f $instant.Year, $instant.Month, $instant.Day, $devCode
  $devTag = "v$devVersion"
  if ($devCode -le [uint64]$vn.code) { throw "the developer tag is not above vN" }
  $devList = Add-A2cRelease $fork.File $A2cDevOwner $A2cDevRepo $devTag "URnetwork-$devVersion-x64.msi" '' ($now - 30) $true $true (Join-Path $A2cDir 'standin-developer.json')
  $routes = @((New-A2cRoute 'api.github.com' $A2cOfficialListPath 200 $official.File),
    (New-A2cRoute 'api.github.com' $A2cDevListPath 200 $devList.File))
  $state = Start-A2cStandIn $routes $now
  # the helper's own folder and lock, as an update makes them
  $first = Invoke-A2cHelper @('--apply-update', $devTag) '' 300
  Test-A2c ($first.ExitCode -eq $A2cRefusal.NotOffered) "with no marker the helper refuses $devTag NotOffered" ("exit 0x{0:X8}" -f $first.ExitCode)
  Write-Host "updates\: $(Get-A2cSddl $UrUpdatesDir); helper.lock: $(Get-A2cSddl $A2cLockPath)"

  # the control, once in the leg: under a trusted marker the tag passes the offer
  Write-Step "1. the control: under a trusted marker, $devTag passes the offer and ends at the download"
  $set = Invoke-A2cHelper @('--set-update-channel', 'developer') 'ok' 120
  Test-A2c ($set.ExitCode -eq 0 -and (Test-A2cMarkerTrusted)) "the channel verb, answered OK, makes the trusted marker" ("exit 0x{0:X8}" -f $set.ExitCode)
  $run = Invoke-A2cHelper @('--apply-update', $devTag) '' 300
  Test-A2c ($run.ExitCode -eq $A2cRefusal.Download) "under it, $devTag passes the offer and ends at the download" ("exit 0x{0:X8}" -f $run.ExitCode)
  $back = Invoke-A2cHelper @('--set-update-channel', 'official') '' 60
  Test-A2c ($back.ExitCode -eq 0 -and -not (Get-A2cMarker).Exists) "and the official channel's verb removes it"

  $userSid = ([Security.Principal.WindowsIdentity]::GetCurrent()).User.Value
  $kinds = @(
    @{ Name = 'a folder'; Kind = 'folder' },
    @{ Name = 'a reparse point'; Kind = 'link' },
    @{ Name = 'a file with bytes'; Kind = 'bytes' },
    @{ Name = 'a fourth ACE that names the user'; Kind = 'file'; Sddl = "$A2cMarkerSddl(A;;FA;;;$userSid)" },
    @{ Name = 'Users able to read its data'; Kind = 'file'; Sddl = 'O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x120089;;;BU)' },
    @{ Name = 'owned by the user'; Kind = 'file'; Sddl = "O:$($userSid)D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x120080;;;BU)" },
    @{ Name = "the lock file under the marker's name"; Kind = 'lock' },
    @{ Name = 'a good marker under an updates\ that Users can write'; Kind = 'openfolder' }
  )
  $step = 1
  foreach ($case in $kinds) {
    $step++
    Write-Step "$step. a marker that must not be trusted: $($case.Name)"
    Stop-UrAppProcesses
    Clear-A2cMarker
    $folderAcl = $null
    switch ($case.Kind) {
      'lock' {
        if (-not (Test-Path -LiteralPath $A2cLockPath)) { throw "no helper.lock to rename" }
        Move-Item -LiteralPath $A2cLockPath -Destination $A2cMarkerPath
        Write-Host "the test moved helper.lock to the marker's name: $((Get-A2cMarker).Text)"
      }
      'openfolder' {
        Set-A2cMarkerAs 'file'
        $folderAcl = Get-Acl -LiteralPath $UrUpdatesDir
        $open = Get-Acl -LiteralPath $UrUpdatesDir
        $open.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule('BUILTIN\Users', 'Modify', 'ContainerInherit, ObjectInherit', 'None', 'Allow')))
        Set-Acl -LiteralPath $UrUpdatesDir -AclObject $open
        Write-Host "updates\ now: $(Get-A2cSddl $UrUpdatesDir)"
      }
      default {
        if ($case['Sddl']) { Set-A2cMarkerAs $case['Kind'] $case['Sddl'] } else { Set-A2cMarkerAs $case['Kind'] }
      }
    }
    $from = Get-UrAppLogLength
    $app = Start-A2cApp 'developer'
    $facts = Wait-A2cChannel $app.Id { param($f) $f.Line -eq $A2cS.S14 -and $f.Repair } 'S14 and its button' 60
    Test-A2c ($facts.Line -eq $A2cS.S14) "$($case.Name): the Developer screen says S14" $facts.Line
    Test-A2c ($facts.Repair -and $facts.RepairEnabled) "$($case.Name): with its Switch to official releases button"
    Test-A2c (-not $facts.SwitchOn) "$($case.Name): the switch shows official"
    Save-UrShot $app.Id "6-$step-untrusted-$A2cOs"
    Stop-UrAppProcesses
    $app = $null
    $run = Invoke-A2cHelper @('--apply-update', $devTag) '' 300
    $log = Get-A2cHelperLog $devTag
    $refusedLine = @($log | Where-Object { $_ -match 'refused' } | Select-Object -Last 1)
    if ($case.Kind -eq 'openfolder') {
      # the lock's first step finds updates\ open to others, takes what is at
      # the marker's name away, and sets the folder right (D3)
      Test-A2c ($run.ExitCode -eq $A2cRefusal.NotOffered) "$($case.Name): the helper refuses $devTag NotOffered" ("exit 0x{0:X8}: $($refusedLine -join '')" -f $run.ExitCode)
      Test-A2c (-not (Get-A2cMarker).Exists) "$($case.Name): and removes the marker it found in a folder open to others" (Get-A2cMarker).Text
      Write-Host "updates\ after the helper: $(Get-A2cSddl $UrUpdatesDir)"
    } else {
      Test-A2c ($run.ExitCode -eq $A2cRefusal.NotOffered) "$($case.Name): the helper refuses $devTag NotOffered" ("exit 0x{0:X8}" -f $run.ExitCode)
      Test-A2c (@($log | Where-Object { $_ -match "refused: the official feed offers .*, not $([regex]::Escape($devTag)) above" }).Count -ge 1) "$($case.Name): with the official feed's line" ($refusedLine -join '')
      Test-A2c (@($log | Where-Object { $_ -match 'the channel marker is not trusted' }).Count -ge 1) "$($case.Name): and its log says the marker is not trusted"
    }
    # the button removes what is there, and the switch then turns the channel on
    $app = Start-A2cApp 'developer'
    $facts = Wait-A2cChannel $app.Id { param($f) $f.Line } 'the channel line' 60
    if ($facts.Repair) {
      Clear-A2cHelperStarts
      Invoke-A2cId $app.Id $A2cIdRepair
      $deadline = (Get-Date).AddSeconds(60)
      while ((Get-Date) -lt $deadline -and (Get-A2cMarker).Exists) { Start-Sleep -Milliseconds 500 }
      Test-A2c (-not (Get-A2cMarker).Exists) "$($case.Name): the button removes what was at the marker's name" (Get-A2cMarker).Text
    } elseif ($case.Kind -eq 'openfolder') {
      Write-Host "nothing left to remove: the helper took it away"
    } else {
      Test-A2c $false "$($case.Name): the Developer screen offers its button"
    }
    $facts = Wait-A2cChannel $app.Id { param($f) $f.Line -eq $A2cS.S4 -and $f.SwitchEnabled } 'S4' 60
    Invoke-A2cToggle $app.Id
    $dialog = Find-A2cDialog 60
    if ($dialog) {
      $helper = Get-Process -Id $dialog.ProcessId
      $null = $helper.Handle
      Invoke-A2cDialogButton $dialog '1'
      [void]$helper.WaitForExit(60000)
    }
    $on = Wait-A2cChannel $app.Id { param($f) $f.Line -eq $A2cS.S5 } 'S5' 60
    Test-A2c ($dialog -and $on.Line -eq $A2cS.S5 -and (Test-A2cMarkerTrusted)) "$($case.Name): then the switch turns the channel on" $on.Line
    Save-UrShot $app.Id "6-$step-repaired-$A2cOs"
    Stop-UrAppProcesses
    $app = $null
    $back = Invoke-A2cHelper @('--set-update-channel', 'official') '' 60
    if ($back.ExitCode -ne 0 -or (Get-A2cMarker).Exists) { throw "the official channel's verb did not remove the marker after $($case.Name)" }
    if ($folderAcl) { Write-Host "updates\ at the end of the case: $(Get-A2cSddl $UrUpdatesDir)" }
  }

  $step++
  Write-Step "$step. an updates\ that is a junction"
  Stop-UrAppProcesses
  # the target and the marker in it carry the helper's own security, so the
  # reparse rule alone makes the marker untrusted (the A2c review's A-3)
  $target = New-Item -ItemType Directory -Force (Join-Path $A2cDir 'junction-target')
  $acl = Get-Acl -LiteralPath $target.FullName
  $acl.SetSecurityDescriptorSddlForm($A2cFolderSddl, [Security.AccessControl.AccessControlSections]'Owner, Access')
  Set-Acl -LiteralPath $target.FullName -AclObject $acl
  $planted = Join-Path $target.FullName 'developer-channel'
  [IO.File]::WriteAllBytes($planted, [byte[]]@())
  $acl = Get-Acl -LiteralPath $planted
  $acl.SetSecurityDescriptorSddlForm($A2cMarkerSddl, [Security.AccessControl.AccessControlSections]'Owner, Access')
  Set-Acl -LiteralPath $planted -AclObject $acl
  # JudgeMarker reads the owner, whether the DACL is protected, and the ACEs;
  # Set-Acl also sets the DACL's auto-inherited bit (D:PAI), which it does not
  # read, so the comparison sets that bit aside
  $judged = { param($path) (Get-A2cSddl $path) -replace '^(O:[^:]+)D:PAI\(', '${1}D:P(' }
  Test-A2c ((& $judged $target.FullName) -eq $A2cFolderSddl -and (& $judged $planted) -eq $A2cMarkerSddl -and (Get-Item -LiteralPath $planted).Length -eq 0) "the junction's target and the marker in it carry the helper's own security, as JudgeMarker reads it" "target: $(Get-A2cSddl $target.FullName); marker: $(Get-A2cSddl $planted)"
  $before = @(Get-ChildItem -Force -LiteralPath $target | ForEach-Object { "$($_.Name) $($_.Length) $(Get-A2cSddl $_.FullName)" })
  Rename-Item -LiteralPath $UrUpdatesDir -NewName (Split-Path -Leaf $saved)
  cmd /c mklink /J "$UrUpdatesDir" "$($target.FullName)" | Write-Host
  $junction = $true
  $from = Get-UrAppLogLength
  $app = Start-A2cApp 'developer'
  $facts = Wait-A2cChannel $app.Id { param($f) $f.Line -eq $A2cS.S4 } 'S4' 60
  Test-A2c ($facts.Line -eq $A2cS.S4) "a junction for updates\: the tray says what the official channel says, S4" $facts.Line
  Test-A2c (-not $facts.Repair -and -not $facts.SwitchOn) "a junction for updates\: with no Switch to official releases button, and the switch off"
  $reason = @(Get-UrAppLogLines 'update: the update channel''s marker is not trusted' $from)
  $reason | ForEach-Object { Write-Host "app log: $_" }
  Test-A2c (@($reason | Where-Object { $_ -match [regex]::Escape('(updates\ is not a plain folder)') }).Count -ge 1) "a junction for updates\: the app's log says why, the junction alone" ($reason -join ' | ')
  # the screen asked for the channel as it was shown (SyncChannel); read it
  # again once that has been answered
  Start-Sleep -Seconds 3
  $again = Get-A2cChannel $app.Id
  Show-A2cChannel $again '(3 s later)'
  Test-A2c ($again.Line -eq $A2cS.S4 -and -not $again.Repair) "a junction for updates\: still S4 and no button, 3 s later" $again.Line
  Save-UrShot $app.Id "6-junction-$A2cOs"
  Stop-UrAppProcesses
  $app = $null
  foreach ($arguments in @(@('--set-update-channel', 'developer'), @('--set-update-channel', 'official'), @('--apply-update', $devTag))) {
    # the developer channel asks before it takes the lock (SPEC-A2c 4.2)
    $answer = ''
    if ($arguments[1] -eq 'developer') { $answer = 'ok' }
    $run = Invoke-A2cHelper $arguments $answer 120
    Test-A2c ($run.ExitCode -eq $A2cRefusal.Staging) "a junction for updates\: '$($arguments -join ' ')' ends Staging" ("exit 0x{0:X8}" -f $run.ExitCode)
  }
  $after = @(Get-ChildItem -Force -LiteralPath $target | ForEach-Object { "$($_.Name) $($_.Length) $(Get-A2cSddl $_.FullName)" })
  Test-A2c (($before -join ';') -eq ($after -join ';')) "and nothing is written through it" "before: $($before -join ';'), after: $($after -join ';')"
  [IO.Directory]::Delete($UrUpdatesDir)
  Rename-Item -LiteralPath $saved -NewName 'updates'
  $junction = $false

  $step++
  Write-Step "$step. uninstall removes the marker and the lock (leg 8)"
  $set = Invoke-A2cHelper @('--set-update-channel', 'developer') 'ok' 120
  Test-A2c ($set.ExitCode -eq 0 -and (Test-A2cMarkerTrusted) -and (Test-Path -LiteralPath $A2cLockPath)) "before the uninstall, the marker and the lock are there"
  $productCode = (Get-Content -LiteralPath (Join-Path $env:RUNNER_TEMP 'vn.productcode')).Trim()
  $code = Invoke-UrMsiexec '/x' $productCode '/qn' 'vn-uninstall.log'
  Test-A2c ($code -eq 0) "vN uninstalls" "exit $code"
  Test-A2c (-not (Test-Path -LiteralPath $A2cMarkerPath) -and -not (Test-Path -LiteralPath $A2cLockPath)) "and the marker and the lock are gone" "$(Test-Path -LiteralPath $UrUpdatesDir)"
  Remove-UrTestNetwork $state
  $state = $null
  $code = Invoke-UrMsiexec '/i' $officialMsi '/qn' 'official-install.log'
  Test-A2c ($code -eq 0) "the real, older official MSI $($officialReading.Offered.Tag) then installs" "exit $code"
  Test-A2c (-not (Test-Path -LiteralPath $A2cMarkerPath)) "on the official channel: no marker"
  Complete-A2cLeg 'markers'
} catch {
  if ($app -and -not $app.HasExited) {
    try { Save-UrShot $app.Id "markers-failure-$A2cOs" } catch { }
    try { Show-UrUiTree $app.Id 250 } catch { }
  }
  foreach ($line in $A2cFailures) { Write-Host "FAILED so far: $line" }
  throw
} finally {
  Stop-UrAppProcesses
  if ($junction -and (Test-Path -LiteralPath $saved)) {
    try { [IO.Directory]::Delete($UrUpdatesDir); Rename-Item -LiteralPath $saved -NewName 'updates' } catch { Write-Host "the junction was not undone: $($_.Exception.Message)" }
  }
  Remove-UrTestNetwork $state
}
