# Is there a notification area on this runner? The app puts its tray icon up
# first and shows no window without one (Shell_NotifyIcon ADD failed with
# E_FAIL on windows-11-arm). Lists the shell processes, looks for the taskbar
# window, and tries Shell_NotifyIcon ADD/DELETE from a hidden window of our own.
# With -StartExplorer it starts explorer.exe first when none is running.
# Pure ASCII, Windows PowerShell 5.1. Throwaway (a ci/wfix-* branch).
param([switch]$StartExplorer)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Windows.Forms;

public static class UrShellProbe {
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr FindWindowW(string cls, string name);
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  struct NOTIFYICONDATAW {
    public int cbSize; public IntPtr hWnd; public int uID; public int uFlags; public int uCallbackMessage; public IntPtr hIcon;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string szTip;
    public int dwState; public int dwStateMask;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string szInfo;
    public int uVersion;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string szInfoTitle;
    public int dwInfoFlags; public Guid guidItem; public IntPtr hBalloonIcon;
  }
  [DllImport("shell32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool Shell_NotifyIconW(int message, ref NOTIFYICONDATAW data);
  [DllImport("user32.dll")] static extern IntPtr LoadIconW(IntPtr instance, IntPtr name);

  public static bool Taskbar() { return FindWindowW("Shell_TrayWnd", null) != IntPtr.Zero; }

  public static string TryTrayIcon() {
    using (Form form = new Form()) {
      IntPtr hwnd = form.Handle;
      NOTIFYICONDATAW data = new NOTIFYICONDATAW();
      data.cbSize = Marshal.SizeOf(typeof(NOTIFYICONDATAW));
      data.hWnd = hwnd; data.uID = 1; data.uFlags = 0x2 | 0x4; // NIF_ICON | NIF_TIP
      data.hIcon = LoadIconW(IntPtr.Zero, new IntPtr(32512)); data.szTip = "probe";
      bool added = Shell_NotifyIconW(0, ref data); // NIM_ADD
      int error = Marshal.GetLastWin32Error();
      if (added) Shell_NotifyIconW(2, ref data); // NIM_DELETE
      return "Shell_NotifyIcon(ADD) " + (added ? "succeeded" : "failed, last error " + error);
    }
  }
}
'@ -ReferencedAssemblies System.Windows.Forms

function Show-Shell([string]$when) {
  "---- $when"
  "session: $([System.Diagnostics.Process]::GetCurrentProcess().SessionId); user interactive: $([Environment]::UserInteractive)"
  foreach ($name in 'explorer', 'sihost', 'ShellExperienceHost', 'StartMenuExperienceHost', 'ShellHost') {
    $p = @(Get-Process -Name $name -ErrorAction SilentlyContinue)
    "  $name : $(if ($p.Count) { ($p | ForEach-Object { "pid $($_.Id) session $($_.SessionId)" }) -join '; ' } else { 'not running' })"
  }
  "  taskbar window (Shell_TrayWnd): $([UrShellProbe]::Taskbar())"
  "  $([UrShellProbe]::TryTrayIcon())"
}

$os = Get-CimInstance Win32_OperatingSystem
"os: $($os.Caption) $($os.Version) $($os.OSArchitecture)"
Show-Shell 'as the runner starts'
# the shell may still be coming up when the job starts: try again for 4 minutes
for ($i = 1; $i -le 16; $i++) {
  Start-Sleep -Seconds 15
  $result = [UrShellProbe]::TryTrayIcon()
  "  +$($i * 15)s: $result"
  if ($result -like '*succeeded*') { break }
}
if ($StartExplorer -and -not (Get-Process -Name explorer -ErrorAction SilentlyContinue)) {
  Start-Process "$env:windir\explorer.exe"
  $deadline = (Get-Date).AddSeconds(60)
  while (-not [UrShellProbe]::Taskbar() -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 2 }
  Start-Sleep -Seconds 5
  Show-Shell 'after starting explorer.exe'
}
