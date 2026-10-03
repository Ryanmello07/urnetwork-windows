# SPDX-License-Identifier: MPL-2.0
#
# Guest half of the Windows insufficient-balance driver (main.go). Runs in the
# acceptance VM over ssh as builder. Every verb prints exactly one line
# "IBRESULT <json object>" on success and throws (nonzero exit) on failure.
# UI operations cannot run here: an ssh session has no desktop, so
# Invoke-Interactive runs uia.ps1 as a scheduled task in builder's
# interactive session (autologon) and exchanges its result through a file.
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$Verb,
  [string]$Op = "",
  [string]$Arg = ""
)

$ErrorActionPreference = "Stop"
$Dir = "C:\acceptance-ib"
$Credentials = Join-Path $Dir "credentials.json"
$AppPathFile = Join-Path $Dir "app-path.txt"
$AppLog = Join-Path $env:LOCALAPPDATA "URnetwork\app\logs\urnetwork-app.log"
$ServiceLog = "C:\ProgramData\URnetwork\service\logs\urnetworkd.log"
# AppController::ReactToBalance writes this once per posted tray notice
$NoticeMarker = "app: insufficient balance notice posted"
# the same endpoints the SDK acceptance agent uses (build/all/acceptance/main.go)
$PublicIpUrls = @("https://checkip.amazonaws.com/", "https://api.ipify.org/")
$TrafficUrl = "https://speed.cloudflare.com/__down?bytes=16000000"
# the build VM's own account (build/all/windows/packer/http/Autounattend.pkrtpl.xml)
$BuilderUser = "builder"
$BuilderPassword = "urnetwork-build"

. (Join-Path $Dir "run-windows-lib.ps1")

function Write-Result($Value) {
  Write-Output ("IBRESULT " + (ConvertTo-Json -InputObject $Value -Compress -Depth 6))
}

# Reads a file another process holds open for writing.
function Read-SharedText([string]$Path) {
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return "" }
  $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
  try {
    $reader = New-Object System.IO.StreamReader($stream)
    return $reader.ReadToEnd()
  }
  finally {
    $stream.Dispose()
  }
}

function Get-NoticeCount {
  $count = 0
  foreach ($line in ((Read-SharedText $AppLog) -split "`n")) {
    if ($line.Contains($NoticeMarker)) { $count++ }
  }
  return $count
}

# Capture prefixes on the URnetwork adapter; main.go decides what is a tunnel.
function Get-TunnelPrefixes {
  $routes = @(Get-NetRoute -InterfaceAlias URnetwork -AddressFamily IPv4 -ErrorAction SilentlyContinue |
      Where-Object { $_.DestinationPrefix -ne "255.255.255.255/32" })
  return @($routes | ForEach-Object { $_.DestinationPrefix })
}

