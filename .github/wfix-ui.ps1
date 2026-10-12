# The app's pages on a runner (ci/pr4-ui-1's harness, reused): the portable zip
# as a user receives it, extracted to a fresh folder, started with
# --preview-ui=<destination> (app/src/App/Startup.h: the signed-in shell with
# no session), one launch per page. Read and pressed through UI Automation
# only, and captured with PrintWindow (PW_RENDERFULLCONTENT) at the size the
# window opens with. Nothing here synthesizes mouse or keyboard input.
# Throwaway (a ci/wfix-* branch of the fork); never merged.
# Pure ASCII, Windows PowerShell 5.1.
#
# SPDX-License-Identifier: MPL-2.0

# -BeforeZipDir: a second portable zip (upstream main's build). When given, the
# scenarios below (the Advanced-mode fold, the App split rules sheet) run on
# both builds after the pages. -ScenariosOnly skips the pages.
param([Parameter(Mandatory = $true)] [string]$ZipDir, [string]$BeforeZipDir = '',
      [switch]$ScenariosOnly)
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
  [DllImport("user32.dll")] private static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
  [DllImport("user32.dll")] private static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int cx, int cy, uint flags);

  public static string Screen() {
    return GetSystemMetrics(0) + "x" + GetSystemMetrics(1);
  }

  // Moves and sizes the window (SWP_NOZORDER | SWP_NOACTIVATE |
  // SWP_NOSENDCHANGING, so it may extend past the monitor). A window API, not
  // input: nothing is clicked or typed.
  public static bool Place(IntPtr hwnd, int x, int y, int width, int height) {
    SetThreadDpiAwarenessContext(new IntPtr(-4));
    return SetWindowPos(hwnd, IntPtr.Zero, x, y, width, height, 0x0004 | 0x0010 | 0x0400);
  }

  // The window as it draws itself (PW_RENDERFULLCONTENT), saved as a PNG. The
  // rectangle is read per-monitor aware (-4), or a scaled display would hand a
  // DPI-unaware caller a smaller rectangle and clip the capture.
  public static string SaveWindow(IntPtr hwnd, string path) {
    SetThreadDpiAwarenessContext(new IntPtr(-4));
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
$os = Get-CimInstance Win32_OperatingSystem
"edition: $($os.Caption) $($os.Version) $($os.OSArchitecture); Segoe Fluent Icons installed: $(Test-Path "$env:windir\Fonts\SegoeIcons.ttf"); Segoe MDL2 Assets installed: $(Test-Path "$env:windir\Fonts\segmdl2.ttf")"
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
  @{ Name = '09-support'; Destination = 'support'; Environment = @{}; Required = $false },
  @{ Name = '10-seedphrase'; Destination = 'seedphrase'; Environment = @{}; Required = $false },
  @{ Name = '11-leaderboard'; Destination = 'leaderboard'; Environment = @{}; Required = $false }
)
if ($ScenariosOnly) { $pages = @() }

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

# ---- the folded Advanced Mode toggle (fix/beta-line-defects-upstream) -------
# Settings builds the Advanced mode switch twice: in the Device pane (two panes,
# 900 dip and wider) and as a fold copy at the foot of General (one pane). Flip
# the Device pane's switch through UI Automation at 1000 wide, narrow the window
# to 640 so the fold copy shows, and read the fold copy. It must read what the
# Device pane's switch was flipped to. Run on each build given.
if (-not $BeforeZipDir) { return }

function Expand-App([string]$zipDir, [string]$name) {
  $zip = Get-ChildItem -LiteralPath $zipDir -Filter 'URnetwork-*-portable.zip' | Select-Object -First 1
  if (-not $zip) { throw "no URnetwork-*-portable.zip in $zipDir" }
  $dir = Join-Path $env:RUNNER_TEMP $name
  New-Item -ItemType Directory -Force $dir | Out-Null
  & "$env:SystemRoot\System32\tar.exe" -xf $zip.FullName -C $dir
  if ($LASTEXITCODE -ne 0) { throw "could not extract $($zip.Name)" }
  Write-Host "$name : $($zip.Name), URnetwork.exe $((Get-Item (Join-Path $dir 'URnetwork.exe')).VersionInfo.FileVersion)"
  return $dir
}

function Find-Toggles($window, [string]$name) {
  $toggle = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::IsTogglePatternAvailableProperty, $true)
  $named = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::NameProperty, $name)
  $both = New-Object System.Windows.Automation.AndCondition($toggle, $named)
  return @($window.FindAll([System.Windows.Automation.TreeScope]::Descendants, $both))
}

