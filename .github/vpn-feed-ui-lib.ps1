# The tray app's own window on a runner, on top of vpn-feed-lib.ps1: started
# with --preview-ui (app/src/App/Startup.h: the signed-in shell with no
# session), read and driven through UI Automation by the app's acceptance.*
# automation ids, and captured with PrintWindow. Used by the "ui" job of
# .github/workflows/vpn-feed.yml. Dot-sourced by its steps:
#   . "$env:GITHUB_WORKSPACE\.github\vpn-feed-ui-lib.ps1"
# Runs on a disposable GitHub runner only. Nothing here synthesizes mouse or
# keyboard input: controls are invoked through their UI Automation patterns.
# Pure ASCII, Windows PowerShell 5.1.
#
# SPDX-License-Identifier: MPL-2.0

. (Join-Path $PSScriptRoot 'vpn-feed-lib.ps1')

$UrShotDir = Join-Path $env:RUNNER_TEMP 'ui-shots'
New-Item -ItemType Directory -Force $UrShotDir | Out-Null
$UrPrefsPath = Join-Path $UrAppRoot 'app_prefs.json'

# The ids the app gives the update banner's controls
# (tests/update_offer_wiring_test.go checkUpdateBannerAutomationIds).
$UrIdBanner = 'acceptance.update.banner'
$UrIdAction = 'acceptance.update.action'
$UrIdLater = 'acceptance.update.later'
$UrIdCheckNow = 'acceptance.update.check-now'
$UrIdCheckLine = 'acceptance.update.check-line'
$UrIdNavConnect = 'acceptance.nav.connect'
$UrIdNavSettings = 'acceptance.nav.settings'

