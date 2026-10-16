# What a process without administrator rights can do to the update channel,
# for A2c's runner legs: a2c-lib.ps1 Start-A2cFiltered starts this script
# under a filtered token (Administrators deny-only, no privileges beyond a
# standard user's, Medium integrity), as a standard user's process runs. It
# writes what it found, one "key: value" line each, to -Out.
#   -Action probe   its own token, then for each of -PathList: whether its
#                   descriptor can be read, and which of the access rights
#                   below a handle can be opened with
#   -Action hold    as probe, then keeps open every handle it could get, with
#                   no sharing, and a mutex named -Mutex it creates and owns,
#                   until the file -Stop exists
#   -Action press   finds the top-level window named -Caption and presses its
#                   button -Button (1 OK, 2 Cancel) through UI Automation's
#                   Invoke pattern, as the runner's own presses are made
#   -Action delete  tries to delete each of -PathList
# Nothing here synthesizes mouse or keyboard input.
# Throwaway (ci/a2c-legs-* only). Pure ASCII, Windows PowerShell 5.1.
#
# SPDX-License-Identifier: MPL-2.0
param(
  [Parameter(Mandatory = $true)][string]$Action,
  [Parameter(Mandatory = $true)][string]$Out,
  # the paths, '|'-separated: -File passes every argument as one string
  [string]$PathList = '',
  [string]$Mutex = '',
  [string]$Stop = '',
  [string]$Caption = '',
  [string]$Button = '1',
  [int]$Seconds = 900
)
$ErrorActionPreference = 'Stop'
$Paths = @($PathList -split '\|' | Where-Object { $_ })

function Say([string]$key, [string]$value) {
  Add-Content -LiteralPath $Out -Value "${key}: $value" -Encoding ascii
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Security.Principal;

public static class A2cMedium {
  [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  public static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition,
                                          uint flags, IntPtr template);
  [DllImport("kernel32.dll", SetLastError = true)]
  public static extern bool CloseHandle(IntPtr handle);
  [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  public static extern IntPtr CreateMutexW(IntPtr security, bool initialOwner, string name);
  [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  public static extern bool DeleteFileW(string name);
  [DllImport("advapi32.dll", SetLastError = true)]
  static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
  [DllImport("kernel32.dll")]
  static extern IntPtr GetCurrentProcess();
  [DllImport("advapi32.dll", SetLastError = true)]
  static extern bool GetTokenInformation(IntPtr token, int infoClass, IntPtr info, int length, out int returned);
  [DllImport("advapi32.dll")]
  static extern IntPtr GetSidSubAuthority(IntPtr sid, uint index);
  [DllImport("advapi32.dll")]
  static extern IntPtr GetSidSubAuthorityCount(IntPtr sid);

  public static readonly IntPtr Invalid = new IntPtr(-1);

  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr dialog, int id);
  [DllImport("user32.dll", SetLastError = true)] static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);

  // The runner's fallback press, from here: the button's own BN_CLICKED
  // posted to its dialog. 0, or the error.
  public static int PostCommand(IntPtr dialog, int id) {
    return PostMessageW(dialog, 0x0111, (IntPtr)id, GetDlgItem(dialog, id)) ? 0 : Marshal.GetLastWin32Error();
  }

  // A handle on `path` with `access`, every sharing allowed unless `share`
  // says otherwise, never following a link: the handle, or -1 with the
  // error in `error`.
  public static IntPtr Open(string path, uint access, uint share, out int error) {
    // OPEN_EXISTING; FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT
    IntPtr handle = CreateFileW(path, access, share, IntPtr.Zero, 3, 0x02000000 | 0x00200000, IntPtr.Zero);
    error = handle == Invalid ? Marshal.GetLastWin32Error() : 0;
    return handle;
  }

  // "<integrity>, Administrators <enabled|deny-only|absent>"
  public static string Token() {
    IntPtr token;
    if (!OpenProcessToken(GetCurrentProcess(), 0x0008, out token)) return "OpenProcessToken error " + Marshal.GetLastWin32Error();
    string level = "?";
    int size;
    GetTokenInformation(token, 25, IntPtr.Zero, 0, out size);
    IntPtr label = Marshal.AllocHGlobal(size);
    try {
      if (GetTokenInformation(token, 25, label, size, out size)) {
        IntPtr sid = Marshal.ReadIntPtr(label);
        int count = Marshal.ReadByte(GetSidSubAuthorityCount(sid));
        int rid = Marshal.ReadInt32(GetSidSubAuthority(sid, (uint)(count - 1)));
        level = rid == 0x2000 ? "Medium" : rid == 0x3000 ? "High" : rid == 0x4000 ? "System" : rid == 0x1000 ? "Low" : "0x" + rid.ToString("X");
      }
    } finally { Marshal.FreeHGlobal(label); }
    WindowsIdentity identity = WindowsIdentity.GetCurrent();
    string admins = "absent";
    SecurityIdentifier administrators = new SecurityIdentifier("S-1-5-32-544");
    foreach (IdentityReference group in identity.Groups) {
      if (group.Equals(administrators)) admins = "enabled";
    }
    if (admins == "absent" && new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator)) admins = "enabled";
    if (admins == "absent") {
      // a deny-only group is not in Groups; GetTokenInformation(TokenGroups) would list it
      admins = "deny-only or absent";
    }
    return level + " integrity, Administrators " + admins + ", user " + identity.Name;
  }
}
'@

