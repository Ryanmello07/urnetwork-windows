# Helpers for .github/workflows/vpn-upgrade.yml (the A2a MSI upgrade test).
# Dot-sourced by every test step:  . "$env:GITHUB_WORKSPACE\.github\vpn-upgrade-lib.ps1"
# Runs on a disposable GitHub runner only: it installs, upgrades and removes
# URnetwork MSIs machine-wide. Pure ASCII, Windows PowerShell 5.1 and pwsh 7.
#
# SPDX-License-Identifier: MPL-2.0

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$UrUpgradeCode = '{A7C1E2D3-4B5F-6081-9C2D-3E4F50617283}'
$UrInstallDir = 'C:\Program Files\URnetwork'
$UrMsiDir = Join-Path $env:RUNNER_TEMP 'msis'
$UrLogDir = Join-Path $env:RUNNER_TEMP 'upgrade-logs'
New-Item -ItemType Directory -Force $UrLogDir | Out-Null

# A real package installs about 316 files. A File-table walk that resolves
# fewer than this proves nothing about the files it missed.
$UrMinimumFileRows = 100

# Package.wxs's launch condition against installing over a newer
# urnetworkd.exe. It must stay identical to the Launch element's Message.
$UrGuardMessage = 'A newer build of URnetwork is already installed. Install that build or a later one.'

function Write-Step([string]$text) { Write-Host "==== $text" }

# The plan values for one MSI: key, version, code, msi_version, file_version
# (what the payload's exes carry).
function Get-UrExpect([string]$key) {
  $path = Join-Path $UrMsiDir "$key.json"
  if (-not (Test-Path $path)) { throw "no plan values for $key at $path" }
  return Get-Content -Raw $path | ConvertFrom-Json
}

function Get-UrMsiPath([string]$key) {
  $path = [string](Join-Path $UrMsiDir "$key.msi")
  if (-not (Test-Path $path)) { throw "no MSI for $key at $path" }
  return $path
}

function Invoke-UrComGet($object, [string]$name, [object[]]$arguments) {
  return $object.GetType().InvokeMember($name, 'GetProperty', $null, $object, $arguments)
}

# Release a COM object now, not at some later garbage collection. A Windows
# Installer database stays open, and its MSI locked, until every object that
# came from it is released; run 37209295701's v5 build failed on that lock.
function Close-UrCom($object) {
  if ($null -ne $object -and [Runtime.InteropServices.Marshal]::IsComObject($object)) {
    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($object)
  }
}

# The rows of a read-only query on an MSI file (msiOpenDatabaseModeReadOnly =
# 0), each an array of $columns strings. Every record, the view, the database
# and the installer object are released and the garbage collector runs before
# this returns, so the file is free again.
function Invoke-UrMsiQuery([string]$msi, [string]$sql, [int]$columns) {
  $installer = New-Object -ComObject WindowsInstaller.Installer
  $db = $null
  $view = $null
  $rows = New-Object System.Collections.Generic.List[object]
  try {
    $db = $installer.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null, $installer, @([string]$msi, 0))
    $view = $db.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $db, @($sql))
    $view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, $null) | Out-Null
    while ($true) {
      $record = $view.GetType().InvokeMember('Fetch', 'InvokeMethod', $null, $view, $null)
      if ($null -eq $record) { break }
      try {
        $values = New-Object string[] $columns
        for ($i = 1; $i -le $columns; $i++) {
          $values[$i - 1] = [string](Invoke-UrComGet $record 'StringData' @($i))
        }
        $rows.Add($values)
      } finally {
        Close-UrCom $record
      }
    }
    $view.GetType().InvokeMember('Close', 'InvokeMethod', $null, $view, $null) | Out-Null
  } finally {
    Close-UrCom $view
    Close-UrCom $db
    Close-UrCom $installer
    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
  }
  return ,$rows.ToArray()
}