function Get-ToggleState($element) {
  return [string]$element.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Current.ToggleState
}

function Test-AdvancedFold([string]$label) {
  # the mode starts Off: the app keeps it in app_prefs.json (Common/Paths.cpp)
  $prefs = Join-Path $env:LOCALAPPDATA 'URnetwork\app\app_prefs.json'
  if (Test-Path -LiteralPath $prefs) { Remove-Item -Force -LiteralPath $prefs }
  $app = Start-Preview 'settings' @{}
  try {
    $hwnd = [IntPtr](Get-MainWindow $app.Id).Current.NativeWindowHandle
    [void][UrUiApi]::Place($hwnd, 0, 0, 1000, 740)
    Start-Sleep -Seconds 4
    $wide = @(Find-Toggles (Get-MainWindow $app.Id) 'Advanced mode')
    Write-Host "advanced-fold ${label}: at 1000 wide, $($wide.Count) 'Advanced mode' switch(es) on the page, reading $(($wide | ForEach-Object { Get-ToggleState $_ }) -join ',')"
    [void](Save-Shot $app.Id "adv-$label-1-wide")
    if ($wide.Count -ne 1) { throw "want the Device pane's switch alone at 1000 wide, found $($wide.Count)" }
    $wide[0].GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Toggle()
    Start-Sleep -Seconds 4
    $device = Get-ToggleState (@(Find-Toggles (Get-MainWindow $app.Id) 'Advanced mode')[0])
    Write-Host "advanced-fold ${label}: flipped the Device pane's switch through UI Automation (TogglePattern); it reads $device"
    [void](Save-Shot $app.Id "adv-$label-2-wide-flipped")
    [void][UrUiApi]::Place($hwnd, 0, 0, 640, 1600)
    Start-Sleep -Seconds 5
    $narrow = @(Find-Toggles (Get-MainWindow $app.Id) 'Advanced mode')
    $states = @($narrow | ForEach-Object { Get-ToggleState $_ })
    Write-Host "advanced-fold ${label}: at 640 wide, $($narrow.Count) 'Advanced mode' switch(es) on the page, reading $($states -join ',')"
    [void](Save-Shot $app.Id "adv-$label-3-narrow-fold-copy")
    if ($narrow.Count -ne 1) { throw "want the fold copy alone at 640 wide, found $($narrow.Count)" }
    return @{ Device = $device; Fold = $states[0] }
  } finally {
    Stop-App $app
  }
}

$verdicts = @{}
$appDirs = @{}
$problems = New-Object System.Collections.Generic.List[string]
foreach ($build in @(@{ Label = 'before'; Zip = $BeforeZipDir }, @{ Label = 'after'; Zip = $ZipDir })) {
  "---- advanced-fold $($build.Label)"
  $script:AppDir = Expand-App $build.Zip "app-$($build.Label)"
  $script:Exe = Join-Path $script:AppDir 'URnetwork.exe'
  $appDirs[$build.Label] = $script:AppDir
  try {
    $verdicts[$build.Label] = Test-AdvancedFold $build.Label
  } catch {
    "advanced-fold $($build.Label) FAILED: $($_.Exception.Message)"
    $verdicts[$build.Label] = @{ Device = '?'; Fold = '?' }
  }
}
"==== advanced-fold: before (upstream main): Device pane $($verdicts['before'].Device), fold copy $($verdicts['before'].Fold); after: Device pane $($verdicts['after'].Device), fold copy $($verdicts['after'].Fold)"
if ($verdicts['after'].Device -ne 'On' -or $verdicts['after'].Fold -ne 'On') {
  $problems.Add("after the fix, the fold copy must read On once the Device pane's switch is flipped On")
}

