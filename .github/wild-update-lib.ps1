# Helpers for .github/workflows/wild-update.yml, on top of vpn-feed-ui-lib.ps1
# (and through it vpn-feed-lib.ps1, vpn-apply-lib.ps1 and vpn-upgrade-lib.ps1,
# A2b's runner-test helpers, unchanged): the published release vN, downloaded
# and installed as a user gets it, the official feed recorded as it reads in
# the run, and what an update leaves behind. Dot-sourced by every step:
#   . "$env:GITHUB_WORKSPACE\.github\wild-update-lib.ps1"
# Runs on a disposable GitHub runner only. It publishes nothing and creates no
# release or tag: urnetwork/build is read without a token, as the app reads
# it, and its MSIs are downloaded as a browser downloads them.
# Pure ASCII, Windows PowerShell 5.1.
#
# SPDX-License-Identifier: MPL-2.0

. (Join-Path $PSScriptRoot 'vpn-feed-ui-lib.ps1')

New-Item -ItemType Directory -Force $UrMsiDir | Out-Null
$WildDir = Join-Path $env:RUNNER_TEMP 'wild'
New-Item -ItemType Directory -Force $WildDir | Out-Null

# The product's own exes: every one carries the release's FILEVERSION.
$WildExes = @('URnetwork.exe', 'urnetworkd.exe', 'URnetworkUpdate.exe')

# ---- GitHub's clock and the feed ----------------------------------------------

