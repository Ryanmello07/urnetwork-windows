# Helpers for .github/workflows/vpn-lock.yml: the update helper's lock, which
# replaced a named mutex that any process without administrator rights could
# create first and hold. On top of vpn-feed-lib.ps1 (the real feed, the install
# and the helper run), this adds the non-admin principals the lock test needs
# and the probes they run.
# Dot-sourced by every test step:
#   . "$env:GITHUB_WORKSPACE\.github\vpn-lock-lib.ps1"
# Runs on a disposable GitHub runner only. It installs URnetwork machine-wide,
# registers short-lived scheduled tasks as LOCAL SERVICE and a throwaway
# standard account, and removes each before the step that made it ends. It
# publishes nothing and creates no release or tag. Pure ASCII, Windows
# PowerShell 5.1.
#
# The runner's own session is elevated (an administrator, high integrity), so
# every "without administrator rights" principal here is made on purpose:
#   - NT AUTHORITY\LOCAL SERVICE: a non-administrator service account that has
#     SeCreateGlobalPrivilege, so it can create the helper's old Global\ mutex
#     and show the defect, and is neither SYSTEM nor an administrator, so the
#     admin-only lock file denies it and shows the fix. A scheduled task runs
#     as it with no password and no extra right.
#   - a throwaway standard local account: the plainest "no rights" principal,
#     used best-effort for the same probes; if it cannot be set up on a runner
#     the leg says so and rests on LOCAL SERVICE.
# What this does NOT show: Windows' own consent prompt (the runner elevates
# without one), and a second interactive user.
#
# SPDX-License-Identifier: MPL-2.0

. (Join-Path $PSScriptRoot 'vpn-feed-lib.ps1')

# Common/UpdateResult.h refusal codes used here (NotOffered and RateLimited are
# defined in vpn-feed-lib.ps1).
$UrRefusalBadArguments = 0x20000004
$UrRefusalBusy = 0x20000005
$UrRefusalStaging = 0x2000000B

# The helper's old mutex name, and the file that replaced it.
$UrMutexName = 'Global\URnetworkUpdateHelper'
$UrLockPath = Join-Path $UrUpdatesDir 'helper.lock'

# The probe script, and a world-writable folder it and its output live in, so
# a principal without administrator rights (LOCAL SERVICE, a standard account)
# can read the script and write its result -- neither can reach the runner
# admin's RUNNER_TEMP or the workspace checkout.
$UrProbeSource = Join-Path $PSScriptRoot 'vpn-lock-probe.ps1'
$UrProbeDir = Join-Path $env:SystemDrive 'urnlockprobe'
$UrProbeScript = Join-Path $UrProbeDir 'vpn-lock-probe.ps1'

function Initialize-UrProbeDir {
  if (-not (Test-Path -LiteralPath $UrProbeDir)) {
    New-Item -ItemType Directory -Force -Path $UrProbeDir | Out-Null
    # a throwaway folder on a disposable runner: let any principal read the
    # probe and write its result there
    & icacls $UrProbeDir /grant '*S-1-1-0:(OI)(CI)M' | Out-Null
  }
  Copy-Item -LiteralPath $UrProbeSource -Destination $UrProbeScript -Force
}

# Builds the scheduled-task principal for a named non-admin service account.
function Get-UrServicePrincipal([string]$account) {
  return New-ScheduledTaskPrincipal -UserId $account -LogonType ServiceAccount -RunLevel Limited
}

