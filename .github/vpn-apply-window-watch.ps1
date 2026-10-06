# Leg f's watcher (vpn-apply-legf.yml): records when a process's tray window
# disappears, or when the process exits, so the moment can be set against
# msiexec's log. Polls every 200 ms. Pure ASCII, Windows PowerShell 5.1.
#
# SPDX-License-Identifier: MPL-2.0
param(
  [Parameter(Mandatory = $true)][int]$ProcessId,
  [Parameter(Mandatory = $true)][string]$Out
)
. (Join-Path $PSScriptRoot 'vpn-apply-lib.ps1')
while ($true) {
  $now = (Get-Date).ToString('HH:mm:ss.fff')
  if (-not (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)) {
    Set-Content -Encoding ascii -LiteralPath $Out -Value "process exited at $now"
    break
  }
  $tray = @((Get-UrTopLevelWindows $ProcessId) | Where-Object { $_.Class -eq 'URnetworkTrayWindow' })
  if ($tray.Count -eq 0) {
    Set-Content -Encoding ascii -LiteralPath $Out -Value "tray window gone at $now"
    break
  }
  Start-Sleep -Milliseconds 200
}