# GitHub's date now, in Unix seconds: the Date header of /rate_limit, which no
# anonymous limit counts.
function Get-WildGitHubNow {
  $response = Invoke-WebRequest -UseBasicParsing -Uri 'https://api.github.com/rate_limit' `
    -UserAgent 'URnetwork-Windows/runner-test' -Headers @{ Accept = 'application/vnd.github+json' }
  $date = [string]$response.Headers['Date']
  if (-not $date) { throw "GitHub's /rate_limit answered with no Date header" }
  return ConvertTo-UrUnix $date
}

# When GitHub's UTC day ends within $margin seconds, waits until it has ended:
# which releases count changes only when that day does (ReleaseSelection.h
# HasSoaked), so the feed's reading here, the tray app's check and the update
# helper's own list must all fall on one day.
function Wait-WildClearOfMidnight([int]$margin = 3600) {
  $now = Get-WildGitHubNow
  $left = $UrFeedDaySeconds - ($now % $UrFeedDaySeconds)
  Write-Host "GitHub's date is $(Format-UrUnix $now); its UTC day ends in $left s"
  if ($left -ge $margin) { return }
  Write-Host "waiting $($left + 120) s for the next UTC day, so the whole update falls on one day"
  Start-Sleep -Seconds ($left + 120)
  Write-Host "GitHub's date is $(Format-UrUnix (Get-WildGitHubNow))"
}

# Throws when GitHub's UTC day is no longer the one the feed was read on.
function Assert-WildSameDay($reading) {
  $now = Get-WildGitHubNow
  $day = $now - ($now % $UrFeedDaySeconds)
  if ($day -ne [long]$reading.DayStart) {
    throw "GitHub's date is $(Format-UrUnix $now): its UTC day is not the one the feed was read on ($(Format-UrUnix ([long]$reading.DayStart))). Run again."
  }
  Write-Host "OK: GitHub's date is $(Format-UrUnix $now), the UTC day the feed was read on"
}

# A field of an object from GitHub's JSON, or $null when the object has none
# (Set-StrictMode would throw on reading a missing one).
function Get-WildField($object, [string]$name) {
  if ($null -eq $object) { return $null }
  $property = $object.PSObject.Properties[$name]
  if ($null -eq $property) { return $null }
  return $property.Value
}

# The feed as it reads in this run, written down whole as feed-state.txt: the
# list's Date header and, for every release in it, the values vN's rules read
# (ReleaseSelection.h SelectRelease, SoakStartOf) and this test's verdict.
function Save-WildFeedRecord($list, $reading, [string]$name = 'feed-state.txt') {
  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add("release list: $UrFeedListUrl, read without a token")
  $lines.Add("Date: $($list.Date) = $(Format-UrUnix $list.ServerUnix); the UTC day began $(Format-UrUnix ([long]$reading.DayStart))")
  $lines.Add("$($list.Releases.Count) releases, $($list.Bytes) bytes")
  foreach ($release in $list.Releases) {
    $tag = [string]$release.tag_name
    $msi = 'no x64 MSI'
    if ($tag.Length -gt 1) {
      # its own variable: $name is the record's file name
      $asset = "URnetwork-$($tag.Substring(1))-x64.msi"
      $assets = @($release.assets | Where-Object { $_.name -eq $asset })
      if ($assets.Count -eq 1) { $msi = "$asset updated_at=$($assets[0].updated_at) digest=$($assets[0].digest)" }
    }
    $verdict = '?'
    $rows = @($reading.Rows | Where-Object { $_.Tag -eq $tag })
    if ($rows.Count -ge 1) {
      $row = $rows[0]
      if ($row.Soaked) { $verdict = "COUNTS (since $(Format-UrUnix ([long]$row.Since)), from $(Format-UrUnix ([long]$row.CountsFrom)))" }
      elseif ([long]$row.CountsFrom -ne 0) { $verdict = "WAITS (since $(Format-UrUnix ([long]$row.Since)), counts from $(Format-UrUnix ([long]$row.CountsFrom)))" }
      else { $verdict = "skipped: $($row.Why)" }
    }
    $lines.Add(("{0} draft={1} prerelease={2} immutable={3} published_at={4} updated_at={5} | {6} | {7}" -f $tag,
        (Get-WildField $release 'draft'), (Get-WildField $release 'prerelease'), (Get-WildField $release 'immutable'),
        (Get-WildField $release 'published_at'), (Get-WildField $release 'updated_at'), $msi, $verdict))
  }
  $offeredText = 'nothing'
  if ($reading.Offered) { $offeredText = "$($reading.Offered.Tag) (code $($reading.Offered.Code))" }
  $waitingText = 'nothing'
  if ($reading.Waiting) { $waitingText = "$($reading.Waiting.Tag) until $(Format-UrUnix ([long]$reading.Waiting.CountsFrom))" }
  $lines.Add("OFFERED: $offeredText")
  $lines.Add("HELD BACK: $waitingText")
  $path = Join-Path $env:RUNNER_TEMP $name
  [IO.File]::WriteAllLines($path, $lines.ToArray(), (New-Object Text.UTF8Encoding($false)))
  foreach ($line in $lines) { Write-Host "record: $line" }
  Write-Host "the record is $path"
}

# One release of the list by its tag, as a row like Get-UrFeedReading's: its
# x64 MSI, the feed's download URL for it and GitHub's SHA-256 of it, from the
# same asset object. $null when the list does not hold the tag.
function Get-WildReleaseRow($releases, [string]$tag, [string]$arch = 'x64') {
  $found = @($releases | Where-Object { $_.tag_name -eq $tag })
  if ($found.Count -eq 0) { return $null }
  if ($found.Count -ne 1) { throw "the list names $tag $($found.Count) times" }
  $release = $found[0]
  $match = [regex]::Match($tag, '^v(\d{4})\.(\d{1,2})\.(\d{1,2})-(\d{1,18})$')
  if (-not $match.Success) { throw "$tag is not a tag of the feed's grammar" }
  $version = $tag.Substring(1)
  $name = "URnetwork-$version-$arch.msi"
  $assets = @($release.assets | Where-Object { $_.name -eq $name })
  if ($assets.Count -ne 1) { throw "$tag has $($assets.Count) assets named $name" }
  $url = "https://github.com/$UrOfficialOwner/$UrOfficialRepo/releases/download/$tag/$name"
  if ([string]$assets[0].browser_download_url -ne $url) { throw "$name is at $($assets[0].browser_download_url), not $url" }
  if ([string]$assets[0].digest -notmatch '^sha256:[0-9a-fA-F]{64}$') { throw "$name has no sha256 digest: '$($assets[0].digest)'" }
  return [pscustomobject]@{ Tag = $tag; Version = $version; Code = [uint64]$match.Groups[4].Value; Asset = $name; Url = $url
    Digest = ([string]$assets[0].digest).Substring(7).ToLowerInvariant(); Size = [long]$assets[0].size
    AssetUpdatedAt = [string](Get-WildField $assets[0] 'updated_at'); PublishedAt = [string](Get-WildField $release 'published_at')
    UpdatedAt = [string](Get-WildField $release 'updated_at'); Draft = (Get-WildField $release 'draft')
    Prerelease = (Get-WildField $release 'prerelease'); Immutable = (Get-WildField $release 'immutable') }
}

# One release of urnetwork/build by its tag, from the API, read with the job's
# token (this test's own read, so the anonymous requests stay the app's), as
# a list of one.
function Get-WildReleaseByTag([string]$tag) {
  $headers = @{ Accept = 'application/vnd.github+json' }
  if ($env:WILD_READ_TOKEN) { $headers['Authorization'] = "Bearer $env:WILD_READ_TOKEN" }
  $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$UrOfficialOwner/$UrOfficialRepo/releases/tags/$tag" `
    -UserAgent 'URnetwork-Windows/runner-test' -Headers $headers
  return ,@($release)
}

