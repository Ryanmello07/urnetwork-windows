# Separate elevated OS acceptance. Never used by selftest or the release build.
# SPDX-License-Identifier: MPL-2.0
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [switch]$DisposableVm,
  [ValidateSet('ARM64', 'x64')]
  [string]$Platform = 'ARM64',
  [string]$WintunDirectory,
  [string]$OutputDirectory,
  [switch]$BuildOnly,
  [switch]$DnsClearDiagnostic
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $DisposableVm) { throw 'Requires explicit disposable-VM acknowledgment.' }
# Resolve script-relative defaults in the body, as app/build.ps1 does for -File.
if (-not $WintunDirectory) {
  $WintunDirectory = Join-Path $PSScriptRoot '..\third_party\wintun'
}
if (-not $BuildOnly) {
  $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
  if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Native acceptance requires an elevated disposable Windows VM.'
  }
}
if (-not $OutputDirectory) {
  $OutputDirectory = Join-Path $env:TEMP ('urn-native-capture-' + [Guid]::NewGuid().ToString('N'))
}
if (Test-Path -LiteralPath $OutputDirectory) { throw 'OutputDirectory must be new; prior receipts are never overwritten.' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$WintunDirectory = (Resolve-Path -LiteralPath $WintunDirectory).Path
$arch = if ($Platform -eq 'ARM64') { 'arm64' } else { 'amd64' }
$dll = Join-Path $WintunDirectory "bin\$arch\wintun.dll"
$header = Join-Path $WintunDirectory 'wintun.h'
if (-not (Test-Path -LiteralPath $dll) -or -not (Test-Path -LiteralPath $header)) {
  throw 'Stage the project-pinned Wintun dependency first; this test never downloads dependencies.'
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Build Tools are required.' }
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\amd64\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild not found.' }
$project = Join-Path $PSScriptRoot 'capture-native-tests.vcxproj'
$app = (Resolve-Path "$PSScriptRoot\..").Path
$inputs = @($project, $PSCommandPath, "$PSScriptRoot\capture-native-tests.cpp", "$app\Directory.Build.props",
  "$app\src\Service\CaptureReadiness.h", "$app\src\Service\NetworkConfig.cpp", "$app\src\Service\NetworkConfig.h",
  "$app\src\Service\NetPolicy.h", "$app\src\Service\WfpPolicy.cpp", "$app\src\Service\WfpPolicy.h",
  "$app\src\Service\Wintun.cpp", "$app\src\Service\Wintun.h", "$app\src\Common\Ids.h",
  "$app\src\Common\Log.cpp", "$app\src\Common\Log.h", "$app\src\Common\Strings.cpp", "$app\src\Common\Strings.h",
  $dll, $header)
function Get-InputIdentity {
  @($inputs | ForEach-Object { $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $_; "$($hash.Hash) $($hash.Path)" }) -join "`n"
}
$before = Get-InputIdentity
$before | Set-Content -LiteralPath (Join-Path $OutputDirectory 'inputs-before.txt') -Encoding UTF8
$started = [DateTime]::UtcNow.ToString('o')
$testExit = $null
try {
  & $msbuild $project /t:Build /m:1 /nologo /v:minimal /p:Configuration=Release "/p:Platform=$Platform" `
      "/p:OutDir=$OutputDirectory/" "/p:IntDir=$OutputDirectory/obj/" "/p:NativeWintunDir=$WintunDirectory" `
      *> (Join-Path $OutputDirectory 'build.log')
  if ($LASTEXITCODE -ne 0) { throw "Native test compilation failed with exit $LASTEXITCODE; see build.log." }
  Copy-Item -LiteralPath $dll -Destination (Join-Path $OutputDirectory 'wintun.dll')
  $exe = Join-Path $OutputDirectory 'capture-native-tests.exe'
  if (-not $BuildOnly) {
    if (-not ('UrnNativeCaptureExit' -as [type])) {
      Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class UrnNativeCaptureExit {
  [DllImport("kernel32.dll", SetLastError = true)]
  [return: MarshalAs(UnmanagedType.Bool)]
  public static extern bool GetExitCodeProcess(IntPtr process, out uint code);
}
'@
    }
    $arguments = @('--disposable-vm')
    if ($DnsClearDiagnostic) { $arguments += '--dns-clear-diagnostic' }
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru -NoNewWindow `
        -RedirectStandardOutput (Join-Path $OutputDirectory 'stdout.log') `
        -RedirectStandardError (Join-Path $OutputDirectory 'stderr.log')
    try {
      # Pin the kernel process object before waiting. Windows PowerShell's
      # Start-Process projection can otherwise lose ExitCode after termination.
      $processHandle = $process.Handle
      if ($processHandle -eq [IntPtr]::Zero -or $processHandle -eq [IntPtr](-1)) {
        throw 'Native parent process handle is unavailable.'
      }
      if (-not $process.WaitForExit(1500000)) {
        $process.Kill()
        if (-not $process.WaitForExit(10000)) { throw 'Native parent did not terminate; discard overlay.' }
        throw 'Native acceptance exceeded its total deadline; discard overlay, do not claim cleanup.'
      }
      [uint32]$nativeCode = 0
      if (-not [UrnNativeCaptureExit]::GetExitCodeProcess($processHandle, [ref]$nativeCode) -or $nativeCode -eq 259) {
        throw 'Native parent terminal exit could not be read from its retained handle.'
      }
      $testExit = $nativeCode
      if ($testExit -ne 0) { throw "Native acceptance failed with exit $testExit; see restricted local logs." }
    } finally {
      if (-not $process.HasExited) {
        $process.Kill()
        $null = $process.WaitForExit(10000)
      }
      $process.Dispose()
    }
  }
} finally {
  $after = Get-InputIdentity
  $after | Set-Content -LiteralPath (Join-Path $OutputDirectory 'inputs-after.txt') -Encoding UTF8
  [ordered]@{ started_utc = $started; finished_utc = [DateTime]::UtcNow.ToString('o');
    platform = $Platform; build_only = [bool]$BuildOnly; native_exit = $testExit;
    inputs_equal = ($before -ceq $after); provider_validation = $false;
    diagnostic_only = [bool]$DnsClearDiagnostic } |
      ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'result.json') -Encoding UTF8
  if ($before -cne $after) { throw 'Native source inputs changed during this run; evidence is invalid.' }
}
Write-Host "Native acceptance artifacts: $OutputDirectory"
