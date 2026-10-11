# The app's pages on a runner, for feat/desktop-ui-polish-upstream: the
# portable zip as a user receives it, extracted to a fresh folder, started with
# --preview-ui=<destination> (app/src/App/Startup.h: the signed-in shell with
# no session), one launch per page. Read and pressed through UI Automation
# only, and captured with PrintWindow (PW_RENDERFULLCONTENT) at the size the
# window opens with. Nothing here synthesizes mouse or keyboard input.
# Throwaway (a ci/pr4-ui-* branch of the fork); never merged.
# Pure ASCII, Windows PowerShell 5.1.
#
# SPDX-License-Identifier: MPL-2.0

param([Parameter(Mandatory = $true)] [string]$ZipDir)
$ErrorActionPreference = 'Stop'

$ShotDir = Join-Path $env:RUNNER_TEMP 'ui-shots'
New-Item -ItemType Directory -Force $ShotDir | Out-Null

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class UrUiApi {
  [StructLayout(LayoutKind.Sequential)]
  public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
  [DllImport("user32.dll")] private static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] private static extern bool IsIconic(IntPtr hwnd);
  [DllImport("user32.dll")] private static extern int GetSystemMetrics(int index);
  [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr hwnd);

  public static string Screen() {
    return GetSystemMetrics(0) + "x" + GetSystemMetrics(1);
  }

  // The window as it draws itself (PW_RENDERFULLCONTENT), saved as a PNG.
  public static string SaveWindow(IntPtr hwnd, string path) {
    RECT rect;
    if (!GetWindowRect(hwnd, out rect)) return "no window rectangle";
    int width = rect.Right - rect.Left;
    int height = rect.Bottom - rect.Top;
    if (width <= 0 || height <= 0) return "empty window " + width + "x" + height;
    using (Bitmap bitmap = new Bitmap(width, height, PixelFormat.Format32bppArgb)) {
      bool ok;
      using (Graphics graphics = Graphics.FromImage(bitmap)) {
        IntPtr hdc = graphics.GetHdc();
        ok = PrintWindow(hwnd, hdc, 2);
        graphics.ReleaseHdc(hdc);
      }
      bitmap.Save(path, ImageFormat.Png);
      return width + "x" + height + " at " + rect.Left + "," + rect.Top + " dpi=" + GetDpiForWindow(hwnd) +
             " printed=" + ok + " minimized=" + IsIconic(hwnd);
    }
  }

  // How many different colours a grid of 64 by 64 samples of the picture
  // holds: one or two means nothing was drawn.
  public static int Colours(string path) {
    HashSet<int> seen = new HashSet<int>();
    using (Bitmap bitmap = new Bitmap(path)) {
      for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
          seen.Add(bitmap.GetPixel(x * (bitmap.Width - 1) / 63, y * (bitmap.Height - 1) / 63).ToArgb());
        }
      }
    }
    return seen.Count;
  }
}
'@

# ---- the app, as the portable zip ships it ----------------------------------
$zip = Get-ChildItem -LiteralPath $ZipDir -Filter 'URnetwork-*-portable.zip' | Select-Object -First 1
if (-not $zip) { throw "no URnetwork-*-portable.zip in $ZipDir" }
$AppDir = Join-Path $env:RUNNER_TEMP 'app'
New-Item -ItemType Directory -Force $AppDir | Out-Null
& "$env:SystemRoot\System32\tar.exe" -xf $zip.FullName -C $AppDir
if ($LASTEXITCODE -ne 0) { throw "could not extract $($zip.Name)" }
$Exe = Join-Path $AppDir 'URnetwork.exe'
if (-not (Test-Path -LiteralPath $Exe)) { throw "the zip has no URnetwork.exe at its root" }
"app: $($zip.Name), $((Get-ChildItem -Recurse -File $AppDir).Count) files, URnetwork.exe $((Get-Item $Exe).VersionInfo.FileVersion)"
"screen: $([UrUiApi]::Screen()); os: $([Environment]::OSVersion.VersionString)"
$theme = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' -ErrorAction SilentlyContinue
"apps use the light theme: $($theme.AppsUseLightTheme)"