# The urnetwork/windows commit urnetwork/build's `windows` submodule names at
# $tag, read with the job's token; '' when it cannot be read.
function Get-WildBuildSource([string]$tag) {
  try {
    $headers = @{ Accept = 'application/vnd.github+json' }
    if ($env:WILD_READ_TOKEN) { $headers['Authorization'] = "Bearer $env:WILD_READ_TOKEN" }
    $entry = Invoke-RestMethod -Uri "https://api.github.com/repos/$UrOfficialOwner/$UrOfficialRepo/contents/windows?ref=$tag" `
      -UserAgent 'URnetwork-Windows/runner-test' -Headers $headers
    return [string]$entry.sha
  } catch {
    Write-Host "urnetwork/build's windows submodule at $tag could not be read: $($_.Exception.Message)"
    return ''
  }
}

# ---- vN's own rules, read from its source -----------------------------------

# The lines of vN's own source (this checkout: urnetwork/windows at vN's
# commit) that decide which release it offers, each found exactly once and
# printed with its line number; and the constants of this test's own reading
# (vpn-feed-lib.ps1) checked against them.
function Show-WildRules {
  $root = $env:GITHUB_WORKSPACE
  $selection = 'app\src\Common\ReleaseSelection.h'
  $apply = 'app\src\Common\UpdateApply.h'
  $checker = 'app\src\App\UpdateChecker.cpp'
  $helper = 'app\src\Updater\ApplyUpdate.cpp'
  $rules = @(
    @($selection, '^\s*\.numericRepoId = 936244679,$', 'the official feed is the release list of repository 936244679 (urnetwork/build)'),
    @($selection, '^\s*\.owner = "urnetwork",$', 'its downloads are urnetwork/...'),
    @($selection, '^\s*\.repo = "build",$', '.../build/releases/download/<tag>/<asset>'),
    @($selection, '^\s*\.acceptBetaPrereleases = false,$', 'no prerelease counts on the official feed'),
    @($selection, '^\s*\.requireImmutable = true,$', 'only an immutable release counts'),
    @($selection, '^\s*\.soakSeconds = 24 \* 60 \* 60,$', 'a release counts once it has been out, unchanged, for 24 h'),
    @($selection, '^inline constexpr int kReleaseListPageSize = 30;$', 'one page of 30 releases is read'),
    @($selection, '^inline constexpr std::int64_t kFutureCodeLimitSeconds = 48 \* 60 \* 60;$', 'a code more than 48 h after the list Date is skipped'),
    @($selection, '^\s*if \(\*published < CodeUnixSeconds\(code\) - kFutureCodeLimitSeconds\) \{$', 'a publication more than 48 h before its code gives no soak'),
    @($selection, '^\s*return \{std::max\(\{\*published, \*changed, \*uploaded\}\), \{\}\};$', 'the soak counts from the LATEST of published_at, updated_at and the MSI asset updated_at'),
    @($selection, '^\s*return \*sinceUnixSeconds \+ feed\.soakSeconds <= UtcDayStart\(serverUnixSeconds\);$', 'judged at the START of the UTC day of the list Date header: one offer per UTC day'),
    @($selection, '^\s*if \(rel\.draft\) continue;$', 'drafts are skipped'),
    @($selection, '^\s*if \(rel\.prerelease && !feed\.acceptAnyPrerelease &&$', 'prereleases are skipped'),
    @($selection, '^\s*if \(feed\.requireImmutable && !rel\.immutable\) \{$', 'releases that are not immutable are skipped'),
    @($selection, '^\s*const std::string name = InstallerAssetName\(tag->version, arch\);$', 'the release must carry URnetwork-<version>-x64.msi'),
    @($selection, '^\s*if \(tag->code > s\.newestCode\) \{$', 'the newest code that counts is the one named'),
    @($selection, '^\s*std::string digestHex = DigestHexFromAssetDigest\(match->digest\);$', 'and offered with GitHub''s sha256 digest of that asset'),
    @($selection, '^inline constexpr std::int64_t kOfferFreshSeconds = 5 \* 60;$', 'an offer older than 5 min is checked again before a click is acted on'),
    @($checker, '^constexpr auto kLaunchDelay = std::chrono::seconds\(30\);$', 'the tray app checks 30 s after it starts'),
    @($checker, '^\s*const update::Feed& feed = update::kOfficialFeed;$', 'the tray app checks the official feed'),
    @($checker, '^\s*const update::Selection sel = update::SelectRelease\(\*parsed, kArch, feed, serverUnixSeconds\);$', 'judged against the list''s own Date header'),
    @($checker, '^\s*\} else if \(offer\.code > version::kCode\) \{$', 'offered only when newer than this build'),
    @($helper, '^\s*if \(!update::SelectionOffers\(selection, tag, version::kCode\)\) \{$', 'the elevated helper installs only the tag its OWN fetch of the list offers'),
    @($apply, '^\s*command\.append\(L"\\" /passive /norestart /l\*v \\""\);$', 'msiexec runs /passive /norestart with a verbose log')
  )
  foreach ($rule in $rules) {
    $path = Join-Path $root $rule[0]
    $hits = @(Select-String -LiteralPath $path -Pattern $rule[1] -CaseSensitive)
    if ($hits.Count -ne 1) { throw "vN's source $($rule[0]) has $($hits.Count) lines matching $($rule[1]), want 1" }
    Write-Host ("rule {0}:{1}: {2}" -f ($rule[0] -replace '\\', '/'), $hits[0].LineNumber, $hits[0].Line.Trim())
    Write-Host "     -> $($rule[2])"
  }
  if ($UrOfficialRepoId -ne '936244679' -or $UrFeedSoakSeconds -ne 86400 -or $UrFeedDaySeconds -ne 86400 -or
      $UrFeedListPath -ne '/repositories/936244679/releases?per_page=30' -or $UrOfficialOwner -ne 'urnetwork' -or $UrOfficialRepo -ne 'build') {
    throw "this test's reading (vpn-feed-lib.ps1) does not use vN's constants"
  }
  Write-Host "OK: this test's reading of the feed (vpn-feed-lib.ps1 Get-UrFeedReading) uses vN's feed, page, soak and day"
}

# ---- install, as a user does --------------------------------------------------

# The top-level windows of one process, as UI Automation sees them.
function Get-WildProcessWindows([int]$processId) {
  Initialize-UrUi
  $condition = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $processId)
  return ,@([System.Windows.Automation.AutomationElement]::RootElement.FindAll(
      [System.Windows.Automation.TreeScope]::Children, $condition))
}

# msiexec as a double-click starts it: /i and the package, so the UI is full
# (for this package, which authors no dialog, Windows Installer's own
# progress) and every property is the package's own default. The verbose log
# is the one thing added, and it changes nothing the install does. A dialog
# that asks anything is printed and answered with its OK button through UI
# Automation's Invoke pattern, as a user who takes the default does; one with
# no OK button is left alone and the install times out. Returns the exit code
# and the dialogs.
function Invoke-WildMsiexecAsUser([string]$target, [string]$logName, [int]$timeoutSeconds = 900) {
  Initialize-UrUi
  $log = Join-Path $UrLogDir $logName
  $arguments = "/i `"$target`" /l*v `"$log`""
  Write-Host "msiexec $arguments (full UI, the package's own defaults)"
  $client = Start-Process -FilePath "$env:SystemRoot\System32\msiexec.exe" -ArgumentList $arguments -PassThru
  $null = $client.Handle
  $dialogs = New-Object System.Collections.Generic.List[string]
  $answered = @{}
  $seen = @{}
  $buttonType = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Button)
  $deadline = (Get-Date).AddSeconds($timeoutSeconds)
  while (-not $client.WaitForExit(500)) {
    $windows = @()
    try { $windows = Get-WildProcessWindows $client.Id } catch { }
    foreach ($window in $windows) {
      try {
        $key = [string]$window.Current.NativeWindowHandle
        # assigned, not wrapped in @(): Get-UrTexts returns its list as one array
        $names = Get-UrTexts $window
        $buttons = @($window.FindAll([System.Windows.Automation.TreeScope]::Descendants, $buttonType))
        $labels = @($buttons | ForEach-Object { [string]$_.Current.Name })
        $line = "'$($window.Current.Name)': $($names -join ' | ') [buttons: $($labels -join ', ')]"
        if (-not $seen.ContainsKey($line)) { $seen[$line] = $true; Write-Host "msiexec window $line" }
        if ($answered.ContainsKey($key)) { continue }
        $ok = @($buttons | Where-Object { ([string]$_.Current.Name -replace '&', '') -eq 'OK' })
        if ($ok.Count -eq 0) { continue }
        $dialogs.Add($line)
        Write-Host "DIALOG $line"
        $ok[0].GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
        Write-Host "answered it with OK (Invoke pattern)"
        $answered[$key] = $true
      } catch [System.Windows.Automation.ElementNotAvailableException] {
        # a window that closed while it was read
      }
    }
    if ((Get-Date) -gt $deadline) {
      Stop-Process -Id $client.Id -Force -ErrorAction SilentlyContinue
      throw "msiexec (pid $($client.Id)) still runs after $timeoutSeconds s"
    }
  }
  $client.WaitForExit()
  Write-Host "msiexec exit code: $($client.ExitCode); dialogs answered: $($dialogs.Count)"
  return [pscustomobject]@{ ExitCode = [int]$client.ExitCode; Dialogs = $dialogs.ToArray() }
}

