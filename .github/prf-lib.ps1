# Helpers for .github/workflows/prf-scenarios.yml, PR F's runner legs, on top
# of wild-update-lib.ps1 (the in-the-wild run's helpers, and through it A2b's
# vpn-*-lib.ps1, all unchanged). Dot-sourced by every leg:
#   . "$env:GITHUB_WORKSPACE\.github\prf-lib.ps1"
# Throwaway: it lives on ci/prf-* branches of the fork only and is never
# merged. Runs on a disposable GitHub runner only. Windows are read and
# pressed through UI Automation and captured with PrintWindow; nothing here
# synthesizes mouse or keyboard input. It publishes nothing and creates no
# release or tag: the stand-in for GitHub serves the feed's own list, with one
# release added, on this runner alone.
# Pure ASCII, Windows PowerShell 5.1.
#
# SPDX-License-Identifier: MPL-2.0

. (Join-Path $PSScriptRoot 'wild-update-lib.ps1')

# 'before': upstream main's build, the controls. 'after': PR F's build.
$PrfMode = [string]$env:PRF_MODE
if ($PrfMode -ne 'before' -and $PrfMode -ne 'after') { throw "PRF_MODE is '$PrfMode', want before or after" }
$PrfOs = [string]$env:ImageOS

# The banners' automation ids. The connect screen's are upstream main's
# (tests/update_offer_wiring_test.go checkUpdateBannerAutomationIds); the
# sign-in screen's are PR F's.
$PrfIdsConnect = [pscustomobject]@{ Name = 'the connect screen'; Banner = 'acceptance.update.banner'
  Action = 'acceptance.update.action'; Later = 'acceptance.update.later' }
$PrfIdsSignedOut = [pscustomobject]@{ Name = 'the sign-in screen'; Banner = 'acceptance.update.signed-out-banner'
  Action = 'acceptance.update.signed-out-action'; Later = 'acceptance.update.signed-out-later' }

# The report's line about the VPN (ConnectPage.cpp kReconnect).
$PrfReconnect = 'The VPN was disconnected for the update; connect again to protect this device.'

$PrfWinSource = @'
using System;
using System.Runtime.InteropServices;

public static class PrfWin {
  [StructLayout(LayoutKind.Sequential)]
  public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
  [DllImport("user32.dll")] private static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] private static extern int GetSystemMetrics(int index);
  [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr hwnd);

  // Moves and sizes the window (SWP_NOZORDER | SWP_NOACTIVATE |
  // SWP_NOSENDCHANGING, so it is not clamped to the monitor). A window API,
  // not input: nothing is clicked or typed.
  public static string Place(IntPtr hwnd, int x, int y, int width, int height) {
    bool ok = SetWindowPos(hwnd, IntPtr.Zero, x, y, width, height, 0x0004 | 0x0010 | 0x0400);
    return "placed=" + ok + ", now " + Describe(hwnd);
  }

  public static string Describe(IntPtr hwnd) {
    RECT rect;
    if (!GetWindowRect(hwnd, out rect)) return "no window rectangle";
    return (rect.Right - rect.Left) + "x" + (rect.Bottom - rect.Top) + " at " + rect.Left + "," + rect.Top +
           " dpi=" + GetDpiForWindow(hwnd);
  }

  public static int Left(IntPtr hwnd) {
    RECT rect;
    return GetWindowRect(hwnd, out rect) ? rect.Left : 0;
  }

  public static string Screen() {
    return GetSystemMetrics(0) + "x" + GetSystemMetrics(1);
  }
}
'@

function Initialize-PrfWin {
  Initialize-UrUi
  if (-not ('PrfWin' -as [type])) { Add-Type -TypeDefinition $PrfWinSource }
}

function Get-PrfHwnd([int]$processId) {
  $window = Get-UrMainWindow $processId
  if (-not $window) { throw "the app (pid $processId) has no window" }
  return [IntPtr]$window.Current.NativeWindowHandle
}

# Sizes the app's window to $width by $height, keeping its left edge, its top
# at the screen's top; waits for the layout to settle.
function Set-PrfWindowSize([int]$processId, [int]$width, [int]$height) {
  Initialize-PrfWin
  $hwnd = Get-PrfHwnd $processId
  Write-Host "window: $([PrfWin]::Place($hwnd, [PrfWin]::Left($hwnd), 0, $width, $height))"
  Start-Sleep -Seconds 3
}