# ---- UI Automation -----------------------------------------------------------
function Get-MainWindow([int]$processId) {
  $windows = [System.Windows.Automation.AutomationElement]::RootElement.FindAll(
    [System.Windows.Automation.TreeScope]::Children, [System.Windows.Automation.Condition]::TrueCondition)
  foreach ($window in $windows) {
    try {
      if ($window.Current.ProcessId -eq $processId -and $window.Current.ClassName -eq 'WinUIDesktopWin32WindowClass') { return $window }
    } catch [System.Windows.Automation.ElementNotAvailableException] { }
  }
  return $null
}

function Find-Id($window, [string]$automationId) {
  $condition = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $automationId)
  return $window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}

function Test-Shown($element) {
  if (-not $element) { return $false }
  try { return -not $element.Current.IsOffscreen } catch { return $false }
}

# The texts on screen, in tree order: what the page says, for the log.
function Show-Texts([int]$processId, [string]$name, [int]$most = 60) {
  $window = Get-MainWindow $processId
  if (-not $window) { "texts ${name}: no window"; return }
  $condition = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Text)
  $texts = New-Object System.Collections.Generic.List[string]
  foreach ($text in $window.FindAll([System.Windows.Automation.TreeScope]::Descendants, $condition)) {
    try {
      if ($text.Current.Name -and -not $text.Current.IsOffscreen) {
        $value = [string]$text.Current.Name
        if ($value.Length -gt 90) { $value = $value.Substring(0, 90) + '...' }
        $texts.Add($value)
      }
    } catch { }
  }
  $shown = $texts | Select-Object -First $most
  "texts ${name} ($($texts.Count) on screen): " + ($shown -join ' | ')
}

function Save-Shot([int]$processId, [string]$name) {
  $window = Get-MainWindow $processId
  if (-not $window) { throw "screenshot ${name}: the app has no window" }
  $path = Join-Path $ShotDir "$name.png"
  $result = [UrUiApi]::SaveWindow([IntPtr]$window.Current.NativeWindowHandle, $path)
  $colours = 0
  if (Test-Path -LiteralPath $path) { $colours = [UrUiApi]::Colours($path) }
  Write-Host "screenshot ${name}.png: $result, $colours colours in 4096 samples"
  return $colours
}

function Start-Preview([string]$destination, [hashtable]$environment) {
  $argument = '--preview-ui'
  if ($destination) { $argument = "--preview-ui=$destination" }
  foreach ($key in $environment.Keys) { [Environment]::SetEnvironmentVariable($key, $environment[$key], 'Process') }
  try {
    $app = Start-Process -FilePath $Exe -ArgumentList $argument -WorkingDirectory $AppDir -PassThru
  } finally {
    foreach ($key in $environment.Keys) { [Environment]::SetEnvironmentVariable($key, $null, 'Process') }
  }
  $null = $app.Handle
  $deadline = (Get-Date).AddSeconds(120)
  $window = $null
  while (-not $window -and (Get-Date) -lt $deadline) {
    if ($app.HasExited) { throw "URnetwork.exe $argument exited by itself (code $($app.ExitCode))" }
    $window = Get-MainWindow $app.Id
    if (-not $window) { Start-Sleep -Milliseconds 500 }
  }
  if (-not $window) { throw "URnetwork.exe $argument (pid $($app.Id)) showed no window within 120 s" }
  $envText = ($environment.Keys | ForEach-Object { "$_=$($environment[$_])" }) -join ' '
  Write-Host "URnetwork.exe $argument $envText is pid $($app.Id); window '$($window.Current.Name)'"
  return $app
}