# A built MSI's ProductVersion.
function Get-UrMsiProductVersion([string]$msi) {
  $rows = Invoke-UrMsiQuery $msi "SELECT Value FROM Property WHERE Property='ProductVersion'" 1
  if ($rows.Count -ne 1) { throw "$msi has $($rows.Count) ProductVersion rows" }
  return $rows[0][0]
}

# The version a built MSI's File table records for one file, by long name.
# Invoke-UrMsiQuery returns its rows as one array, so iterate it with foreach:
# a pipeline would receive the whole array as a single item.
function Get-UrMsiFileVersion([string]$msi, [string]$fileName) {
  $versions = @()
  foreach ($row in (Invoke-UrMsiQuery $msi 'SELECT FileName, Version FROM File' 2)) {
    if ((Get-UrLongName $row[0]) -eq $fileName) { $versions += $row[1] }
  }
  if ($versions.Count -ne 1) { throw "$msi has $($versions.Count) File rows named $fileName" }
  return $versions[0]
}

# The long name in an MSI name column: "short|long", or just "name".
function Get-UrLongName([string]$name) {
  if ($name.Contains('|')) { return ($name -split '\|')[1] }
  return $name
}

# Where each File row of an MSI lands, resolved through the Component and
# Directory tables below INSTALLFOLDER, which is $UrInstallDir. Files of a
# conditional component, or outside INSTALLFOLDER, are counted in Skipped.
function Get-UrMsiFileTargets([string]$msi) {
  $directories = @{}
  foreach ($row in (Invoke-UrMsiQuery $msi 'SELECT Directory, Directory_Parent, DefaultDir FROM Directory' 3)) {
    # DefaultDir is target[:source]; the target may be short|long, or "." for
    # the parent directory itself.
    $directories[$row[0]] = @{ Parent = $row[1]; Name = (Get-UrLongName (($row[2] -split ':')[0])) }
  }
  $components = @{}
  foreach ($row in (Invoke-UrMsiQuery $msi 'SELECT Component, Directory_, Condition FROM Component' 3)) {
    $components[$row[0]] = @{ Directory = $row[1]; Condition = $row[2] }
  }
  $resolved = @{}
  $targets = New-Object System.Collections.Generic.List[string]
  $skipped = 0
  foreach ($row in (Invoke-UrMsiQuery $msi 'SELECT File, Component_, FileName FROM File' 3)) {
    $component = $components[$row[1]]
    if (-not $component -or $component.Condition) { $skipped++; continue }
    $directory = $component.Directory
    if (-not $resolved.ContainsKey($directory)) {
      $segments = New-Object System.Collections.Generic.List[string]
      $current = $directory
      $path = $null
      for ($depth = 0; $depth -lt 64 -and $current; $depth++) {
        if ($current -eq 'INSTALLFOLDER') {
          $path = $UrInstallDir
          for ($i = $segments.Count - 1; $i -ge 0; $i--) { $path = [IO.Path]::Combine($path, $segments[$i]) }
          break
        }
        if (-not $directories.ContainsKey($current)) { break }
        $entry = $directories[$current]
        if ($entry.Name -ne '.') { $segments.Add($entry.Name) }
        if ($entry.Parent -eq $current) { break }
        $current = $entry.Parent
      }
      $resolved[$directory] = $path
    }
    if (-not $resolved[$directory]) { $skipped++; continue }
    $targets.Add([IO.Path]::Combine($resolved[$directory], (Get-UrLongName $row[2])))
  }
  return [pscustomobject]@{ Targets = $targets.ToArray(); Skipped = $skipped }
}

# The File rows of $msi that are not on disk; throws if too few rows resolve.
function Get-UrMissingFiles([string]$msi) {
  $files = Get-UrMsiFileTargets $msi
  if ($files.Targets.Count -lt $UrMinimumFileRows) {
    throw "only $($files.Targets.Count) File rows of $msi resolved under $UrInstallDir (want at least $UrMinimumFileRows): the table walk is broken"
  }
  $missing = @($files.Targets | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) })
  return [pscustomobject]@{ Missing = $missing; Total = $files.Targets.Count; Skipped = $files.Skipped }
}