# Runs vpn-lock-probe.ps1 once, as $principal (a ScheduledTaskPrincipal), with
# the given arguments, and returns the JSON result it wrote. The task is
# registered, started, waited for and removed here. $extra is the probe's own
# arguments beyond -Out.
function Invoke-UrProbe($principal, [string]$label, [string[]]$extra, [int]$timeoutSeconds = 120) {
  Initialize-UrProbeDir
  $taskName = "urn-lock-probe-$label-$([guid]::NewGuid().ToString('N').Substring(0,8))"
  $out = Join-Path $UrProbeDir "probe-$label.json"
  if (Test-Path -LiteralPath $out) { Remove-Item -Force -LiteralPath $out }
  $argline = ('-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{0}" -Out "{1}" {2}' -f $UrProbeScript, $out, ($extra -join ' '))
  $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $argline
  Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Force | Out-Null
  try {
    Start-ScheduledTask -TaskName $taskName
    $deadline = (Get-Date).AddSeconds($timeoutSeconds)
    $pending = @(0x41301, 0x41303)
    do {
      Start-Sleep -Milliseconds 500
      $task = Get-ScheduledTask -TaskName $taskName
      $info = Get-ScheduledTaskInfo -TaskName $taskName
      if ((Get-Date) -gt $deadline) { throw "probe $label did not finish in $timeoutSeconds s" }
    } while ($task.State -eq 'Running' -or $pending -contains [int]$info.LastTaskResult)
    if (-not (Test-Path -LiteralPath $out)) { throw "probe $label wrote no result (task result $([int]$info.LastTaskResult))" }
    $result = Get-Content -Raw -LiteralPath $out | ConvertFrom-Json
    Write-Host ("probe {0} as {1}: {2}" -f $label, $result.whoami, (Get-Content -Raw -LiteralPath $out).Trim())
    return $result
  } finally {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
  }
}

# Starts a holder of the helper's old mutex as LOCAL SERVICE: a scheduled task
# running the probe in 'mutex' mode, which creates and owns $UrMutexName and
# holds it until the stop file appears. Returns what Stop-UrMutexHolder needs,
# after confirming the mutex is held. Throws if the principal could not take
# the name (and says so, which is itself a result).
function Start-UrMutexHolder([string]$account = 'LOCAL SERVICE', [int]$holdSeconds = 600) {
  Initialize-UrProbeDir
  $label = 'mutexhold'
  $taskName = "urn-$label-$([guid]::NewGuid().ToString('N').Substring(0,8))"
  $out = Join-Path $UrProbeDir "mutexhold-$($account -replace '[^A-Za-z0-9]','_').json"
  $stop = Join-Path $UrProbeDir "mutexhold-stop-$([guid]::NewGuid().ToString('N').Substring(0,8))"
  foreach ($p in $out, $stop) { if (Test-Path -LiteralPath $p) { Remove-Item -Force -LiteralPath $p } }
  $argline = ('-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{0}" -Mode mutex -MutexName "{1}" -Out "{2}" -StopFile "{3}" -HoldSeconds {4}' -f $UrProbeScript, $UrMutexName, $out, $stop, $holdSeconds)
  $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $argline
  Register-ScheduledTask -TaskName $taskName -Action $action -Principal (Get-UrServicePrincipal $account) -Force | Out-Null
  Start-ScheduledTask -TaskName $taskName
  $deadline = (Get-Date).AddSeconds(60)
  while (-not (Test-Path -LiteralPath $out)) {
    if ((Get-Date) -gt $deadline) {
      New-Item -ItemType File -Force -Path $stop | Out-Null
      Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
      throw "the mutex holder as $account did not report within 60 s"
    }
    Start-Sleep -Milliseconds 300
  }
  $result = Get-Content -Raw -LiteralPath $out | ConvertFrom-Json
  Write-Host ("mutex holder as {0}: {1}" -f $result.whoami, (Get-Content -Raw -LiteralPath $out).Trim())
  if (-not $result.held) {
    New-Item -ItemType File -Force -Path $stop | Out-Null
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
    throw "the principal $account could not create and hold $UrMutexName (error: $($result.error))"
  }
  return [pscustomobject]@{ Task = $taskName; Stop = $stop; Account = $account; Whoami = $result.whoami; CreatedNew = $result.createdNew }
}