function Invoke-Curl([string[]]$Arguments) {
  $id = [guid]::NewGuid().ToString("N")
  $out = Join-Path $env:TEMP "ib-curl-$id.out"
  $err = Join-Path $env:TEMP "ib-curl-$id.err"
  try {
    $code = Invoke-AcceptanceProcess -FilePath "$env:SystemRoot\System32\curl.exe" `
      -ArgumentList $Arguments -StandardOutput $out -StandardError $err
    return [pscustomobject]@{
      ExitCode = $code
      Output = (Get-Content -Raw -LiteralPath $out -ErrorAction SilentlyContinue)
      Error = (Get-Content -Raw -LiteralPath $err -ErrorAction SilentlyContinue)
    }
  }
  finally {
    Remove-Item -LiteralPath $out, $err -Force -ErrorAction SilentlyContinue
  }
}

# Runs uia.ps1 -Op in builder's interactive desktop session and returns its
# parsed result. The task is limited (medium integrity), like the app itself.
function Invoke-Interactive([string]$Operation, [string]$Argument, [int]$TimeoutSeconds = 200) {
  $io = Join-Path $Dir "io"
  New-Item -ItemType Directory -Force -Path $io | Out-Null
  $id = [guid]::NewGuid().ToString("N")
  $out = Join-Path $io "$id.json"
  $err = Join-Path $io "$id.err"
  $arguments = "-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$Dir\uia.ps1`" -Op $Operation -Out `"$out`" -Err `"$err`""
  if ($Argument) { $arguments += " -Arg `"$Argument`"" }
  $task = "urnetwork-ib-$id"
  $action = New-ScheduledTaskAction -Execute "powershell.exe" -Argument $arguments
  $principal = New-ScheduledTaskPrincipal -UserId $BuilderUser -LogonType Interactive -RunLevel Limited
  $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit (New-TimeSpan -Seconds ($TimeoutSeconds + 30))
  Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings $settings -Force | Out-Null
  try {
    Start-ScheduledTask -TaskName $task
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
      if (Test-Path -LiteralPath $err) {
        throw ("interactive ${Operation}: " + (Get-Content -Raw -LiteralPath $err).Trim())
      }
      if (Test-Path -LiteralPath $out) {
        return (Get-Content -Raw -LiteralPath $out | ConvertFrom-Json)
      }
      Start-Sleep -Milliseconds 250
    }
    $info = Get-ScheduledTaskInfo -TaskName $task
    throw "interactive $Operation timed out (task result $($info.LastTaskResult))"
  }
  finally {
    Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $out, $err -Force -ErrorAction SilentlyContinue
  }
}

switch ($Verb) {
  "boot-time" {
    $boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString("o")
    Write-Result @{ boot_time = $boot }
  }
  "enable-autologon" {
    # the throwaway overlay only: builder signs in to the console at boot, so
    # the app's window and tray exist for UI Automation
    $winlogon = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon"
    Set-ItemProperty -Path $winlogon -Name AutoAdminLogon -Value "1"
    Set-ItemProperty -Path $winlogon -Name DefaultUserName -Value $BuilderUser
    Set-ItemProperty -Path $winlogon -Name DefaultDomainName -Value $env:COMPUTERNAME
    Set-ItemProperty -Path $winlogon -Name DefaultPassword -Value $BuilderPassword
    Remove-ItemProperty -Path $winlogon -Name AutoLogonCount -ErrorAction SilentlyContinue
    # an idle console must not lock or sleep during the observation window
    & powercfg.exe /change monitor-timeout-ac 0 | Out-Null
    & powercfg.exe /change standby-timeout-ac 0 | Out-Null
    $personalization = "HKLM:\SOFTWARE\Policies\Microsoft\Windows\Personalization"
    New-Item -Path $personalization -Force | Out-Null
    Set-ItemProperty -Path $personalization -Name NoLockScreen -Value 1 -Type DWord
    $boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString("o")
    Write-Result @{ boot_time = $boot }
  }
  "reboot" {
    & shutdown.exe /r /t 3 /f | Out-Null
    Write-Result @{}
  }
  "session" {
    $interactive = $false
    foreach ($process in @(Get-CimInstance Win32_Process -Filter "Name='explorer.exe'")) {
      $owner = Invoke-CimMethod -InputObject $process -MethodName GetOwner
      if ($owner.User -eq $BuilderUser -and $process.SessionId -gt 0) { $interactive = $true }
    }
    Write-Result @{ interactive = $interactive }
  }
  "install" {
    $msi = Resolve-AcceptanceNativePath -LiteralPath (Join-Path $Dir "urnetwork.msi")
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $msi).Hash -ne $Arg) {
      throw "copied MSI hash does not match the locally built artifact"
    }
    $code = Invoke-AcceptanceMsi -Action Install -Msi $msi -Log (Join-Path $Dir "install.log")
    if ($code -ne 0) { throw "MSI install failed with exit code $code" }
    $service = $null
    for ($i = 0; $i -lt 60; $i++) {
      $service = Get-Service -Name urnetworkd -ErrorAction SilentlyContinue
      if ($service -and $service.Status -eq "Running") { break }
      Start-Sleep -Seconds 1
    }
    if (-not $service -or $service.Status -ne "Running") {
      throw "urnetworkd did not reach Running after installation"
    }
    $serviceConfig = Get-CimInstance Win32_Service -Filter "Name='urnetworkd'"
    $app = Join-Path (Split-Path -Parent $serviceConfig.PathName.Trim('"')) "URnetwork.exe"
    if (-not (Test-Path -LiteralPath $app -PathType Leaf)) {
      throw "URnetwork.exe was not installed beside urnetworkd.exe"
    }
    Set-Content -LiteralPath $AppPathFile -Value $app -Encoding ascii
    Wait-AcceptanceControlPlaneDns -HostNames @("api.bringyour.com", "connect.bringyour.com")
    Write-Result @{ app = $app }
  }
  "interactive" {
    try {
      Write-Result (Invoke-Interactive -Operation $Op -Argument $Arg)
    }
    finally {
      if ($Op -eq "login") { Remove-Item -LiteralPath $Credentials -Force -ErrorAction SilentlyContinue }
    }
  }
  "observe" {
    $uia = Invoke-Interactive -Operation "facts" -Argument "acceptance.connect|acceptance.insufficient-balance.alert|acceptance.insufficient-balance.upgrade"
    Write-Result @{ uia = $uia; routes = @(Get-TunnelPrefixes); notices = (Get-NoticeCount) }
  }
  "notices" {
    Write-Result @{ count = (Get-NoticeCount) }
  }
  "egress" {
    # one bounded probe per endpoint from this (non-app) process; failures
    # while the tunnel holds traffic are the expected answer
    $failures = @()
    foreach ($url in $PublicIpUrls) {
      $r = Invoke-Curl @("-s", "-S", "--max-time", "10", $url)
      if ($r.ExitCode -eq 0 -and $r.Output) {
        Write-Result @{ ip = $r.Output.Trim() }
        exit 0
      }
      $failures += "$url=curl $($r.ExitCode)"
    }
    Write-Result @{ error = ($failures -join "; ") }
  }
  "traffic" {
    $r = Invoke-Curl @("-s", "-o", "NUL", "--max-time", "30", $TrafficUrl)
    Write-Result @{ exit = $r.ExitCode }
  }
  "collect" {
    $outDir = Join-Path $Dir "out"
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $files = @()
    foreach ($pair in @(@($AppLog, "urnetwork-app.log"), @($ServiceLog, "urnetworkd.log"), @((Join-Path $Dir "install.log"), "install.log"))) {
      if (Test-Path -LiteralPath $pair[0] -PathType Leaf) {
        Set-Content -LiteralPath (Join-Path $outDir $pair[1]) -Value (Read-SharedText $pair[0]) -Encoding utf8
        $files += $pair[1]
      }
    }
    Write-Result @{ files = $files }
  }
  "cleanup-private" {
    Remove-Item -LiteralPath $Credentials -Force -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $Credentials) { throw "private credentials remained in the guest" }
    Write-Result @{}
  }
  default {
    throw "unknown verb $Verb"
  }
}