function Assert-UrFilesPresent([string]$msi) {
  $result = Get-UrMissingFiles $msi
  if ($result.Missing.Count -ne 0) {
    $result.Missing | Select-Object -First 20 | ForEach-Object { Write-Host "missing: $_" }
    throw "$($result.Missing.Count) of $($result.Total) File rows of $(Split-Path -Leaf $msi) are not on disk"
  }
  Write-Host "OK: all $($result.Total) File rows of $(Split-Path -Leaf $msi) are on disk ($($result.Skipped) conditional or outside the install folder)"
}

# An msiexec /l*v log as text, whatever its encoding: UTF-16 or UTF-8 with a
# byte-order mark, else the ANSI code page.
function Read-UrLog([string]$logName) {
  $path = Join-Path $UrLogDir $logName
  if (-not (Test-Path -LiteralPath $path)) { throw "no log at $path" }
  $bytes = [IO.File]::ReadAllBytes($path)
  if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) {
    return [Text.Encoding]::Unicode.GetString($bytes, 2, $bytes.Length - 2)
  }
  if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
    return [Text.Encoding]::UTF8.GetString($bytes, 3, $bytes.Length - 3)
  }
  return [Text.Encoding]::Default.GetString($bytes)
}

# Functions that return a list return it with a leading comma, whole: with
# Set-StrictMode, an empty list unrolled into $null fails on .Count. Callers
# assign the result, or wrap the call in parentheses before piping it.
function Get-UrLogLines([string]$text, [string]$pattern) {
  return ,@($text -split "`r?`n" | Where-Object { $_ -match $pattern })
}

# Windows Installer skips a component whose key file on disk has a higher
# version, logs this line, and the old product's removal then deletes the
# file. Fails on it; the log must be a finished msiexec log, or the search
# would pass on an empty file.
function Assert-UrLogClean([string]$logName) {
  $text = Read-UrLog $logName
  if ($text -notmatch 'MainEngineThread is returning') {
    throw "$logName is not a finished msiexec log (no 'MainEngineThread is returning' line)"
  }
  $disallowed = Get-UrLogLines $text 'Disallowing installation of component'
  if ($disallowed.Count -ne 0) {
    $disallowed | Select-Object -First 10 | ForEach-Object { Write-Host $_ }
    throw "${logName}: $($disallowed.Count) component(s) disallowed for a higher-versioned file on disk"
  }
  Write-Host "OK: $logName has no 'Disallowing installation of component'"
}

# An install the guard refused: exit 1603, the guard's own message in the log
# (not the downgrade rule's), and nothing costed or removed: no component
# disallowed and no RemoveExistingProducts.
function Assert-UrRefusedByGuard([int]$code, [string]$logName) {
  if ($code -ne 1603) { throw "exit code $code, want 1603: the install was not refused" }
  $text = Read-UrLog $logName
  if (-not $text.Contains($UrGuardMessage)) { throw "$logName does not show the guard's message: $UrGuardMessage" }
  $disallowed = Get-UrLogLines $text 'Disallowing installation of component'
  if ($disallowed.Count -ne 0) { throw "${logName}: $($disallowed.Count) component(s) disallowed, so costing ran" }
  $removal = Get-UrLogLines $text 'Action start \d{1,2}:\d{2}:\d{2}: RemoveExistingProducts\.'
  if ($removal.Count -ne 0) { throw "${logName}: RemoveExistingProducts started" }
  (Get-UrLogLines $text 'Action ended \d{1,2}:\d{2}:\d{2}: LaunchConditions\.') | ForEach-Object { Write-Host "log: $_" }
  Write-Host "OK: refused by the guard (exit 1603), before any component was costed or removed"
}