function Get-PrfWindowSize([int]$processId) {
  Initialize-PrfWin
  return [PrfWin]::Describe((Get-PrfHwnd $processId))
}

# A rectangle relative to the window's top left, or $null.
function Get-PrfRect($element, $frame) {
  if (-not $element) { return $null }
  try {
    $r = $element.Current.BoundingRectangle
    if ($r.IsEmpty) { return $null }
    return [pscustomobject]@{ Top = [int]([double]$r.Top - [double]$frame.Top); Bottom = [int]([double]$r.Bottom - [double]$frame.Top)
      Left = [int]([double]$r.Left - [double]$frame.Left); Right = [int]([double]$r.Right - [double]$frame.Left)
      Height = [int]([double]$r.Height); Offscreen = [bool]$element.Current.IsOffscreen }
  } catch { return $null }
}

function Format-PrfRect($rect) {
  if ($null -eq $rect) { return 'none' }
  return "y={0}..{1} (h {2}) x={3}..{4} offscreen={5}" -f $rect.Top, $rect.Bottom, $rect.Height, $rect.Left, $rect.Right, $rect.Offscreen
}

# What a banner shows now ($ids: $PrfIdsConnect or $PrfIdsSignedOut): whether
# it is on the screen, its texts, its action's label and state, and Later.
function Get-PrfBanner([int]$processId, $ids) {
  $window = Get-UrMainWindow $processId
  if (-not $window) { throw "the app (pid $processId) has no window" }
  $banner = Find-UrId $window $ids.Banner
  $action = Find-UrId $window $ids.Action
  $later = Find-UrId $window $ids.Later
  $open = Test-UrShown $banner
  $texts = @()
  if ($open) {
    $texts = Get-UrTexts $banner
    if ($null -eq $texts) { $texts = @() }
    if ($banner.Current.Name -and -not ($texts -contains [string]$banner.Current.Name)) { $texts = @([string]$banner.Current.Name) + $texts }
  }
  $facts = [pscustomobject]@{ Ids = $ids; Found = [bool]$banner; Open = $open; Texts = $texts; Text = ($texts -join ' | ')
    Action = ''; ActionEnabled = $false; Later = (Test-UrShown $later); LaterName = '' }
  if (Test-UrShown $action) {
    $facts.Action = [string]$action.Current.Name
    $facts.ActionEnabled = [bool]$action.Current.IsEnabled
  }
  if ($facts.Later) { $facts.LaterName = [string]$later.Current.Name }
  return $facts
}

function Show-PrfBanner($banner, [string]$when) {
  Write-Host ("banner on {0} {1}: found={2} open={3} action='{4}' enabled={5} later={6} '{7}' texts: {8}" -f $banner.Ids.Name, $when,
      $banner.Found, $banner.Open, $banner.Action, $banner.ActionEnabled, $banner.Later, $banner.LaterName, $banner.Text)
}

function Wait-PrfBanner([int]$processId, $ids, [scriptblock]$until, [string]$what, [int]$seconds = 60) {
  $deadline = (Get-Date).AddSeconds($seconds)
  $banner = $null
  do {
    $banner = Get-PrfBanner $processId $ids
    if (& $until $banner) { return $banner }
    Start-Sleep -Milliseconds 500
  } while ((Get-Date) -lt $deadline)
  Show-PrfBanner $banner 'at the end of the wait'
  throw "the banner on $($ids.Name) did not come to show $what within $seconds s"
}

# Every element of the window whose automation id starts with
# acceptance.update., with whether it is on the screen.
function Get-PrfUpdateElements([int]$processId) {
  $window = Get-UrMainWindow $processId
  $found = New-Object System.Collections.Generic.List[string]
  foreach ($element in $window.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)) {
    try {
      $id = [string]$element.Current.AutomationId
      if ($id.StartsWith('acceptance.update.')) { $found.Add("$id (offscreen=$($element.Current.IsOffscreen), '$($element.Current.Name)')") }
    } catch { }
  }
  return ,$found.ToArray()
}