$UrUiApiSource = @'
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
  [DllImport("user32.dll")] private static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int cx, int cy, uint flags);

  // Makes the window `height` tall from the top of the screen, past the
  // screen's bottom if need be, so a scrolling column is laid out whole.
  // SWP_NOSENDCHANGING keeps the size from being clamped to the monitor; with
  // SWP_NOZORDER and SWP_NOACTIVATE nothing else about the window changes.
  public static string Grow(IntPtr hwnd, int height) {
    RECT rect;
    if (!GetWindowRect(hwnd, out rect)) return "no window rectangle";
    bool ok = SetWindowPos(hwnd, IntPtr.Zero, rect.Left, 0, rect.Right - rect.Left, height, 0x0004 | 0x0010 | 0x0400);
    GetWindowRect(hwnd, out rect);
    return "resized=" + ok + ", now " + (rect.Right - rect.Left) + "x" + (rect.Bottom - rect.Top);
  }

  // The window as it draws itself (PW_RENDERFULLCONTENT), saved as a PNG.
  // Says what it did: "<width>x<height> printed=<ok> minimized=<bool>".
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
      return width + "x" + height + " at " + rect.Left + "," + rect.Top + " printed=" + ok + " minimized=" + IsIconic(hwnd);
    }
  }

  // The primary screen as it is shown, saved as a PNG.
  public static string SaveScreen(string path) {
    int width = GetSystemMetrics(0);
    int height = GetSystemMetrics(1);
    using (Bitmap bitmap = new Bitmap(width, height, PixelFormat.Format32bppArgb)) {
      using (Graphics graphics = Graphics.FromImage(bitmap)) {
        graphics.CopyFromScreen(0, 0, 0, 0, new Size(width, height));
      }
      bitmap.Save(path, ImageFormat.Png);
      return width + "x" + height;
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

function Initialize-UrUi {
  Add-Type -AssemblyName UIAutomationClient
  Add-Type -AssemblyName UIAutomationTypes
  Add-Type -AssemblyName System.Drawing
  if (-not ('UrUiApi' -as [type])) { Add-Type -TypeDefinition $UrUiApiSource -ReferencedAssemblies System.Drawing }
}

# The app's XAML window (not the tray's helper windows), or $null.
function Get-UrMainWindow([int]$processId) {
  Initialize-UrUi
  $windows = [System.Windows.Automation.AutomationElement]::RootElement.FindAll(
    [System.Windows.Automation.TreeScope]::Children, [System.Windows.Automation.Condition]::TrueCondition)
  foreach ($window in $windows) {
    try {
      if ($window.Current.ProcessId -eq $processId -and $window.Current.ClassName -eq 'WinUIDesktopWin32WindowClass') { return $window }
    } catch [System.Windows.Automation.ElementNotAvailableException] {
      # a window that closed while the list was read
    }
  }
  return $null
}

# Starts the installed tray app with --preview-ui (the connect screen) or
# --preview-ui=<destination>, and waits for its window. Returns the process.
function Start-UrPreviewApp([string]$destination = '', [int]$seconds = 120) {
  $argument = '--preview-ui'
  if ($destination) { $argument = "--preview-ui=$destination" }
  $app = Start-Process -FilePath (Join-Path $UrInstallDir 'URnetwork.exe') -ArgumentList $argument -PassThru
  $null = $app.Handle
  $deadline = (Get-Date).AddSeconds($seconds)
  $window = $null
  while (-not $window -and (Get-Date) -lt $deadline) {
    if ($app.HasExited) { throw "URnetwork.exe $argument exited by itself (code $($app.ExitCode))" }
    $window = Get-UrMainWindow $app.Id
    if (-not $window) { Start-Sleep -Milliseconds 500 }
  }
  if (-not $window) { throw "URnetwork.exe $argument (pid $($app.Id)) showed no window within $seconds s" }
  Write-Host "URnetwork.exe $argument is pid $($app.Id); its window is '$($window.Current.Name)' ($($window.Current.NativeWindowHandle))"
  return $app
}

function Find-UrId($window, [string]$automationId) {
  $condition = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $automationId)
  return $window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}

# Whether an element is there and on the screen.
function Test-UrShown($element) {
  if (-not $element) { return $false }
  try { return -not $element.Current.IsOffscreen } catch { return $false }
}

# The names of the text elements under $element, in order.
function Get-UrTexts($element) {
  $texts = New-Object System.Collections.Generic.List[string]
  if ($element) {
    $condition = New-Object System.Windows.Automation.PropertyCondition(
      [System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Text)
    foreach ($text in $element.FindAll([System.Windows.Automation.TreeScope]::Descendants, $condition)) {
      try { if ($text.Current.Name) { $texts.Add([string]$text.Current.Name) } } catch { }
    }
  }
  return ,$texts.ToArray()
}

# What the update banner shows now: whether it is open, its texts (title and
# message), its action's label and whether it can be pressed, and whether
# Later is offered.
function Get-UrBanner([int]$processId) {
  $window = Get-UrMainWindow $processId
  if (-not $window) { throw "the app (pid $processId) has no window" }
  $banner = Find-UrId $window $UrIdBanner
  $action = Find-UrId $window $UrIdAction
  $later = Find-UrId $window $UrIdLater
  $open = Test-UrShown $banner
  $texts = @()
  if ($open) {
    # the bar's own name, when it has one, and then its title and message
    $texts = @(Get-UrTexts $banner)
    if ($banner.Current.Name -and -not ($texts -contains [string]$banner.Current.Name)) { $texts = @([string]$banner.Current.Name) + $texts }
  }
  $facts = [pscustomobject]@{ Open = $open; Texts = $texts; Text = ($texts -join ' | '); Action = ''; ActionEnabled = $false
    Later = (Test-UrShown $later); LaterName = '' }
  if (Test-UrShown $action) {
    $facts.Action = [string]$action.Current.Name
    $facts.ActionEnabled = [bool]$action.Current.IsEnabled
  }
  if ($facts.Later) { $facts.LaterName = [string]$later.Current.Name }
  return $facts
}

function Show-UrBanner($banner, [string]$when) {
  Write-Host ("banner {0}: open={1} action='{2}' enabled={3} later={4} '{5}' texts: {6}" -f $when, $banner.Open, $banner.Action,
      $banner.ActionEnabled, $banner.Later, $banner.LaterName, $banner.Text)
}

# Polls the banner until $until says yes; returns it, or throws with what it
# showed last.
function Wait-UrBanner([int]$processId, [scriptblock]$until, [string]$what, [int]$seconds = 60) {
  $deadline = (Get-Date).AddSeconds($seconds)
  $banner = $null
  do {
    $banner = Get-UrBanner $processId
    if (& $until $banner) { return $banner }
    Start-Sleep -Milliseconds 500
  } while ((Get-Date) -lt $deadline)
  Show-UrBanner $banner 'at the end of the wait'
  throw "the banner did not come to show $what within $seconds s"
}

# Presses a control through its Invoke pattern, as a click does.
function Invoke-UrId([int]$processId, [string]$automationId) {
  $window = Get-UrMainWindow $processId
  $element = Find-UrId $window $automationId
  if (-not (Test-UrShown $element)) { throw "$automationId is not on the screen" }
  if (-not $element.Current.IsEnabled) { throw "$automationId is disabled" }
  Write-Host "invoke $automationId ('$($element.Current.Name)')"
  $element.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}

# Selects a navigation item by its automation id. Says whether it was found.
# At the width the window opens with, the navigation pane is closed and its
# items are not in the tree: the pane's own button opens it first, as a user
# does, and selecting an item closes it again.
function Select-UrNav([int]$processId, [string[]]$automationIds, [int]$seconds = 20) {
  $deadline = (Get-Date).AddSeconds($seconds)
  $opened = $false
  do {
    $window = Get-UrMainWindow $processId
    foreach ($automationId in $automationIds) {
      $item = Find-UrId $window $automationId
      if ($item) {
        $pattern = $null
        if ($item.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern, [ref]$pattern)) {
          $pattern.Select()
        } else {
          $item.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
        }
        Write-Host "navigated by $automationId ('$($item.Current.Name)')"
        Start-Sleep -Seconds 2
        # a pane that stayed open over the page is closed again
        $toggle = Find-UrId (Get-UrMainWindow $processId) 'TogglePaneButton'
        if ($opened -and (Test-UrShown $toggle) -and $toggle.Current.Name -match 'Close') {
          $toggle.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
          Start-Sleep -Milliseconds 800
        }
        return $true
      }
    }
    if (-not $opened) {
      $toggle = Find-UrId $window 'TogglePaneButton'
      if (Test-UrShown $toggle) {
        Write-Host "opening the navigation pane ('$($toggle.Current.Name)')"
        $toggle.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
        $opened = $true
        Start-Sleep -Milliseconds 1000
        continue
      }
    }
    Start-Sleep -Milliseconds 500
  } while ((Get-Date) -lt $deadline)
  return $false
}

# The window's controls that have an automation id or a name, for a failure's
# output: what UI Automation sees.
function Show-UrUiTree([int]$processId, [int]$most = 400) {
  $window = Get-UrMainWindow $processId
  if (-not $window) { Write-Host "ui: the app has no window"; return }
  $all = $window.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
  $shown = 0
  foreach ($element in $all) {
    try {
      $current = $element.Current
      if (-not $current.AutomationId -and -not $current.Name) { continue }
      $name = [string]$current.Name
      if ($name.Length -gt 100) { $name = $name.Substring(0, 100) + '...' }
      Write-Host ("ui: {0} id='{1}' name='{2}' offscreen={3} enabled={4}" -f $current.ControlType.ProgrammaticName.Replace('ControlType.', ''),
          $current.AutomationId, $name, $current.IsOffscreen, $current.IsEnabled)
      $shown++
      if ($shown -ge $most) { Write-Host "ui: (more than $most; the rest is left out)"; break }
    } catch { }
  }
  Write-Host "ui: $($all.Count) elements under the window, $shown listed"
}

# Saves the app's window as <name>.png, and the screen as <name>.screen.png,
# under ui-shots. Says how many colours each holds: a picture of one or two
# was not drawn.
function Save-UrShot([int]$processId, [string]$name) {
  Initialize-UrUi
  $window = Get-UrMainWindow $processId
  if (-not $window) { Write-Host "screenshot ${name}: the app has no window"; return }
  $path = Join-Path $UrShotDir "$name.png"
  $result = [UrUiApi]::SaveWindow([IntPtr]$window.Current.NativeWindowHandle, $path)
  $colours = 0
  if (Test-Path $path) { $colours = [UrUiApi]::Colours($path) }
  Write-Host "screenshot $name.png: $result, $colours colours in 4096 samples"
  try {
    $screen = Join-Path $UrShotDir "$name.screen.png"
    $size = [UrUiApi]::SaveScreen($screen)
    Write-Host "screenshot $name.screen.png: $size, $([UrUiApi]::Colours($screen)) colours in 4096 samples"
  } catch {
    Write-Host "screenshot $name.screen.png: not taken ($($_.Exception.Message))"
  }
}

# Makes the app's window tall enough to lay its scrolling column out whole
# (app/src/App/Startup.h says how a whole column is captured), so that what is
# below the fold at the size the window opens with can be read and captured.
function Expand-UrWindow([int]$processId, [int]$height = 1500) {
  Initialize-UrUi
  $window = Get-UrMainWindow $processId
  if (-not $window) { throw "the app (pid $processId) has no window" }
  Write-Host "window: $([UrUiApi]::Grow([IntPtr]$window.Current.NativeWindowHandle, $height))"
  Start-Sleep -Seconds 2
}

# Everything UI Automation sees under the element with $automationId.
function Show-UrSubtree([int]$processId, [string]$automationId) {
  $window = Get-UrMainWindow $processId
  $element = Find-UrId $window $automationId
  if (-not $element) { Write-Host "ui: no element with id $automationId"; return }
  Write-Host ("ui: {0} id='{1}' name='{2}' offscreen={3}" -f $element.Current.ControlType.ProgrammaticName.Replace('ControlType.', ''),
      $element.Current.AutomationId, $element.Current.Name, $element.Current.IsOffscreen)
  foreach ($child in $element.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)) {
    try {
      Write-Host ("ui:   {0} id='{1}' name='{2}' offscreen={3} enabled={4}" -f $child.Current.ControlType.ProgrammaticName.Replace('ControlType.', ''),
          $child.Current.AutomationId, $child.Current.Name, $child.Current.IsOffscreen, $child.Current.IsEnabled)
    } catch { }
  }
}