function Stop-UrMutexHolder($holder) {
  if ($null -eq $holder) { return }
  New-Item -ItemType File -Force -Path $holder.Stop | Out-Null
  $deadline = (Get-Date).AddSeconds(30)
  while ((Get-Date) -lt $deadline) {
    $task = Get-ScheduledTask -TaskName $holder.Task -ErrorAction SilentlyContinue
    if (-not $task -or $task.State -ne 'Running') { break }
    Start-Sleep -Milliseconds 300
  }
  Unregister-ScheduledTask -TaskName $holder.Task -Confirm:$false -ErrorAction SilentlyContinue
  Write-Host "mutex holder $($holder.Account) released"
}

# Holds updates\helper.lock the way a running helper does, from an elevated
# process in this session (opened for its data, shared with nobody), to stand
# in for a first helper while a second runs. Returns the process; stop it to
# let go. The elevated session is the only principal that can open the
# admin-only lock at all, which is the point.
function Start-UrLockHolderElevated([int]$holdSeconds = 600) {
  $script = "`$ErrorActionPreference='Stop'; `$f=[IO.FileStream]::new('$UrLockPath',[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::None); [Console]::Out.WriteLine('held'); Start-Sleep -Seconds $holdSeconds; `$f.Close()"
  $holder = Start-Process -FilePath 'powershell.exe' -PassThru -WindowStyle Hidden -ArgumentList @(
    '-NoProfile', '-NonInteractive', '-Command', $script)
  Start-Sleep -Seconds 2
  if ($holder.HasExited) { throw "the elevated lock holder exited at once ($($holder.ExitCode)); is $UrLockPath present?" }
  Write-Host "a first helper stand-in (pid $($holder.Id)) holds $UrLockPath (read data, shared with nobody)"
  return $holder
}

function Stop-UrLockHolder($holder) {
  if ($null -ne $holder -and -not $holder.HasExited) { Stop-Process -Id $holder.Id -Force -ErrorAction SilentlyContinue }
}

# ---- a throwaway standard account, best effort ------------------------------

$UrLsaSource = @'
using System;
using System.Runtime.InteropServices;

public static class UrLsa {
  [StructLayout(LayoutKind.Sequential)]
  struct LSA_UNICODE_STRING { public ushort Length; public ushort MaximumLength; public IntPtr Buffer; }
  [StructLayout(LayoutKind.Sequential)]
  struct LSA_OBJECT_ATTRIBUTES { public int Length; public IntPtr RootDirectory; public IntPtr ObjectName; public int Attributes; public IntPtr SecurityDescriptor; public IntPtr SecurityQualityOfService; }

  [DllImport("advapi32.dll", SetLastError = true)]
  static extern uint LsaOpenPolicy(IntPtr SystemName, ref LSA_OBJECT_ATTRIBUTES Attr, int Access, out IntPtr Handle);
  [DllImport("advapi32.dll", SetLastError = true)]
  static extern uint LsaAddAccountRights(IntPtr Policy, byte[] Sid, LSA_UNICODE_STRING[] Rights, int Count);
  [DllImport("advapi32.dll")]
  static extern uint LsaClose(IntPtr Handle);
  [DllImport("advapi32.dll", SetLastError = true)]
  static extern uint LsaNtStatusToWinError(uint Status);
  [DllImport("advapi32.dll", CharSet = CharSet.Auto, SetLastError = true)]
  static extern bool LookupAccountName(string System, string Account, byte[] Sid, ref uint SidLen, System.Text.StringBuilder Domain, ref uint DomLen, out int Use);

  static LSA_UNICODE_STRING Str(string s) {
    LSA_UNICODE_STRING u = new LSA_UNICODE_STRING();
    u.Buffer = Marshal.StringToHGlobalUni(s);
    u.Length = (ushort)(s.Length * 2);
    u.MaximumLength = (ushort)((s.Length + 1) * 2);
    return u;
  }