# Where a banner's row is, by UI Automation's own rectangles, in pixels from
# the window's top: the end of the screen's visible part (the status strip's
# top while it shows, the sign-in column's bottom while that shows, else the
# window's bottom), the connect pane's bottom fade while it shows, and the
# rows of the banner's action and of Later. UI Automation cuts a control's
# rectangle to what is in view, so $natural (the place read with the column
# laid out whole) gives each control's uncut height. A row is clear when
# both its controls are on the screen, uncut, above the visible part's end,
# and not under the fade.
function Get-PrfPlace([int]$processId, $ids, [string]$when, $natural = $null) {
  $window = Get-UrMainWindow $processId
  if (-not $window) { throw "the app (pid $processId) has no window" }
  $frame = $window.Current.BoundingRectangle
  $fold = [int]([double]$frame.Height)
  $foldBy = 'the window bottom'
  $strip = Find-UrId $window 'StatusStrip'
  $login = Find-UrId $window 'LoginRoot'
  if (Test-UrShown $strip) {
    $fold = [int]([double]$strip.Current.BoundingRectangle.Top - [double]$frame.Top)
    $foldBy = 'the status strip'
  } elseif (Test-UrShown $login) {
    $fold = [int]([double]$login.Current.BoundingRectangle.Bottom - [double]$frame.Top)
    $foldBy = 'the sign-in column'
  }
  $fade = $null
  $fadeElement = Find-UrId $window 'PaneAScrollFade'
  if (Test-UrShown $fadeElement) { $fade = Get-PrfRect $fadeElement $frame }
  $place = [pscustomobject]@{ When = $when; Width = [int]$frame.Width; Height = [int]$frame.Height; Fold = $fold; FoldBy = $foldBy
    Fade = $fade; Banner = (Get-PrfRect (Find-UrId $window $ids.Banner) $frame)
    Action = (Get-PrfRect (Find-UrId $window $ids.Action) $frame); Later = (Get-PrfRect (Find-UrId $window $ids.Later) $frame)
    Clear = $false; Why = '' }
  $why = New-Object System.Collections.Generic.List[string]
  foreach ($part in 'Action', 'Later') {
    $rect = $place.$part
    if ($null -eq $rect) { $why.Add("no $part"); continue }
    if ($rect.Offscreen) { $why.Add("$part offscreen") }
    if ($rect.Bottom -gt $fold) { $why.Add("$part ends at y=$($rect.Bottom), past $foldBy at y=$fold") }
    if ($null -ne $natural -and $null -ne $natural.$part -and $rect.Height -lt $natural.$part.Height - 1) {
      $why.Add("$part cut to $($rect.Height) of its $($natural.$part.Height) px")
    }
    if ($null -ne $fade -and $rect.Bottom -gt $fade.Top -and $rect.Top -lt $fade.Bottom) {
      $why.Add("$part under the pane's bottom fade (y=$($fade.Top)..$($fade.Bottom))")
    }
  }
  $place.Clear = $why.Count -eq 0
  $place.Why = $why -join '; '
  Write-Host ("place on {0} {1}: window {2}x{3}; the visible part ends at y={4} ({5}); fade {6}; banner {7}; action {8}; Later {9}; row clear: {10}{11}" -f
    $ids.Name, $when, $place.Width, $place.Height, $fold, $foldBy, (Format-PrfRect $fade), (Format-PrfRect $place.Banner),
    (Format-PrfRect $place.Action), (Format-PrfRect $place.Later), $place.Clear, $(if ($place.Why) { " ($($place.Why))" } else { '' }))
  return $place
}