# ---- what is installed ----------------------------------------------------------

$WildTokenSource = @'
using System;
using System.Runtime.InteropServices;

public static class WildToken {
  [DllImport("kernel32.dll", SetLastError = true)]
  private static extern IntPtr OpenProcess(uint access, bool inherit, int processId);
  [DllImport("advapi32.dll", SetLastError = true)]
  private static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
  [DllImport("advapi32.dll", SetLastError = true)]
  private static extern bool GetTokenInformation(IntPtr token, int infoClass, IntPtr info, int length, out int returned);
  [DllImport("advapi32.dll")]
  private static extern IntPtr GetSidSubAuthority(IntPtr sid, uint index);
  [DllImport("advapi32.dll")]
  private static extern IntPtr GetSidSubAuthorityCount(IntPtr sid);
  [DllImport("kernel32.dll")]
  private static extern bool CloseHandle(IntPtr handle);
  private const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
  private const uint TOKEN_QUERY = 0x0008;
  private const int TokenElevationType = 18;
  private const int TokenIntegrityLevel = 25;

  // "<integrity> integrity, elevation type <Default|Full|Limited>", or what failed.
  public static string Describe(int processId) {
    IntPtr process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, processId);
    if (process == IntPtr.Zero) return "OpenProcess error " + Marshal.GetLastWin32Error();
    IntPtr token;
    bool opened = OpenProcessToken(process, TOKEN_QUERY, out token);
    int openError = Marshal.GetLastWin32Error();
    CloseHandle(process);
    if (!opened) return "OpenProcessToken error " + openError;
    try {
      int size;
      GetTokenInformation(token, TokenIntegrityLevel, IntPtr.Zero, 0, out size);
      if (size <= 0) return "no integrity level";
      IntPtr label = Marshal.AllocHGlobal(size);
      string level;
      try {
        if (!GetTokenInformation(token, TokenIntegrityLevel, label, size, out size)) {
          return "GetTokenInformation error " + Marshal.GetLastWin32Error();
        }
        IntPtr sid = Marshal.ReadIntPtr(label);
        int count = Marshal.ReadByte(GetSidSubAuthorityCount(sid));
        int rid = Marshal.ReadInt32(GetSidSubAuthority(sid, (uint)(count - 1)));
        if (rid == 0x1000) level = "Low";
        else if (rid == 0x2000) level = "Medium";
        else if (rid == 0x2100) level = "Medium Plus";
        else if (rid == 0x3000) level = "High";
        else if (rid == 0x4000) level = "System";
        else level = "0x" + rid.ToString("X");
      } finally {
        Marshal.FreeHGlobal(label);
      }
      IntPtr typeBuffer = Marshal.AllocHGlobal(4);
      string type = "?";
      try {
        int returned;
        if (GetTokenInformation(token, TokenElevationType, typeBuffer, 4, out returned)) {
          int value = Marshal.ReadInt32(typeBuffer);
          type = value == 1 ? "Default" : value == 2 ? "Full" : value == 3 ? "Limited" : value.ToString();
        }
      } finally {
        Marshal.FreeHGlobal(typeBuffer);
      }
      return level + " integrity, elevation type " + type;
    } finally {
      CloseHandle(token);
    }
  }
}
'@