# the rights a handle is asked for, one at a time
$rights = [ordered]@{
  'READ_CONTROL' = 0x00020000; 'FILE_READ_ATTRIBUTES' = 0x00000080; 'SYNCHRONIZE' = 0x00100000
  'FILE_READ_DATA' = 0x00000001; 'FILE_WRITE_DATA' = 0x00000002; 'FILE_APPEND_DATA' = 0x00000004
  'FILE_WRITE_ATTRIBUTES' = 0x00000100; 'DELETE' = 0x00010000; 'WRITE_DAC' = 0x00040000; 'WRITE_OWNER' = 0x00080000
  'GENERIC_READ' = [uint32]2147483648; 'GENERIC_WRITE' = 0x40000000; 'MAXIMUM_ALLOWED' = 0x02000000
}

try {
  Say 'token' ([A2cMedium]::Token())
  Say 'pid' $PID
  $held = New-Object System.Collections.Generic.List[IntPtr]
  if ($Action -eq 'probe' -or $Action -eq 'hold') {
    foreach ($path in $Paths) {
      $name = Split-Path -Leaf $path
      try {
        $sddl = (Get-Acl -LiteralPath $path).GetSecurityDescriptorSddlForm([Security.AccessControl.AccessControlSections]'Owner, Access')
        Say "$name descriptor" $sddl
      } catch {
        Say "$name descriptor" "not read: $($_.Exception.GetType().Name)"
      }
      foreach ($right in $rights.Keys) {
        $openError = 0
        # the hold keeps what it gets and shares nothing; the probe shares all
        $share = 7
        if ($Action -eq 'hold') { $share = 0 }
        $handle = [A2cMedium]::Open($path, [uint32]$rights[$right], [uint32]$share, [ref]$openError)
        if ($handle -eq [A2cMedium]::Invalid) {
          Say "$name $right" "refused ($openError)"
        } elseif ($Action -eq 'hold') {
          $held.Add($handle)
          Say "$name $right" 'opened and held'
        } else {
          [void][A2cMedium]::CloseHandle($handle)
          Say "$name $right" 'opened'
        }
      }
    }
  }
  if ($Action -eq 'hold') {
    if ($Mutex) {
      $mutexHandle = [A2cMedium]::CreateMutexW([IntPtr]::Zero, $true, $Mutex)
      $mutexError = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
      if ($mutexHandle -eq [IntPtr]::Zero) { Say 'mutex' "$Mutex not created ($mutexError)" }
      elseif ($mutexError -eq 183) { Say 'mutex' "$Mutex opened, it existed already (183)" }
      else { Say 'mutex' "$Mutex created and owned" }
    }
    Say 'holding' "$($held.Count) handles"
    $deadline = (Get-Date).AddSeconds($Seconds)
    while (-not (Test-Path -LiteralPath $Stop) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
    foreach ($handle in $held) { [void][A2cMedium]::CloseHandle($handle) }
    Say 'released' "$($held.Count) handles"
  }
  if ($Action -eq 'delete') {
    foreach ($path in $Paths) {
      $deleted = [A2cMedium]::DeleteFileW($path)
      $code = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
      Say "delete $(Split-Path -Leaf $path)" $(if ($deleted) { 'deleted' } else { "refused ($code)" })
    }
  }
  if ($Action -eq 'press') {
    Add-Type -AssemblyName UIAutomationClient
    Add-Type -AssemblyName UIAutomationTypes
    try {
      Add-Type -AssemblyName UIAutomationClientsideProviders
      [System.Windows.Automation.ClientSettings]::RegisterClientSideProviderAssembly(
        [UIAutomationClientsideProviders.UIAutomationClientSideProviders].Assembly.GetName())
    } catch { Say 'providers' "not registered: $($_.Exception.Message)" }
    $condition = New-Object System.Windows.Automation.PropertyCondition(
      [System.Windows.Automation.AutomationElement]::NameProperty, $Caption)
    $dialog = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
      [System.Windows.Automation.TreeScope]::Children, $condition)
    if (-not $dialog) {
      Say 'dialog' 'not found'
    } else {
      $hwnd = [IntPtr]$dialog.Current.NativeWindowHandle
      Say 'dialog' "found, pid $($dialog.Current.ProcessId), window $hwnd"
      # the same presses the runner makes, in its order: the Invoke pattern
      # through the button's own window, then the button's BN_CLICKED
      $pattern = $null
      $buttonHwnd = [A2cMedium]::GetDlgItem($hwnd, [int]$Button)
      $invoked = $false
      if ($buttonHwnd -ne [IntPtr]::Zero) {
        try {
          $target = [System.Windows.Automation.AutomationElement]::FromHandle($buttonHwnd)
          Say 'button' "$Button '$($target.Current.Name)'"
          if ($target.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) {
            $pattern.Invoke()
            $invoked = $true
            Say 'invoke' 'returned'
          } else {
            Say 'invoke' 'no Invoke pattern'
          }
        } catch {
          Say 'invoke' "threw $($_.Exception.GetType().Name): $($_.Exception.Message)"
        }
      } else {
        Say 'button' "$Button not found"
      }
      if (-not $invoked) {
        $posted = [A2cMedium]::PostCommand($hwnd, [int]$Button)
        Say 'command' $(if ($posted -eq 0) { 'posted' } else { "PostMessage error $posted" })
      }
    }
  }
  Say 'done' 'yes'
} catch {
  Say 'failed' $_.Exception.Message
  exit 1
}
