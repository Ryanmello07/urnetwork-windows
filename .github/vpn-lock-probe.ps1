# A single probe, run by .github/vpn-lock.yml as a principal that is NOT an
# administrator (a scheduled task whose principal is NT AUTHORITY\LOCAL SERVICE,
# or a standard local account). It performs one file or mutex operation on a
# path the elevated side chose, and writes the outcome as JSON to -Out, so the
# caller can read what a process without administrator rights was able to do.
#
# It never elevates and never installs or launches a product. Modes:
#   open    try to open <Target> for its data (FILE_READ_DATA)
#   create  try to create <Target> (a new file) in updates\
#   delete  try to delete <Target>
#   rename  try to replace <Target> by renaming a sibling over it
#   mutex   create and own the named mutex <MutexName>, write "held", then wait
#           until <StopFile> appears (or the timeout), holding it throughout
#
# SPDX-License-Identifier: MPL-2.0
param(
  [Parameter(Mandatory = $true)][ValidateSet('open', 'create', 'delete', 'rename', 'mutex')][string]$Mode,
  [string]$Target = '',
  [Parameter(Mandatory = $true)][string]$Out,
  [string]$MutexName = '',
  [string]$StopFile = '',
  [int]$HoldSeconds = 600
)

$ErrorActionPreference = 'Stop'

function Write-Result([hashtable]$fields) {
  $fields['mode'] = $Mode
  $fields['target'] = $Target
  $fields['whoami'] = (whoami)
  $written = "$Out.new"
  ($fields | ConvertTo-Json -Compress) | Set-Content -Encoding ascii -LiteralPath $written
  Move-Item -Force -LiteralPath $written -Destination $Out
}

try {
  switch ($Mode) {
    'open' {
      # A plain FILE_READ_DATA open, no sharing asked: this is what a process
      # would do to read the lock, or to hold it the way the helper does.
      $ok = $false
      $err = ''
      try {
        $stream = [IO.FileStream]::new($Target, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
        $stream.Close()
        $ok = $true
      } catch {
        $err = $_.Exception.Message.Trim()
      }
      Write-Result @{ ok = $ok; error = $err }
    }
    'create' {
      # Try to add a new file under the name, as an attacker would to plant the
      # lock before the helper runs.
      $ok = $false
      $err = ''
      try {
        $stream = [IO.FileStream]::new($Target, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        $stream.Close()
        $ok = $true
      } catch {
        $err = $_.Exception.Message.Trim()
      }
      Write-Result @{ ok = $ok; error = $err }
    }
    'delete' {
      $ok = $false
      $err = ''
      try {
        [IO.File]::Delete($Target)
        $ok = $true
      } catch {
        $err = $_.Exception.Message.Trim()
      }
      Write-Result @{ ok = $ok; error = $err }
    }
    'rename' {
      # Make a sibling beside the output file (a directory this principal can
      # write), then try to move it over the lock.
      $ok = $false
      $err = ''
      try {
        $sibling = Join-Path (Split-Path -Parent $Out) ('urn-rename-' + [guid]::NewGuid().ToString('N') + '.tmp')
        [IO.File]::WriteAllText($sibling, 'x')
        [IO.File]::Move($sibling, $Target)
        $ok = $true
      } catch {
        $err = $_.Exception.Message.Trim()
      }
      Write-Result @{ ok = $ok; error = $err }
    }
    'mutex' {
      # Create and OWN the named mutex, as a process without administrator
      # rights that squats the helper's name would. Report whether the name was
      # new to us, then hold it until told to stop.
      $createdNew = $false
      $held = $false
      $err = ''
      try {
        $mutex = [Threading.Mutex]::new($true, $MutexName, [ref]$createdNew)
        # own it: with initiallyOwned = true the caller holds it when createdNew
        $held = $true
        Write-Result @{ ok = $true; createdNew = $createdNew; held = $true; error = '' }
        $deadline = (Get-Date).AddSeconds($HoldSeconds)
        while ((Get-Date) -lt $deadline) {
          if ($StopFile -and (Test-Path -LiteralPath $StopFile)) { break }
          Start-Sleep -Milliseconds 300
        }
        $mutex.ReleaseMutex()
        $mutex.Dispose()
      } catch {
        $err = $_.Exception.Message.Trim()
        Write-Result @{ ok = $false; createdNew = $createdNew; held = $held; error = $err }
      }
    }
  }
} catch {
  Write-Result @{ ok = $false; error = ("probe threw: " + $_.Exception.Message.Trim()) }
}