# ---- the App split rules sheet with two rules differing only in case -------
# The SDK keeps the app rules in its local state, <space>\.by\.block_action_
# overrides (sdk local_state.go), a JSON list of BlockActionOverride. Write two
# rules for one app whose paths differ only in case, open Settings, press
# "Manage apps" through UI Automation (InvokePattern), and read whether the
# sheet opened and how many rows name the app.
# -Single writes only the first rule: the control, a list the sheet opens on.
function Set-CaseVariantRules([switch]$Single) {
  $storage = Join-Path $env:LOCALAPPDATA 'URnetwork\app\storage'
  $dirs = @(Get-ChildItem -LiteralPath $storage -Recurse -Directory -Force -Filter '.by' -ErrorAction SilentlyContinue)
  if ($dirs.Count -eq 0) { throw "no SDK local state (.by) under $storage" }
  $first = '{"OverrideId":"0b2f6c1e-5d4a-4c3b-9a8e-7f6d5c4b3a21","AppIds":["C:\\Tools\\Demo.exe"],"RouteOverride":{"Local":true}}'
  $second = '{"OverrideId":"1c3e7d2f-6e5b-4d4c-8b9f-8e7d6c5b4a32","AppIds":["c:\\tools\\demo.exe"],"RouteOverride":{"Local":false}}'
  $json = '[' + $first + ']'
  $what = 'one app rule, C:\Tools\Demo.exe,'
  if (-not $Single) {
    $json = '[' + $first + ',' + $second + ']'
    $what = 'two app rules, C:\Tools\Demo.exe and c:\tools\demo.exe,'
  }
  foreach ($dir in $dirs) {
    [System.IO.File]::WriteAllText((Join-Path $dir.FullName '.block_action_overrides'), $json)
  }
  Write-Host "split-rules: wrote $what into $($dirs.Count) local state dir(s): $(($dirs | ForEach-Object { $_.FullName }) -join '; ')"
}

function Get-Texts($window) {
  $condition = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Text)
  $names = New-Object System.Collections.Generic.List[string]
  foreach ($text in $window.FindAll([System.Windows.Automation.TreeScope]::Descendants, $condition)) {
    try { $names.Add([string]$text.Current.Name) } catch { }
  }
  return ,$names
}

function Test-SplitRulesSheet([string]$label) {
  $app = Start-Preview 'settings' @{}
  try {
    Start-Sleep -Seconds 2
    $named = New-Object System.Windows.Automation.PropertyCondition(
      [System.Windows.Automation.AutomationElement]::NameProperty, 'Manage apps')
    $button = (Get-MainWindow $app.Id).FindFirst([System.Windows.Automation.TreeScope]::Descendants, $named)
    if (-not $button) { throw "no 'Manage apps' button on the Settings page" }
    $button.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    Start-Sleep -Seconds 6
    $texts = Get-Texts (Get-MainWindow $app.Id)
    $rows = @($texts | Where-Object { $_ -eq 'Demo.exe' }).Count
    $configured = @($texts | Where-Object { $_ -eq 'Configured' }).Count
    $open = $configured -gt 0
    Write-Host "split-rules ${label}: pressed 'Manage apps' (InvokePattern); sheet open: $open ('Configured' header: $configured); rows named Demo.exe: $rows"
    [void](Save-Shot $app.Id "split-$label-after-manage-apps")
    return @{ Open = $open; Rows = $rows }
  } finally {
    Stop-App $app
  }
}

$split = @{}
# the control first: upstream main's sheet opens on one rule for the app
foreach ($case in @(@{ Label = 'before-control'; Build = 'before'; Single = $true },
                    @{ Label = 'before'; Build = 'before'; Single = $false },
                    @{ Label = 'after'; Build = 'after'; Single = $false })) {
  "---- split-rules $($case.Label)"
  $script:AppDir = $appDirs[$case.Build]
  $script:Exe = Join-Path $script:AppDir 'URnetwork.exe'
  try {
    Set-CaseVariantRules -Single:$case.Single
    $split[$case.Label] = Test-SplitRulesSheet $case.Label
  } catch {
    "split-rules $($case.Label) FAILED: $($_.Exception.Message)"
    $split[$case.Label] = @{ Open = '?'; Rows = '?' }
  }
}
"==== split-rules: before (upstream main), one rule (control): sheet open $($split['before-control'].Open), Demo.exe rows $($split['before-control'].Rows); before, the pair: sheet open $($split['before'].Open), Demo.exe rows $($split['before'].Rows); after, the pair: sheet open $($split['after'].Open), Demo.exe rows $($split['after'].Rows)"
if ($split['before-control'].Open -ne $true) {
  $problems.Add("control: upstream main's sheet must open on one rule, or 'not open' says nothing about the pair")
}
if ($split['after'].Open -ne $true -or $split['after'].Rows -ne 1) {
  $problems.Add("after the fix, the sheet must open with one row for the app the two rules name")
}
if ($problems.Count -gt 0) { throw ($problems -join '; ') }