# GitHub's own release list (the bytes of $listFile) with one release put in
# front: $expect's version, out since $publishedUnix, immutable, neither a
# draft nor a prerelease, its x64 MSI the file $msi at the feed's own URL,
# with the SHA-256 GitHub would list for it. The rest is GitHub's bytes.
# Writes it to $out; returns this test's own reading of it at $serverUnix.
function New-PrfStandInList([string]$listFile, $expect, [string]$msi, [long]$publishedUnix, [long]$serverUnix, [string]$out) {
  $text = [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($listFile)).Trim()
  if (-not $text.StartsWith('[')) { throw "the release list is not a JSON array" }
  $tag = 'v' + $expect.version
  if ($text.Contains('"tag_name":"' + $tag + '"')) { throw "the list already names $tag" }
  $name = "URnetwork-$($expect.version)-x64.msi"
  $digest = (Get-FileHash -Algorithm SHA256 -LiteralPath $msi).Hash.ToLowerInvariant()
  $size = (Get-Item -LiteralPath $msi).Length
  $at = Format-UrUnix $publishedUnix
  $url = "https://github.com/$UrOfficialOwner/$UrOfficialRepo/releases/download/$tag/$name"
  $release = '{"url":"https://api.github.com/repos/' + $UrOfficialOwner + '/' + $UrOfficialRepo + '/releases/1","id":1,' +
    '"tag_name":"' + $tag + '","name":"' + $tag + '","draft":false,"prerelease":false,"immutable":true,' +
    '"created_at":"' + $at + '","published_at":"' + $at + '","updated_at":"' + $at + '",' +
    '"assets":[{"name":"' + $name + '","content_type":"application/x-msi","state":"uploaded","size":' + $size + ',' +
    '"digest":"sha256:' + $digest + '","created_at":"' + $at + '","updated_at":"' + $at + '",' +
    '"browser_download_url":"' + $url + '"}]}'
  $rest = $text.Substring(1).TrimStart()
  $separator = ','
  if ($rest.StartsWith(']')) { $separator = '' }
  $joined = '[' + $release + $separator + $rest
  [IO.File]::WriteAllBytes($out, [Text.Encoding]::UTF8.GetBytes($joined))
  $releases = @($joined | ConvertFrom-Json)
  if ($releases.Count -eq 1 -and $releases[0] -is [array]) { $releases = @($releases[0]) }
  Write-Host "stand-in list: $tag added in front of GitHub's $($releases.Count - 1) releases: $name, $size bytes, sha256 $digest, out since $at"
  return Get-UrFeedReading $releases $serverUnix
}

# Presses a control through its Invoke pattern, as a click does.
function Invoke-PrfId([int]$processId, [string]$automationId) {
  $window = Get-UrMainWindow $processId
  $element = Find-UrId $window $automationId
  if (-not (Test-UrShown $element)) { throw "$automationId is not on the screen" }
  if (-not $element.Current.IsEnabled) { throw "$automationId is disabled" }
  Write-Host "invoke $automationId ('$($element.Current.Name)')"
  $element.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}

# The window's texts, in tree order, for the log.
function Show-PrfTexts([int]$processId, [string]$when) {
  $texts = Get-UrTexts (Get-UrMainWindow $processId)
  Write-Host "texts $when ($($texts.Count)): $($texts -join ' | ')"
  return ,$texts
}

# Installs vN silently, as A2b's legs did; checks the product and its helper.
function Install-PrfVn {
  if ((Get-UrProducts).Count -ne 0) { throw "a URnetwork product is already installed on this runner" }
  if (Test-Path (Join-Path $env:LOCALAPPDATA 'URnetwork')) { throw "the user's URnetwork folder already exists: not a fresh profile" }
  $code = Invoke-UrMsiexec '/i' (Get-UrMsiPath 'vn') '/qn' 'vn-install.log'
  if ($code -ne 0) { throw "vN install exit code $code" }
  $product = Assert-UrInstalled (Get-UrExpect 'vn') 'vn-install.log'
  Set-Content -Encoding ascii (Join-Path $env:RUNNER_TEMP 'vn.productcode') $product.ProductCode
  Assert-WildExeVersions (Get-UrExpect 'vn')
  if ((Get-UrHelperFeed $UrHelperPath) -ne 'official') { throw "the installed URnetworkUpdate.exe is not the official feed's" }
  Write-Host "OK: vN is installed: $($product.ProductCode) ProductVersion $($product.Version); its update helper is the official feed's"
}

# The window's screen size, its first size, and the session.
function Show-PrfInventory {
  Initialize-PrfWin
  Write-Host "runner image: $env:ImageOS $env:ImageVersion; $([Environment]::OSVersion.VersionString); screen $([PrfWin]::Screen()); mode $PrfMode"
  Get-CimInstance Win32_VideoController | ForEach-Object { Write-Host "display: $($_.Name) $($_.CurrentHorizontalResolution)x$($_.CurrentVerticalResolution)" }
  $os = Get-CimInstance Win32_OperatingSystem
  Write-Host "edition: $($os.Caption) $($os.Version); Segoe Fluent Icons installed: $(Test-Path "$env:windir\Fonts\SegoeIcons.ttf")"
}