# Every product registered under URnetwork's UpgradeCode, as
# @{ ProductCode; Version }.
function Get-UrProducts {
  $installer = New-Object -ComObject WindowsInstaller.Installer
  $codes = $null
  $products = @()
  try {
    $codes = Invoke-UrComGet $installer 'RelatedProducts' @($UrUpgradeCode)
    foreach ($code in @($codes)) {
      if (-not $code) { continue }
      $products += [pscustomobject]@{
        ProductCode = [string]$code
        Version     = [string](Invoke-UrComGet $installer 'ProductInfo' @($code, 'VersionString'))
      }
    }
  } finally {
    Close-UrCom $codes
    Close-UrCom $installer
  }
  return ,$products
}

function Get-UrFileVersion([string]$path) {
  $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($path)
  return '{0}.{1}.{2}.{3}' -f $info.FileMajorPart, $info.FileMinorPart, $info.FileBuildPart, $info.FilePrivatePart
}

function Get-UrServiceInfo {
  $service = Get-CimInstance Win32_Service -Filter "Name='urnetworkd'"
  if (-not $service) { return $null }
  return [pscustomobject]@{ State = [string]$service.State; PathName = [string]$service.PathName }
}

# Every service that is urnetworkd, by name or by binary.
function Get-UrServices {
  return ,@(Get-CimInstance Win32_Service | Where-Object { $_.Name -eq 'urnetworkd' -or $_.PathName -like '*urnetworkd.exe*' } |
    ForEach-Object { [pscustomobject]@{ Name = [string]$_.Name; State = [string]$_.State; PathName = [string]$_.PathName } })
}

function Wait-UrServiceRunning([int]$seconds = 30) {
  $deadline = (Get-Date).AddSeconds($seconds)
  do {
    $info = Get-UrServiceInfo
    if ($info -and $info.State -eq 'Running') { return $info }
    Start-Sleep -Milliseconds 500
  } while ((Get-Date) -lt $deadline)
  throw "urnetworkd is not running after $seconds s: $(Get-UrServiceInfo | Out-String)"
}

# msiexec with a verbose log; returns the exit code. $mode is /qn (tests) or
# /passive (what the in-app updater passes).
function Invoke-UrMsiexec([string]$verb, [string]$target, [string]$mode, [string]$logName) {
  $log = Join-Path $UrLogDir $logName
  $arguments = "$verb `"$target`" $mode /norestart /l*v `"$log`""
  Write-Host "msiexec $arguments"
  $process = Start-Process -FilePath "$env:SystemRoot\System32\msiexec.exe" -ArgumentList $arguments -Wait -PassThru
  Write-Host "msiexec exit code: $($process.ExitCode)"
  return $process.ExitCode
}

# Uninstalls every URnetwork product; nothing may remain registered.
function Remove-UrProducts([string]$logPrefix) {
  $index = 0
  foreach ($product in (Get-UrProducts)) {
    $index++
    $code = Invoke-UrMsiexec '/x' $product.ProductCode '/qn' "$logPrefix-$index.log"
    if ($code -ne 0 -and $code -ne 3010) { throw "uninstall of $($product.ProductCode) $($product.Version) exit code $code" }
  }
  if ((Get-UrProducts).Count -ne 0) { throw "a URnetwork product is still registered after the uninstall" }
}

# The state every install that must succeed leaves: exactly one URnetwork
# product, at the expected ProductVersion; urnetworkd running from the
# install folder; both exes carrying the expected file version; every File
# row of the package on disk; and, given its log, no component disallowed.
function Assert-UrInstalled($expect, [string]$logName = '') {
  $products = Get-UrProducts
  Write-Host "products: $($products | Format-Table -AutoSize | Out-String)"
  if ($products.Count -ne 1) { throw "want exactly one URnetwork product, found $($products.Count)" }
  if ($products[0].Version -ne $expect.msi_version) {
    throw "ProductVersion $($products[0].Version), want $($expect.msi_version) ($($expect.key))"
  }
  $service = Wait-UrServiceRunning
  if ($service.PathName -notlike "*$UrInstallDir\urnetworkd.exe*") {
    throw "urnetworkd runs $($service.PathName), not the installed copy"
  }
  foreach ($exe in 'URnetwork.exe', 'urnetworkd.exe') {
    $version = Get-UrFileVersion (Join-Path $UrInstallDir $exe)
    if ($version -ne $expect.file_version) {
      throw "$exe is $version, want $($expect.file_version) ($($expect.key))"
    }
  }
  Assert-UrFilesPresent (Get-UrMsiPath $expect.key)
  if ($logName) { Assert-UrLogClean $logName }
  Write-Host "OK: $($expect.key) installed: $($products[0].ProductCode) $($products[0].Version), files $($expect.file_version)"
  return $products[0]
}

