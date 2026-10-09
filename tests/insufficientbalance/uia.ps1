# SPDX-License-Identifier: MPL-2.0
#
# UI Automation half of the Windows insufficient-balance driver. guest.ps1
# runs it as a scheduled task in builder's interactive desktop session, where
# the URnetwork window lives. It finds controls only by the app's acceptance.*
# automation ids (tests/acceptance_automation_ids_test.go), reports raw facts,
# and leaves every decision to main.go. The result is one JSON object written
# atomically to -Out; a failure message goes to -Err.
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$Op,
  [Parameter(Mandatory = $true)][string]$Out,
  [Parameter(Mandatory = $true)][string]$Err,
  [string]$Arg = ""
)

$ErrorActionPreference = "Stop"
$Dir = "C:\acceptance-ib"

function Write-Atomic([string]$Path, [string]$Text) {
  $tmp = "$Path.tmp"
  Set-Content -LiteralPath $tmp -Value $Text -Encoding utf8
  Move-Item -LiteralPath $tmp -Destination $Path -Force
}

try {
  Add-Type -AssemblyName UIAutomationClient
  Add-Type -AssemblyName UIAutomationTypes
  $AutomationElement = [System.Windows.Automation.AutomationElement]
  $TreeScope = [System.Windows.Automation.TreeScope]
  $AppPath = (Get-Content -Raw -LiteralPath (Join-Path $Dir "app-path.txt")).Trim()

  function Get-AppWindow {
    $ids = @(Get-Process -Name URnetwork -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
    if ($ids.Count -eq 0) { return $null }
    $windows = $AutomationElement::RootElement.FindAll($TreeScope::Children,
      [System.Windows.Automation.Condition]::TrueCondition)
    # the XAML main window, not the tray's helper windows
    foreach ($window in $windows) {
      if (($ids -contains $window.Current.ProcessId) -and $window.Current.ClassName -eq "WinUIDesktopWin32WindowClass") {
        return $window
      }
    }
    return $null
  }

  # As a user would: the first launch goes to the notification area, and a
  # second launch is redirected to the running instance, which shows the
  # window (App::OnLaunched). Explorer starts it, so it is not a child of
  # this task and runs at the user's normal integrity.
  function Start-App {
    Start-Process -FilePath "$env:SystemRoot\explorer.exe" -ArgumentList "`"$AppPath`""
  }

  function Wait-AppWindow([int]$Seconds) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    $launched = (Get-Date).AddSeconds(-60)
    while ((Get-Date) -lt $deadline) {
      $window = Get-AppWindow
      if ($window) { return $window }
      if (((Get-Date) - $launched).TotalSeconds -ge 10) {
        Start-App
        $launched = Get-Date
      }
      Start-Sleep -Milliseconds 500
    }
    return $null
  }

  function Find-Id($Window, [string]$AutomationId) {
    $condition = New-Object System.Windows.Automation.PropertyCondition(
      $AutomationElement::AutomationIdProperty, $AutomationId)
    return $Window.FindFirst($TreeScope::Descendants, $condition)
  }

  function Wait-Id($Window, [string]$AutomationId, [int]$Seconds) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
      $element = Find-Id $Window $AutomationId
      if ($element -and -not $element.Current.IsOffscreen) { return $element }
      Start-Sleep -Milliseconds 500
    }
    return $null
  }

  function Get-Facts($Element) {
    if (-not $Element) { return @{ present = $false } }
    $current = $Element.Current
    return @{
      present = $true
      offscreen = [bool]$current.IsOffscreen
      enabled = [bool]$current.IsEnabled
      name = [string]$current.Name
    }
  }

  function Invoke-Element($Element) {
    $Element.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
  }

  function Select-Nav($Window, [string]$AutomationId) {
    $item = Wait-Id $Window $AutomationId 20
    if (-not $item) { throw "$AutomationId is not in the window" }
    $pattern = $null
    if ($item.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern, [ref]$pattern)) {
      $pattern.Select()
    }
    else {
      Invoke-Element $item
    }
  }

  function Set-Value($Element, [string]$Value, [string]$SendKeysValue) {
    $pattern = $null
    if ($Element.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref]$pattern)) {
      try {
        $pattern.SetValue($Value)
        return
      }
      catch {
        # a PasswordBox may refuse SetValue; type it instead
      }
    }
    Add-Type -AssemblyName System.Windows.Forms
    $Element.SetFocus()
    [System.Windows.Forms.SendKeys]::SendWait($SendKeysValue)
  }

  $window = Wait-AppWindow 90
  $result = $null
  switch ($Op) {
    "launch" {
      $result = @{ window = [bool]$window }
    }
    "facts" {
      $elements = @{}
      if ($window) {
        foreach ($automationId in $Arg.Split("|")) {
          $elements[$automationId] = Get-Facts (Find-Id $window $automationId)
        }
      }
      $result = @{ window = [bool]$window; elements = $elements }
    }
    "invoke" {
      if (-not $window) { throw "the URnetwork window did not open" }
      $parts = $Arg.Split("|")
      $element = Find-Id $window $parts[0]
      if (-not $element) {
        $result = @{ present = $false; invoked = $false; name = "" }
      }
      else {
        $name = [string]$element.Current.Name
        $invoked = $false
        # only the action the caller expects; never the opposite one
        if ($name -eq $parts[1] -and $element.Current.IsEnabled) {
          Invoke-Element $element
          $invoked = $true
        }
        $result = @{ present = $true; invoked = $invoked; name = $name }
      }
    }
    "login" {
      if (-not $window) { throw "the URnetwork window did not open" }
      $credentials = Get-Content -Raw -LiteralPath (Join-Path $Dir "credentials.json") | ConvertFrom-Json
      $detail = ""
      $user = Wait-Id $window "acceptance.password.user" 60
      if (-not $user) {
        $detail = "the sign-in field is not shown (already signed in or another page)"
      }
      else {
        Set-Value $user $credentials.email $credentials.email
        $next = Wait-Id $window "acceptance.password.next" 20
        $deadline = (Get-Date).AddSeconds(20)
        while ($next -and -not $next.Current.IsEnabled -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
        if (-not $next -or -not $next.Current.IsEnabled) {
          $detail = "Get started did not enable"
        }
        else {
          Invoke-Element $next
          $password = Wait-Id $window "acceptance.password.input" 60
          if (-not $password) {
            $detail = "the password step did not open (account discovery)"
          }
          else {
            Set-Value $password $credentials.password $credentials.password_sendkeys
            $submit = Wait-Id $window "acceptance.password.submit" 20
            if (-not $submit) {
              $detail = "the sign-in button is not shown"
            }
            else {
              Invoke-Element $submit
              if (-not (Wait-Id $window "acceptance.connect" 120)) {
                $detail = "the connect page did not open after sign-in"
              }
            }
          }
        }
      }
      $credentials = $null
      $result = @{ signed_in = ($detail -eq ""); detail = $detail }
    }
    "kill-switch" {
      if (-not $window) { throw "the URnetwork window did not open" }
      Select-Nav $window "acceptance.nav.settings"
      $toggle = Wait-Id $window "acceptance.settings.kill-switch" 20
      if (-not $toggle) {
        # the toggle may be below the fold; patterns work on it all the same
        $toggle = Find-Id $window "acceptance.settings.kill-switch"
      }
      if (-not $toggle) {
        $result = @{ found = $false; on = $false }
      }
      else {
        $pattern = $toggle.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
        $on = { $pattern.Current.ToggleState -eq [System.Windows.Automation.ToggleState]::On }
        if (($Arg -eq "on") -ne (& $on)) {
          $pattern.Toggle()
          Start-Sleep -Seconds 2
        }
        $result = @{ found = $true; on = [bool](& $on) }
      }
      Select-Nav $window "acceptance.nav.connect"
      if (-not (Wait-Id $window "acceptance.connect" 20)) { throw "the connect page did not return" }
    }
    default {
      throw "unknown operation $Op"
    }
  }
  Write-Atomic $Out (ConvertTo-Json -InputObject $result -Compress -Depth 6)
}
catch {
  Write-Atomic $Err ([string]$_.Exception.Message)
  exit 1
}
