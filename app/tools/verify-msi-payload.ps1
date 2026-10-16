# Asserts the built MSI actually carries a working app, not just that it
# compiled. `dotnet build app/installer/Installer.wixproj` is a compile-proof
# only -- it never inspected what landed inside the .msi -- and that gap is
# exactly how the MSI shipped for months installing 7 files (the exe, the
# SDK dll, resources.pri, the service exe, wintun.dll, and two license
# files) while silently omitting the self-contained WindowsAppRuntime and
# Assets\Fonts. An install from that MSI produced an app that could not
# start on any machine that had not already run the portable zip.
#
# Inspection method: the built-in Windows Installer COM object
# (WindowsInstaller.Installer), which ships with every Windows install --
# CI runners included -- so this needs no extra tooling (no msiinfo/msidb/
# lessmsi). It opens the MSI as a database and queries the File table
# directly; nothing is installed. One file is extracted: the update helper,
# from the embedded cabinet (msi.dll and expand.exe), to read its version
# resource, since a helper built with a runner test's feed must never ship.
#
# Run locally the same way CI does:
#   powershell -NoProfile -File app\tools\verify-msi-payload.ps1 -MsiPath app\installer\dist\URnetwork.msi
#
# SPDX-License-Identifier: MPL-2.0
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$MsiPath,

  # 304 files shipped in the portable zip for the 2026.8.11 beta (7
  # hand-authored + ~297 harvested WindowsAppRuntime/WinUI3/Assets files).
  # The floor is set well below that so a routine WindowsAppSDK bump
  # (which changes the exact runtime file count) does not make this flaky,
  # while still catching a regression back to the old 7-8 file MSI by a
  # wide margin.
  [int]$MinFileCount = 250,

  # Long filenames (as recorded in the MSI File table) that must be present
  # for the app to actually start and render in its own brand:
  #   - Microsoft.WindowsAppRuntime.dll / Microsoft.ui.xaml.dll: the
  #     self-contained runtime; their absence is the exact defect this
  #     script exists to catch.
  #   - resources.pri: without it every string renders as its key id.
  #   - pp_neue_montreal_regular.ttf: a brand font under Assets\Fonts; its
  #     absence means the app silently falls back to a system font.
  #   - App.xbf: the app's own compiled XAML for App.xaml. Without it the
  #     app cannot even construct its Application object.
  #   - vcruntime140.dll / vcruntime140_1.dll / msvcp140.dll: the app-local
  #     VC++ runtime (Service.vcxproj UrnStageVCRuntime). Without it the /MD
  #     urnetworkd.exe cannot load on a machine lacking the VC++
  #     Redistributable, and the MSI fails with error 1920.
  #   - URnetworkUpdate.exe: the in-app update's elevated helper, and the
  #     target of the MSI's relaunch after an update. Without it an installed
  #     copy cannot update itself, and the relaunch fails.
  [string[]]$RequireNames = @(
    "Microsoft.WindowsAppRuntime.dll",
    "Microsoft.ui.xaml.dll",
    "resources.pri",
    "pp_neue_montreal_regular.ttf",
    "App.xbf",
    "vcruntime140.dll",
    "vcruntime140_1.dll",
    "msvcp140.dll",
    "URnetworkUpdate.exe"
  )
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $MsiPath)) {
  throw "no MSI at $MsiPath"
}
$MsiPath = (Resolve-Path $MsiPath).Path

# --- read the File table via the Windows Installer COM object ----------------
$installer = New-Object -ComObject WindowsInstaller.Installer
# msiOpenDatabaseModeReadOnly = 0
$db = $installer.GetType().InvokeMember(
  "OpenDatabase", "InvokeMethod", $null, $installer, @($MsiPath, 0))
$sql = "SELECT File, FileName, FileSize FROM File"
$view = $db.GetType().InvokeMember("OpenView", "InvokeMethod", $null, $db, @($sql))
$view.GetType().InvokeMember("Execute", "InvokeMethod", $null, $view, $null)

$longNames = @()
$fileKeys = @{}
$fileCount = 0
while ($true) {
  $record = $view.GetType().InvokeMember("Fetch", "InvokeMethod", $null, $view, $null)
  if ($null -eq $record) { break }
  $fileCount++
  $fileKey = $record.GetType().InvokeMember("StringData", "GetProperty", $null, $record, 1)
  $fileName = $record.GetType().InvokeMember("StringData", "GetProperty", $null, $record, 2)
  # FileName is "shortname|longname" when the two differ (the common case
  # for the harvested payload), or just "shortname" when they are the same
  # (e.g. wintun.dll, already 8.3-safe). Always take the long form.
  $longName = ($fileName -split '\|')[-1]
  $longNames += $longName
  $fileKeys[$longName] = $fileKey
}

# The embedded cabinets (Media.Cabinet "#<stream>").
$cabinets = @()
$mediaView = $db.GetType().InvokeMember("OpenView", "InvokeMethod", $null, $db, @("SELECT Cabinet FROM Media"))
$mediaView.GetType().InvokeMember("Execute", "InvokeMethod", $null, $mediaView, $null)
while ($true) {
  $record = $mediaView.GetType().InvokeMember("Fetch", "InvokeMethod", $null, $mediaView, $null)
  if ($null -eq $record) { break }
  $cabinet = $record.GetType().InvokeMember("StringData", "GetProperty", $null, $record, 1)
  if ($cabinet.StartsWith("#")) { $cabinets += $cabinet.Substring(1) }
}