function Stop-App($app) {
  if (-not $app.HasExited) { Stop-Process -Id $app.Id -Force }
  $null = $app.WaitForExit(20000)
  Get-Process URnetwork -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$AppDir*" } | Stop-Process -Force
  Start-Sleep -Seconds 2
}

# ---- the pages ---------------------------------------------------------------
# Connect is the page #4 changed most (its activity feed, panes and hero);
# URNETWORK_PREVIEW_SAMPLE fills its panes with the app's own synthetic rows
# (ConnectPage::PreviewSampleActive), since a preview has no live data.
$pages = @(
  @{ Name = '01-connect'; Destination = ''; Environment = @{}; Required = $true },
  @{ Name = '02-connect-sample'; Destination = ''; Environment = @{ URNETWORK_PREVIEW_SAMPLE = '1' }; Required = $false },
  @{ Name = '03-network'; Destination = 'network'; Environment = @{}; Required = $false },
  @{ Name = '04-account'; Destination = 'account'; Environment = @{}; Required = $true; Sessions = $true },
  @{ Name = '06-wallet'; Destination = 'wallet'; Environment = @{}; Required = $false },
  @{ Name = '07-settings'; Destination = 'settings'; Environment = @{}; Required = $true },
  @{ Name = '08-developer'; Destination = 'developer'; Environment = @{}; Required = $true },
  @{ Name = '09-support'; Destination = 'support'; Environment = @{}; Required = $false }
)

$results = New-Object System.Collections.Generic.List[string]
$failed = New-Object System.Collections.Generic.List[string]
foreach ($page in $pages) {
  "---- $($page.Name)"
  $app = $null
  try {
    $app = Start-Preview $page.Destination $page.Environment
    # the destination's entrance and the first layout pass settle first
    Start-Sleep -Seconds 6
    $colours = Save-Shot $app.Id $page.Name
    Show-Texts $app.Id $page.Name
    if ($colours -le 2) { throw "$($page.Name) was not drawn ($colours colours)" }
    $results.Add("$($page.Name).png ($colours colours)")
    if ($page.Sessions) {
      # Account > Sessions: the row's own Invoke, as a press through UI Automation
      $window = Get-MainWindow $app.Id
      $row = Find-Id $window 'acceptance.account.sessions'
      if (-not (Test-Shown $row)) {
        "NOT REACHED: the Account page's Sessions row (acceptance.account.sessions) is not on the screen"
        $failed.Add('05-account-sessions (row not on screen)')
      } else {
        "invoke acceptance.account.sessions ('$($row.Current.Name)')"
        $row.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
        Start-Sleep -Seconds 5
        $refresh = Find-Id (Get-MainWindow $app.Id) 'acceptance.sessions.refresh'
        "the Sessions page's Refresh (acceptance.sessions.refresh) shown: $(Test-Shown $refresh)"
        $colours = Save-Shot $app.Id '05-account-sessions'
        Show-Texts $app.Id '05-account-sessions'
        if ($colours -le 2) { throw "05-account-sessions was not drawn ($colours colours)" }
        $results.Add("05-account-sessions.png ($colours colours)")
      }
    }
  } catch {
    "FAILED $($page.Name): $($_.Exception.Message)"
    $failed.Add("$($page.Name) ($($_.Exception.Message))")
    if ($app -and -not $app.HasExited) {
      try { [void](Save-Shot $app.Id "$($page.Name)-failure") } catch { }
    }
  } finally {
    if ($app) { Stop-App $app }
  }
}

"==== captured: $($results.Count)"
$results | ForEach-Object { "  $_" }
"==== not captured: $($failed.Count)"
$failed | ForEach-Object { "  $_" }
$requiredMissing = @($pages | Where-Object { $_.Required -and -not ($results -match "^$($_.Name)\.png") })
if ($requiredMissing.Count -gt 0) {
  throw "required pages not captured: $(($requiredMissing | ForEach-Object { $_.Name }) -join ', ')"
}