  // Grants "Log on as a batch job" to the account, so a scheduled task can run
  // as it with a password. Returns the Win32 error (0 = ok).
  public static uint GrantBatchLogon(string account) {
    byte[] sid = new byte[256];
    uint sidLen = 256;
    System.Text.StringBuilder domain = new System.Text.StringBuilder(256);
    uint domLen = 256;
    int use;
    if (!LookupAccountName(null, account, sid, ref sidLen, domain, ref domLen, out use))
      return (uint)Marshal.GetLastWin32Error();
    Array.Resize(ref sid, (int)sidLen);
    LSA_OBJECT_ATTRIBUTES attr = new LSA_OBJECT_ATTRIBUTES();
    IntPtr policy;
    uint st = LsaOpenPolicy(IntPtr.Zero, ref attr, 0x00000010 /* POLICY_CREATE_ACCOUNT */ | 0x00000020 /* POLICY_LOOKUP_NAMES */, out policy);
    if (st != 0) return LsaNtStatusToWinError(st);
    try {
      LSA_UNICODE_STRING[] rights = new LSA_UNICODE_STRING[] { Str("SeBatchLogonRight") };
      st = LsaAddAccountRights(policy, sid, rights, 1);
      return LsaNtStatusToWinError(st);
    } finally {
      LsaClose(policy);
    }
  }
}
'@

function Initialize-UrLsa {
  if (-not ('UrLsa' -as [type])) { Add-Type -TypeDefinition $UrLsaSource }
}

# Creates a throwaway standard local account (not an administrator) and grants
# it batch logon, so Invoke-UrProbe can run as it. Returns @{ User; Password;
# Principal } or $null if it could not be made (the caller reports and rests on
# LOCAL SERVICE).
function New-UrStandardUser {
  try {
    Initialize-UrLsa
    $user = 'urnlocktest'
    $password = 'Lk!' + [guid]::NewGuid().ToString('N').Substring(0, 16) + 'aA1'
    $secure = ConvertTo-SecureString $password -AsPlainText -Force
    $existing = Get-LocalUser -Name $user -ErrorAction SilentlyContinue
    if ($existing) { Remove-LocalUser -Name $user -ErrorAction SilentlyContinue }
    New-LocalUser -Name $user -Password $secure -AccountNeverExpires -PasswordNeverExpires -UserMayNotChangePassword -ErrorAction Stop | Out-Null
    # explicitly NOT added to Administrators; it stays in Users only
    $account = "$env:COMPUTERNAME\$user"
    $err = [UrLsa]::GrantBatchLogon($account)
    if ($err -ne 0) { throw "GrantBatchLogon returned $err" }
    $isAdmin = @(Get-LocalGroupMember -Group 'Administrators' -ErrorAction SilentlyContinue | Where-Object { $_.Name -like "*\$user" }).Count -gt 0
    if ($isAdmin) { throw "the throwaway account ended up an administrator" }
    $principal = New-ScheduledTaskPrincipal -UserId $account -LogonType Password -RunLevel Limited
    Write-Host "throwaway standard account $account created (not an administrator)"
    return [pscustomobject]@{ User = $user; Account = $account; Password = $password; Principal = $principal }
  } catch {
    Write-Host "NOTE: could not establish a standard account on this runner ($($_.Exception.Message.Trim())); the asserted non-admin principal is LOCAL SERVICE"
    return $null
  }
}

function Remove-UrStandardUser($info) {
  if ($null -eq $info) { return }
  Remove-LocalUser -Name $info.User -ErrorAction SilentlyContinue
  Write-Host "throwaway standard account $($info.Account) removed"
}