# A process's integrity level and elevation type, as a sentence.
function Get-WildTokenText([int]$processId) {
  if (-not ('WildToken' -as [type])) { Add-Type -TypeDefinition $WildTokenSource }
  try { return [WildToken]::Describe($processId) } catch { return "not read: $($_.Exception.Message)" }
}

# What is installed now, printed and saved as installed-<label>.json: the
# products under URnetwork's UpgradeCode, every exe in the install folder
# (FileVersion, ProductVersion, FileDescription, signature, SHA-256),
# urnetworkd as the service manager has it and the image it runs, and the
# update helper.
function Show-WildInstalled([string]$label) {
  $products = Get-UrProducts
  foreach ($product in $products) { Write-Host "$label product: $($product.ProductCode) ProductVersion $($product.Version)" }
  $exes = New-Object System.Collections.Generic.List[object]
  foreach ($file in @(Get-ChildItem -LiteralPath $UrInstallDir -Recurse -File -Filter '*.exe' -ErrorAction SilentlyContinue)) {
    if ($file.FullName.StartsWith($UrUpdatesDir + '\', [StringComparison]::OrdinalIgnoreCase)) { continue }
    $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($file.FullName)
    $exe = [pscustomobject]@{ Path = $file.FullName.Substring($UrInstallDir.Length + 1); FileVersion = (Get-UrFileVersion $file.FullName)
      ProductVersion = [string]$info.ProductVersion; Description = [string]$info.FileDescription; Size = [long]$file.Length
      Sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash.ToLowerInvariant()
      Signature = [string](Get-AuthenticodeSignature -LiteralPath $file.FullName).Status }
    $exes.Add($exe)
    Write-Host ("{0} exe: {1} FileVersion {2} (ProductVersion '{3}', '{4}', {5} bytes, signature {6}, SHA-256 {7})" -f $label, $exe.Path,
      $exe.FileVersion, $exe.ProductVersion, $exe.Description, $exe.Size, $exe.Signature, $exe.Sha256)
  }
  $service = $null
  $cim = Get-CimInstance Win32_Service -Filter "Name='urnetworkd'"
  if ($cim) {
    $image = ''
    $imageVersion = ''
    if ([int]$cim.ProcessId -ne 0) {
      $process = Get-Process -Id ([int]$cim.ProcessId) -ErrorAction SilentlyContinue
      if ($process -and $process.Path) { $image = $process.Path; $imageVersion = Get-UrFileVersion $process.Path }
    }
    $service = [pscustomobject]@{ Name = [string]$cim.Name; State = [string]$cim.State; StartMode = [string]$cim.StartMode
      StartName = [string]$cim.StartName; PathName = [string]$cim.PathName; ProcessId = [int]$cim.ProcessId
      RunningImage = $image; RunningImageVersion = $imageVersion }
    Write-Host ("{0} service: urnetworkd {1}, start {2}, as {3}, pid {4}, binPath {5}, running image {6} ({7})" -f $label, $service.State,
      $service.StartMode, $service.StartName, $service.ProcessId, $service.PathName, $service.RunningImage, $service.RunningImageVersion)
  } else {
    Write-Host "$label service: no urnetworkd service"
  }
  $helper = $null
  if (Test-Path -LiteralPath $UrHelperPath) {
    $acl = Get-Acl -LiteralPath $UrHelperPath
    $helper = [pscustomobject]@{ Path = $UrHelperPath; FileVersion = (Get-UrFileVersion $UrHelperPath)
      Description = [string][Diagnostics.FileVersionInfo]::GetVersionInfo($UrHelperPath).FileDescription
      Feed = (Get-UrHelperFeed $UrHelperPath); Owner = [string]$acl.Owner
      Sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $UrHelperPath).Hash.ToLowerInvariant()
      Signature = [string](Get-AuthenticodeSignature -LiteralPath $UrHelperPath).Status }
    Write-Host ("{0} update helper: {1} FileVersion {2}, '{3}' ({4} feed), owner {5}, signature {6}, SHA-256 {7}" -f $label, $helper.Path,
      $helper.FileVersion, $helper.Description, $helper.Feed, $helper.Owner, $helper.Signature, $helper.Sha256)
  } else {
    Write-Host "$label update helper: none at $UrHelperPath"
  }
  $state = [pscustomobject]@{ Label = $label; Products = $products; Exes = $exes.ToArray(); Service = $service; Helper = $helper }
  $state | ConvertTo-Json -Depth 5 | Set-Content -Encoding ascii (Join-Path $WildDir "installed-$label.json")
  return $state
}

# Every one of the product's exes carries $expect's FILEVERSION; any other exe
# in the install folder is printed with its own.
function Assert-WildExeVersions($expect) {
  foreach ($exe in $WildExes) {
    $path = Join-Path $UrInstallDir $exe
    if (-not (Test-Path -LiteralPath $path)) { throw "$exe is not in $UrInstallDir" }
    $version = Get-UrFileVersion $path
    if ($version -ne $expect.file_version) { throw "$exe is $version, want $($expect.file_version) ($($expect.key))" }
    Write-Host "OK: $exe is $version ($($expect.key))"
  }
  foreach ($file in @(Get-ChildItem -LiteralPath $UrInstallDir -Recurse -File -Filter '*.exe' -ErrorAction SilentlyContinue)) {
    if ($WildExes -contains $file.Name -and $file.DirectoryName -eq $UrInstallDir) { continue }
    if ($file.FullName.StartsWith($UrUpdatesDir + '\', [StringComparison]::OrdinalIgnoreCase)) { continue }
    Write-Host "OBSERVED: $($file.FullName) is $(Get-UrFileVersion $file.FullName) (not one of the product's own exes)"
  }
}

# Waits up to $seconds for no URnetworkUpdate.exe to run; returns what is
# still running then.
function Wait-WildNoHelper([int]$seconds = 90) {
  $deadline = (Get-Date).AddSeconds($seconds)
  $left = @()
  do {
    $left = @(Get-CimInstance Win32_Process -Filter "Name='URnetworkUpdate.exe'" |
        ForEach-Object { "pid $($_.ProcessId): $($_.CommandLine)" })
    if ($left.Count -eq 0) { return ,@() }
    Start-Sleep -Seconds 1
  } while ((Get-Date) -lt $deadline)
  return ,$left
}

# ---- what the app shows ----------------------------------------------------------

# The banner as it must look while it offers $release (A2b's Assert-UrOffer).
function Assert-WildOffer($banner, $release, [string]$when) {
  Show-UrBanner $banner $when
  if (-not $banner.Open) { throw "$when`: the banner is not open" }
  if ($banner.Text -notmatch ('Update available: v' + [regex]::Escape($release.Version))) { throw "$when`: the banner does not offer v$($release.Version): $($banner.Text)" }
  if ($banner.Action -ne 'Update' -or -not $banner.ActionEnabled) { throw "$when`: the banner's action is '$($banner.Action)' (enabled $($banner.ActionEnabled)), want Update" }
  if (-not $banner.Later -or $banner.LaterName -ne 'Later') { throw "$when`: the banner does not offer Later ('$($banner.LaterName)')" }
}

# The tray app's own check against this test's reading of the feed: its own
# code is vN's, the release it names newest is the one vN's rules give for
# the day, and the one it holds back is the reading's. A release the pipeline
# published between the reading and the check changes only what is held back:
# the list is then read again and the check judged by that reading.
function Assert-WildCheck($check, $reading, $vn) {
  if ($check.OwnCode -ne [string]$vn.code) { throw "the app says its code is $($check.OwnCode), want vN's $($vn.code)" }
  $offered = $reading.Offered
  if ($check.NewestCode -ne [string]$offered.Code -or $check.Newest -ne [string]$offered.Version) {
    throw "the app's own check found $($check.Newest) (code $($check.NewestCode)) newest, and vN's rules applied to the feed as read give $($offered.Tag)"
  }
  $judged = $reading
  for ($pass = 1; $pass -le 2; $pass++) {
    $wantWaiting = ''
    $wantFrom = ''
    if ($judged.Waiting) { $wantWaiting = [string]$judged.Waiting.Version; $wantFrom = Format-UrUnix ([long]$judged.Waiting.CountsFrom) }
    if ($check.Waiting -eq $wantWaiting -and $check.WaitingFrom -eq $wantFrom) {
      if ($pass -eq 2) { Write-Host "OBSERVED: the feed changed between the reading and the app's check; read again, it holds back what the app does" }
      if ($wantWaiting) {
        Write-Host "OK: the tray app's own check, from GitHub, names $($check.Newest) (code $($check.NewestCode)) newest and holds $wantWaiting back until $wantFrom, as vN's rules give for this day"
      } else {
        Write-Host "OK: the tray app's own check, from GitHub, names $($check.Newest) (code $($check.NewestCode)) newest and holds nothing back, as vN's rules give for this day"
      }
      return
    }
    if ($pass -eq 2) { break }
    Write-Host "the app holds '$($check.Waiting)' back until '$($check.WaitingFrom)', the reading '$wantWaiting' until '$wantFrom': reading the list again"
    [void](Wait-UrAnonymousRequests 2 45)
    $again = Get-UrFeedList (Join-Path $WildDir 'releases-again.json')
    $judged = Get-UrFeedReading $again.Releases $again.ServerUnix
    if (-not $judged.Offered -or $judged.Offered.Tag -ne $offered.Tag) { throw "read again, the feed offers $(if ($judged.Offered) { $judged.Offered.Tag } else { 'nothing' }), not $($offered.Tag)" }
  }
  throw "the app holds '$($check.Waiting)' back until '$($check.WaitingFrom)', and the feed, read twice, does not"
}

# Waits up to $seconds for process $processId to show its XAML window, then
# captures it as <name>.png (and the screen) and prints the texts UI
# Automation reads in it. Returns those texts, or $null when no window came.
function Save-WildWindowOf([int]$processId, [string]$name, [int]$seconds = 60) {
  Initialize-UrUi
  $deadline = (Get-Date).AddSeconds($seconds)
  $window = $null
  while (-not $window -and (Get-Date) -lt $deadline) {
    if (-not (Get-Process -Id $processId -ErrorAction SilentlyContinue)) { break }
    $window = Get-UrMainWindow $processId
    if (-not $window) { Start-Sleep -Milliseconds 500 }
  }
  if (-not $window) {
    Write-Host "OBSERVED: pid $processId showed no window within $seconds s"
    try {
      $screen = Join-Path $UrShotDir "$name.screen.png"
      Write-Host "screenshot $name.screen.png: $([UrUiApi]::SaveScreen($screen))"
    } catch { Write-Host "screenshot $name.screen.png: not taken ($($_.Exception.Message))" }
    return $null
  }
  Start-Sleep -Seconds 4
  Save-UrShot $processId $name
  $texts = Get-UrTexts (Get-UrMainWindow $processId)
  Write-Host "window of pid $processId ('$($window.Current.Name)') says: $($texts -join ' | ')"
  return ,$texts
}