function Get-UrServiceHash {
  return (Get-FileHash (Join-Path $UrInstallDir 'urnetworkd.exe') -Algorithm SHA256).Hash
}

# Top-level windows of one process, and WM_CLOSE for them: EnumWindows,
# GetWindowThreadProcessId and PostMessageW. A window message only; nothing
# here synthesizes mouse or keyboard input.
$UrWindowApiSource = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class UrWindowApi {
  private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);
  [DllImport("user32.dll")]
  private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);
  [DllImport("user32.dll")]
  private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  private static extern int GetClassNameW(IntPtr hwnd, StringBuilder name, int capacity);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  private static extern int GetWindowTextW(IntPtr hwnd, StringBuilder text, int capacity);
  [DllImport("user32.dll", SetLastError = true)]
  private static extern bool PostMessageW(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam);
  private const uint WM_CLOSE = 0x0010;

  public static IntPtr[] TopLevelWindows(uint processId) {
    List<IntPtr> found = new List<IntPtr>();
    EnumWindows(delegate (IntPtr hwnd, IntPtr lParam) {
      uint owner;
      GetWindowThreadProcessId(hwnd, out owner);
      if (owner == processId) { found.Add(hwnd); }
      return true;
    }, IntPtr.Zero);
    return found.ToArray();
  }

  public static string ClassName(IntPtr hwnd) {
    StringBuilder name = new StringBuilder(256);
    GetClassNameW(hwnd, name, name.Capacity);
    return name.ToString();
  }

  public static string Title(IntPtr hwnd) {
    StringBuilder text = new StringBuilder(256);
    GetWindowTextW(hwnd, text, text.Capacity);
    return text.ToString();
  }

  // 0 when posted, else the Win32 error.
  public static int PostClose(IntPtr hwnd) {
    return PostMessageW(hwnd, WM_CLOSE, IntPtr.Zero, IntPtr.Zero) ? 0 : Marshal.GetLastWin32Error();
  }
}
'@

function Initialize-UrWindowApi {
  if (-not ('UrWindowApi' -as [type])) { Add-Type -TypeDefinition $UrWindowApiSource }
}

function Get-UrTopLevelWindows([int]$processId) {
  Initialize-UrWindowApi
  return ,@([UrWindowApi]::TopLevelWindows([uint32]$processId) | ForEach-Object {
      [pscustomobject]@{ Handle = $_; Class = [UrWindowApi]::ClassName($_); Title = [UrWindowApi]::Title($_) }
    })
}

# Posts WM_CLOSE to every top-level window of the process; returns how many
# were posted. A window can be gone before its turn (error 1400), when an
# earlier WM_CLOSE already ended the app.
function Send-UrWmClose([int]$processId) {
  $posted = 0
  foreach ($window in (Get-UrTopLevelWindows $processId)) {
    $result = [UrWindowApi]::PostClose($window.Handle)
    $outcome = switch ($result) { 0 { 'posted' } 1400 { 'already gone (1400)' } default { "PostMessage error $result" } }
    Write-Host ("WM_CLOSE -> {0} class '{1}' title '{2}': {3}" -f $window.Handle, $window.Class, $window.Title, $outcome)
    if ($result -eq 0) { $posted++ }
  }
  return $posted
}