# The developer screen's update line, or '' when it is not shown.
function Get-UrCheckLine([int]$processId) {
  $window = Get-UrMainWindow $processId
  $line = Find-UrId $window $UrIdCheckLine
  if (-not $line) { return '' }
  return [string]$line.Current.Name
}

# Waits for an app log line that matches $pattern, from line $from on, and
# returns the matching lines; throws with what the update checker logged.
function Wait-UrAppLog([string]$pattern, [int]$from, [string]$what, [int]$seconds = 60) {
  $deadline = (Get-Date).AddSeconds($seconds)
  do {
    $found = Get-UrAppLogLines $pattern $from
    if ($found.Count -gt 0) {
      $found | ForEach-Object { Write-Host "app log: $_" }
      return ,$found
    }
    Start-Sleep -Milliseconds 500
  } while ((Get-Date) -lt $deadline)
  (Get-UrAppLogLines 'update: ' $from) | ForEach-Object { Write-Host "app log: $_" }
  throw "the app did not log $what within $seconds s (no line matches $pattern)"
}

# The tray app's preferences file, as text, and its keys.
function Get-UrPrefs {
  if (-not (Test-Path -LiteralPath $UrPrefsPath)) { return [pscustomobject]@{ Text = ''; Keys = @() } }
  $text = Get-Content -Raw -LiteralPath $UrPrefsPath
  $keys = @(($text | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $_.Name })
  return [pscustomobject]@{ Text = $text.Trim(); Keys = $keys }
}

# Waits until the stand-in's clock (Set-UrFeedStandIn -Running) has passed
# $unix by $margin seconds.
function Wait-UrStandInClock($state, [long]$unix, [int]$margin = 5) {
  $now = Get-UrStandInClock $state
  $wait = $unix + $margin - $now
  Write-Host "the stand-in's date is $(Format-UrUnix $now); waiting $wait s for it to pass $(Format-UrUnix $unix) by $margin s"
  if ($wait -gt 0) { Start-Sleep -Seconds $wait }
  $now = Get-UrStandInClock $state
  if ($now -lt $unix + $margin) { throw "the stand-in's date is $(Format-UrUnix $now), still short of $(Format-UrUnix $unix)" }
  Write-Host "the stand-in's date is $(Format-UrUnix $now)"
}