# Runs vpn-lock-probe.ps1 once as a standard account (password logon).
function Invoke-UrProbeAsUser($userInfo, [string]$label, [string[]]$extra, [int]$timeoutSeconds = 120) {
  Initialize-UrProbeDir
  $taskName = "urn-userprobe-$label-$([guid]::NewGuid().ToString('N').Substring(0,8))"
  $out = Join-Path $UrProbeDir "userprobe-$label.json"
  if (Test-Path -LiteralPath $out) { Remove-Item -Force -LiteralPath $out }
  $argline = ('-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{0}" -Out "{1}" {2}' -f $UrProbeScript, $out, ($extra -join ' '))
  $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $argline
  # -Principal and -User/-Password are different parameter sets of
  # Register-ScheduledTask, so passing both is AmbiguousParameterSet. Register
  # with -User/-Password (a password logon as the standard account) and
  # -RunLevel Limited, which keeps the task unelevated -- the point of a
  # standard-account probe. ($userInfo.Principal is left unused.)
  Register-ScheduledTask -TaskName $taskName -Action $action -User $userInfo.Account -Password $userInfo.Password -RunLevel Limited -Force | Out-Null
  try {
    Start-ScheduledTask -TaskName $taskName
    $deadline = (Get-Date).AddSeconds($timeoutSeconds)
    $pending = @(0x41301, 0x41303)
    do {
      Start-Sleep -Milliseconds 500
      $task = Get-ScheduledTask -TaskName $taskName
      $info = Get-ScheduledTaskInfo -TaskName $taskName
      if ((Get-Date) -gt $deadline) { throw "user probe $label did not finish in $timeoutSeconds s" }
    } while ($task.State -eq 'Running' -or $pending -contains [int]$info.LastTaskResult)
    if (-not (Test-Path -LiteralPath $out)) { throw "user probe $label wrote no result (task result $([int]$info.LastTaskResult))" }
    $result = Get-Content -Raw -LiteralPath $out | ConvertFrom-Json
    Write-Host ("user probe {0} as {1}: {2}" -f $label, $result.whoami, (Get-Content -Raw -LiteralPath $out).Trim())
    return $result
  } finally {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
  }
}

# Puts an old-helper build (main's URnetworkUpdate.exe, downloaded as an
# artifact) into its own admin-only folder under Program Files, so it passes
# the helper's own install-location check and can be run to show the old mutex
# behaviour. Returns the exe path.
function Install-UrOldHelper([string]$sourceExe) {
  $dir = 'C:\Program Files\urn-oldhelper'
  if (Test-Path -LiteralPath $dir) { Remove-Item -Recurse -Force -LiteralPath $dir }
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  $dest = Join-Path $dir 'URnetworkUpdate.exe'
  Copy-Item -LiteralPath $sourceExe -Destination $dest -Force
  # A fresh folder under Program Files inherits its admin-only ACL (Users get
  # read and execute, no write), so it is not a *protected* DACL and
  # Assert-UrAdminOnly does not apply; the helper's own install-location check
  # is what matters, and if it did not pass the helper would answer NotInstalled
  # (0x20000002) rather than the Busy we assert. Print the ACL for the record.
  # Write-Host, never the pipeline: were these lines to reach the output
  # stream they would join this function's return value, and the returned
  # path would be a polluted array that Start-Process cannot find.
  (Get-Acl -LiteralPath $dir).Access | ForEach-Object { Write-Host "urn-oldhelper ACE: $($_.IdentityReference) $($_.FileSystemRights) $($_.AccessControlType)" }
  Write-Host "old helper placed at $dest ($(Get-UrFileVersion $dest)), feed $(Get-UrHelperFeed $dest)"
  return $dest
}

# Runs a helper exe at an explicit path (the installed one, or the old-helper
# stand-in) as the elevated session does, and returns the exit code. Unlike
# Invoke-UrHelper it does not assume the installed path.
function Invoke-UrHelperAt([string]$exe, [string]$tag, [int]$timeoutSeconds = 300) {
  Write-Host "$exe --apply-update $tag"
  $helper = Start-Process -FilePath $exe -ArgumentList @('--apply-update', $tag) -PassThru
  $null = $helper.Handle
  if (-not $helper.WaitForExit($timeoutSeconds * 1000)) {
    Stop-Process -Id $helper.Id -Force -ErrorAction SilentlyContinue
    throw "the helper $exe (pid $($helper.Id)) was still running after $timeoutSeconds s"
  }
  Write-Host ("helper {0} pid {1} exit {2} (0x{2:X8})" -f $exe, $helper.Id, $helper.ExitCode)
  return [pscustomobject]@{ Process = $helper; ExitCode = [long]$helper.ExitCode }
}
