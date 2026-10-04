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

function Write-Step([string]$text) { Write-Host "==== $text" }

# The plan values for one MSI: key, version, code, msi_version, file_version
# (what the payload's exes carry).
function Get-UrExpect([string]$key) {
  $path = Join-Path $UrMsiDir "$key.json"
  if (-not (Test-Path $path)) { throw "no plan values for $key at $path" }
  return Get-Content -Raw $path | ConvertFrom-Json
}

function Get-UrMsiPath([string]$key) {
  $path = Join-Path $UrMsiDir "$key.msi"
  if (-not (Test-Path $path)) { throw "no MSI for $key at $path" }
  return $path
}

function Invoke-UrComGet($object, [string]$name, [object[]]$arguments) {
  return $object.GetType().InvokeMember($name, 'GetProperty', $null, $object, $arguments)
}

# Every product registered under URnetwork's UpgradeCode, as
# @{ ProductCode; Version }.
function Get-UrProducts {
  $installer = New-Object -ComObject WindowsInstaller.Installer
  $codes = Invoke-UrComGet $installer 'RelatedProducts' @($UrUpgradeCode)
  $products = @()
  foreach ($code in @($codes)) {
    if (-not $code) { continue }
    $products += [pscustomobject]@{
      ProductCode = [string]$code
      Version     = [string](Invoke-UrComGet $installer 'ProductInfo' @($code, 'VersionString'))
    }
  }
  return ,$products
}

# A built MSI's ProductVersion, read-only (msiOpenDatabaseModeReadOnly = 0).
function Get-UrMsiProductVersion([string]$msi) {
  $installer = New-Object -ComObject WindowsInstaller.Installer
  $db = $installer.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null, $installer, @($msi, 0))
  $view = $db.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $db,
    @("SELECT Value FROM Property WHERE Property='ProductVersion'"))
  $view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, $null) | Out-Null
  $record = $view.GetType().InvokeMember('Fetch', 'InvokeMethod', $null, $view, $null)
  $value = Invoke-UrComGet $record 'StringData' @(1)
  $view.GetType().InvokeMember('Close', 'InvokeMethod', $null, $view, $null) | Out-Null
  return [string]$value
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

# The state every successful install must leave: exactly one URnetwork
# product, at the expected ProductVersion; urnetworkd running from the
# install folder; both exes carrying the expected file version.
function Assert-UrInstalled($expect) {
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
  Write-Host "OK: $($expect.key) installed: $($products[0].ProductCode) $($products[0].Version), files $($expect.file_version)"
  return $products[0]
}

function Get-UrServiceHash {
  return (Get-FileHash (Join-Path $UrInstallDir 'urnetworkd.exe') -Algorithm SHA256).Hash
}