Write-Host "MSI file table: $fileCount files ($MsiPath)"

# --- assert: floor on total payload count -------------------------------------
$failures = @()
if ($fileCount -lt $MinFileCount) {
  $failures += "file count $fileCount is below the floor of $MinFileCount -- " +
    "this is the exact shape of the defect that shipped a non-starting MSI " +
    "(it had 7 files). Check that installer/Package.wxs's RuntimeFiles " +
    "<Files> harvest still points at a populated `$(var.BinDir) and that " +
    "-p:BinDir was passed to the wixproj build."
}

# --- assert: critical names present -------------------------------------------
foreach ($name in $RequireNames) {
  if ($longNames -notcontains $name) {
    $failures += "required file '$name' is not in the MSI payload"
  }
}

# --- assert: the packaged update helper polls the official feed ---------------
# The helper installs releases with administrator rights. One built with a
# runner test's feed says so in its FileDescription (src\Updater\Updater.rc)
# and must never ship. The packaged copy is taken out of the MSI's embedded
# cabinet (the stream read through msi.dll, then expand.exe; nothing is
# installed) and its version resource read.
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;

public static class UrMsiStreams {
  [DllImport("msi.dll", CharSet = CharSet.Unicode)]
  static extern uint MsiOpenDatabaseW(string path, IntPtr persist, out IntPtr database);
  [DllImport("msi.dll", CharSet = CharSet.Unicode)]
  static extern uint MsiDatabaseOpenViewW(IntPtr database, string query, out IntPtr view);
  [DllImport("msi.dll")]
  static extern uint MsiViewExecute(IntPtr view, IntPtr record);
  [DllImport("msi.dll")]
  static extern uint MsiViewFetch(IntPtr view, out IntPtr record);
  [DllImport("msi.dll")]
  static extern uint MsiRecordReadStream(IntPtr record, uint field, byte[] buffer, ref uint size);
  [DllImport("msi.dll")]
  static extern uint MsiCloseHandle(IntPtr handle);

  // Writes the MSI's stream `name` to `output`; false when it has none.
  public static bool Save(string msi, string name, string output) {
    IntPtr database;
    // MSIDBOPEN_READONLY is 0
    if (MsiOpenDatabaseW(msi, IntPtr.Zero, out database) != 0) throw new IOException("MsiOpenDatabase failed: " + msi);
    IntPtr view = IntPtr.Zero;
    IntPtr record = IntPtr.Zero;
    try {
      if (MsiDatabaseOpenViewW(database, "SELECT `Data` FROM `_Streams` WHERE `Name`='" + name + "'", out view) != 0 ||
          MsiViewExecute(view, IntPtr.Zero) != 0) {
        throw new IOException("the _Streams query failed for " + name);
      }
      if (MsiViewFetch(view, out record) != 0) return false;
      using (FileStream file = File.Create(output)) {
        byte[] buffer = new byte[1 << 20];
        while (true) {
          uint size = (uint)buffer.Length;
          if (MsiRecordReadStream(record, 1, buffer, ref size) != 0) throw new IOException("MsiRecordReadStream failed");
          if (size == 0) break;
          file.Write(buffer, 0, (int)size);
        }
      }
      return true;
    } finally {
      if (record != IntPtr.Zero) MsiCloseHandle(record);
      if (view != IntPtr.Zero) MsiCloseHandle(view);
      MsiCloseHandle(database);
    }
  }
}
'@
$helperKey = $fileKeys["URnetworkUpdate.exe"]
if ($helperKey) {
  $work = Join-Path ([IO.Path]::GetTempPath()) ("verify-msi-" + [Guid]::NewGuid().ToString("N"))
  New-Item -ItemType Directory -Force $work | Out-Null
  try {
    $extracted = $null
    foreach ($cabinet in $cabinets) {
      $cabFile = Join-Path $work $cabinet
      if (-not [UrMsiStreams]::Save($MsiPath, $cabinet, $cabFile)) { continue }
      & "$env:SystemRoot\System32\expand.exe" $cabFile "-F:$helperKey" $work | Out-Null
      $candidate = Join-Path $work $helperKey
      if (Test-Path -LiteralPath $candidate) { $extracted = $candidate; break }
    }
    if (-not $extracted) {
      $failures += "URnetworkUpdate.exe ($helperKey) is not in any of the MSI's embedded cabinets"
    } else {
      $description = [Diagnostics.FileVersionInfo]::GetVersionInfo($extracted).FileDescription
      if ($description -ne "URnetwork update") {
        $failures += "the packaged URnetworkUpdate.exe describes itself as '$description', not " +
          "'URnetwork update': it was built with a test feed and must not ship"
      } else {
        Write-Host "packaged URnetworkUpdate.exe: '$description' (the official feed)"
      }
    }
  } finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
  }
}

if ($failures.Count -gt 0) {
  Write-Host "MSI payload verification FAILED:" -ForegroundColor Red
  foreach ($f in $failures) { Write-Host "  - $f" -ForegroundColor Red }
  throw "MSI payload verification failed ($($failures.Count) issue(s)); see above"
}

Write-Host "MSI payload verification passed: $fileCount files, all $($RequireNames.Count) required names present." -ForegroundColor Green
