#requires -RunAsAdministrator
<#
===============================================================================
revert_ssh_and_firewall.ps1
===============================================================================
Purpose:
  Revert OpenSSH Server configuration and system changes as desired:
    - Stop and disable the sshd service
    - Remove Windows Firewall rules for the SSH port
    - Optionally uninstall the OpenSSH Server capability
    - Optionally delete host keys under C:\ProgramData\ssh
    - Optionally restore a previous sshd_config backup created by setup script
    - Optionally verify that the TCP port is no longer reachable

This script is designed to pair with setup_ssh_and_firewall.ps1 which may
normalize sshd_config (e.g., inserting "Port <N>" before the first Match block).

Usage examples:
  .\revert_ssh_and_firewall.ps1
  .\revert_ssh_and_firewall.ps1 -DetectPortFromConfig -VerifyPort
  .\revert_ssh_and_firewall.ps1 -AllPorts -UninstallCapability -DeleteHostKeys
  .\revert_ssh_and_firewall.ps1 -RestoreConfig
  .\revert_ssh_and_firewall.ps1 -Port 2222 -VerifyPort -VerboseLog

Exit codes:
  0 = success
  1 = general failure
  2 = failed to stop or disable sshd service
  3 = firewall rule removal failed
  4 = capability uninstall failed
  5 = local TCP port still reachable after revert (when -VerifyPort specified)
===============================================================================
#>

param(
  [int]    $Port = 22,                       # Target SSH port for firewall cleanup and verification
  [switch] $DetectPortFromConfig,            # Detect effective port from current sshd_config instead of using -Port
  [switch] $AllPorts,                        # Remove all "OpenSSH Server" inbound rules regardless of port
  [switch] $SkipFirewall,                    # Do not modify firewall rules
  [switch] $SkipServiceStop,                 # Do not stop/disable sshd service
  [switch] $UninstallCapability,             # Remove OpenSSH Server Windows capability
  [switch] $DeleteHostKeys,                  # Delete C:\ProgramData\ssh\ssh_host_* key files
  [switch] $RestoreConfig,                   # Restore latest sshd_config backup (*.bak_YYYYMMDD_HHMMSS)
  [string] $ConfigPath = "C:\ProgramData\ssh\sshd_config",
  [switch] $VerifyPort,                      # Verify port is not reachable after revert
  [switch] $VerboseLog                       # Start transcript to TEMP
)

# -----------------------------------------------------------------------------
# UI helpers
# -----------------------------------------------------------------------------
function Write-Info ($msg)  { Write-Host "[INFO ] $msg" -ForegroundColor Cyan }
function Write-Ok   ($msg)  { Write-Host "[ OK  ] $msg" -ForegroundColor Green }
function Write-Warn ($msg)  { Write-Host "[WARN ] $msg" -ForegroundColor Yellow }
function Write-Err  ($msg)  { Write-Host "[ERROR] $msg" -ForegroundColor Red }

# -----------------------------------------------------------------------------
# Admin check
# -----------------------------------------------------------------------------
function Test-IsAdmin {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  $p  = New-Object Security.Principal.WindowsPrincipal($id)
  return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
if (-not (Test-IsAdmin)) {
  Write-Err "This script must be run as Administrator."
  exit 1
}

# -----------------------------------------------------------------------------
# Optional transcript
# -----------------------------------------------------------------------------
$transcriptStarted = $false
if ($VerboseLog) {
  try {
    $ts = Join-Path $env:TEMP ("ssh_revert_" + (Get-Date -Format "yyyyMMdd_HHmmss") + ".log")
    Start-Transcript -Path $ts -ErrorAction Stop | Out-Null
    $transcriptStarted = $true
    Write-Info "Transcript started: $ts"
  } catch {
    Write-Warn "Could not start transcript: $($_.Exception.Message)"
  }
}

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------
function Get-EffectivePortFromConfig {
  param([string] $Path, [int] $Fallback)
  if (-not (Test-Path $Path)) { return $Fallback }
  try {
    $lines = Get-Content $Path -ErrorAction Stop
    $ports = @()
    foreach ($ln in $lines) {
      if ($ln -match '^\s*Port\s+(\d+)\s*$' -and ($ln -notmatch '^\s*#')) {
        $ports += [int]$matches[1]
      }
    }
    if ($ports.Count -gt 0) { return $ports[-1] } else { return $Fallback }
  } catch {
    return $Fallback
  }
}

# -----------------------------------------------------------------------------
# Service control
# -----------------------------------------------------------------------------
function Stop-And-Disable-SSHD {
  Write-Info "Stopping and disabling sshd service..."
  try {
    $svc = Get-Service sshd -ErrorAction SilentlyContinue
    if (-not $svc) {
      Write-Ok "sshd service not present. Skipping stop/disable."
      return $true
    }

    if ($svc.Status -ne 'Stopped') {
      try {
        Stop-Service sshd -Force -ErrorAction Stop
      } catch {
        Write-Warn "Stop-Service failed, retrying with sc.exe..."
        sc.exe stop sshd | Out-Null
      }
    }

    $stopped = $false
    for ($i=0; $i -lt 20; $i++) {
      Start-Sleep -Milliseconds 300
      $svc = Get-Service sshd -ErrorAction SilentlyContinue
      if ($svc -and $svc.Status -eq 'Stopped') { $stopped = $true; break }
      if (-not $svc) { $stopped = $true; break }
    }
    if (-not $stopped) {
      Write-Err "sshd did not reach Stopped state."
      return $false
    }

    try {
      Set-Service sshd -StartupType Disabled -ErrorAction Stop
    } catch {
      Write-Warn "Failed to set StartupType Disabled: $($_.Exception.Message)"
    }

    Write-Ok "sshd stopped and disabled."
    return $true
  } catch {
    Write-Err "Failed to stop/disable sshd: $($_.Exception.Message)"
    return $false
  }
}

# -----------------------------------------------------------------------------
# Firewall cleanup
# -----------------------------------------------------------------------------
function Remove-OpenSSH-FirewallRules {
  param([int] $TargetPort, [switch] $All)

  Write-Info "Removing Windows Firewall inbound rules for OpenSSH..."
  try {
    # 1) enumerate candidate rules
    $rules = Get-NetFirewallRule -ErrorAction SilentlyContinue | Where-Object {
      $_.Direction -eq 'Inbound' -and
      ($_.DisplayName -match '^OpenSSH Server' -or $_.DisplayGroup -match 'OpenSSH')
    }

    if (-not $rules -or $rules.Count -eq 0) {
      Write-Ok "No OpenSSH inbound rules found."
      return $true
    }

    # 2) if not -All, filter to rules that match TCP + desired port
    if (-not $All) {
      $filtered = @()
      foreach ($r in $rules) {
        $ports = Get-NetFirewallPortFilter -AssociatedNetFirewallRule $r -ErrorAction SilentlyContinue
        $tcpMatch = $ports | Where-Object { $_.Protocol -eq 'TCP' -and $_.LocalPort -eq "$TargetPort" }
        if ($tcpMatch) { $filtered += $r }
      }
      $rules = $filtered
    }

    # 3) deduplicate by unique Name to avoid double removal
    $rules = $rules | Sort-Object -Property Name -Unique

    if (-not $rules -or $rules.Count -eq 0) {
      Write-Ok "No matching OpenSSH inbound rules to remove."
      return $true
    }

    # 4) remove, but tolerate already-deleted objects
    foreach ($r in $rules) {
      try {
        Remove-NetFirewallRule -Name $r.Name -ErrorAction Stop | Out-Null
        Write-Ok ("Removed firewall rule: {0}" -f $r.DisplayName)
      } catch {
        # If the rule vanished after we enumerated it, treat as already removed
        Write-Warn ("Skip (already removed or missing): {0} - {1}" -f $r.DisplayName, $_.Exception.Message)
      }
    }
    return $true

  } catch {
    Write-Err "Firewall rule enumeration failed: $($_.Exception.Message)"
    return $false
  }
}

# -----------------------------------------------------------------------------
# Capability uninstall
# -----------------------------------------------------------------------------
function Uninstall-OpenSSHServerCapability {
  Write-Info "Checking OpenSSH Server capability to uninstall..."
  try {
    $cap = Get-WindowsCapability -Online -Name OpenSSH.Server* -ErrorAction Stop
  } catch {
    Write-Err "Get-WindowsCapability failed: $($_.Exception.Message)"
    return $false
  }

  if ($cap.State -ne 'Installed') {
    Write-Ok "OpenSSH Server capability is not installed."
    return $true
  }

  Write-Info "Uninstalling OpenSSH Server capability..."
  try {
    Remove-WindowsCapability -Online -Name $cap.Name -ErrorAction Stop | Out-Null
    Write-Ok "OpenSSH Server capability removed."
    return $true
  } catch {
    Write-Err "Failed to remove capability: $($_.Exception.Message)"
    return $false
  }
}

# -----------------------------------------------------------------------------
# Host key deletion
# -----------------------------------------------------------------------------
function Delete-HostKeys {
  $sshDir = Join-Path $env:ProgramData "ssh"
  if (-not (Test-Path $sshDir)) {
    Write-Ok "C:\ProgramData\ssh not found. Skipping host key deletion."
    return $true
  }

  Write-Info "Deleting host keys in C:\ProgramData\ssh ..."
  try {
    $deletedAny = $false
    Get-ChildItem (Join-Path $sshDir "ssh_host_*") -ErrorAction SilentlyContinue | ForEach-Object {
      try {
        Remove-Item $_.FullName -Force -ErrorAction Stop
        Write-Ok ("Deleted {0}" -f $_.Name)
        $deletedAny = $true
      } catch {
        Write-Err ("Failed to delete {0}: {1}" -f $_.Name, $_.Exception.Message)
        throw
      }
    }
    if (-not $deletedAny) {
      Write-Ok "No host keys were found to delete."
    }
    return $true
  } catch {
    Write-Err "Host key deletion encountered an error: $($_.Exception.Message)"
    return $false
  }
}

# -----------------------------------------------------------------------------
# Restore sshd_config from backup
# -----------------------------------------------------------------------------
function Restore-SSHDConfig {
  param([string] $Path)

  $dir = Split-Path $Path -Parent
  if (-not (Test-Path $dir)) {
    Write-Ok "Config directory not present. Nothing to restore."
    return $true
  }

  $backups = Get-ChildItem -Path $dir -Filter "sshd_config.bak_*" -File -ErrorAction SilentlyContinue |
             Sort-Object -Property LastWriteTime -Descending
  if (-not $backups -or $backups.Count -eq 0) {
    Write-Warn "No sshd_config backups found (pattern: sshd_config.bak_YYYYMMDD_HHMMSS)."
    return $true
  }

  $latest = $backups[0].FullName
  Write-Info ("Restoring from backup: {0}" -f $latest)
  try {
    Copy-Item $latest $Path -Force
    Write-Ok "sshd_config restored."
    return $true
  } catch {
    Write-Err "Failed to restore sshd_config: $($_.Exception.Message)"
    return $false
  }
}

# -----------------------------------------------------------------------------
# Local port verification
# -----------------------------------------------------------------------------
function Verify-Port-Closed {
  param([int] $LocalPort)

  Write-Info "Verifying localhost:$LocalPort is NOT reachable..."
  $reachable = $false

  try {
    $c6 = New-Object System.Net.Sockets.TcpClient
    $ar6 = $c6.BeginConnect("::1", $LocalPort, $null, $null)
    if ($ar6.AsyncWaitHandle.WaitOne(1000) -and $c6.Connected) { $reachable = $true }
    $c6.Close()
  } catch { }

  if (-not $reachable) {
    try {
      $c4 = New-Object System.Net.Sockets.TcpClient
      $ar4 = $c4.BeginConnect("127.0.0.1", $LocalPort, $null, $null)
      if ($ar4.AsyncWaitHandle.WaitOne(1000) -and $c4.Connected) { $reachable = $true }
      $c4.Close()
    } catch { }
  }

  if ($reachable) {
    Write-Err "TCP $LocalPort is still reachable locally."
    return $false
  } else {
    Write-Ok "TCP $LocalPort is not reachable locally."
    return $true
  }
}

# -----------------------------------------------------------------------------
# Main flow
# -----------------------------------------------------------------------------
try {
  # 0) Determine effective port if requested
  $effectivePort = if ($DetectPortFromConfig) {
    $p = Get-EffectivePortFromConfig -Path $ConfigPath -Fallback $Port
    Write-Info ("Detected effective port from config: {0}" -f $p)
    $p
  } else {
    $Port
  }

  # 1) Stop and disable service
  if (-not $SkipServiceStop) {
    $svcOk = Stop-And-Disable-SSHD
    if (-not $svcOk) { exit 2 }
  } else {
    Write-Warn "Skipping service stop/disable as requested."
  }

  # 2) Remove firewall rules
  if (-not $SkipFirewall) {
    $fwOk = Remove-OpenSSH-FirewallRules -TargetPort $effectivePort -All:$AllPorts
    if (-not $fwOk) { exit 3 }
  } else {
    Write-Warn "Skipping firewall cleanup as requested."
  }

  # 3) Optional capability uninstall
  if ($UninstallCapability) {
    $unOk = Uninstall-OpenSSHServerCapability
    if (-not $unOk) { exit 4 }
  } else {
    Write-Info "Capability uninstall not requested."
  }

  # 4) Optional host key deletion
  if ($DeleteHostKeys) {
    $keysOk = Delete-HostKeys
    if (-not $keysOk) { exit 1 }
  } else {
    Write-Info "Host key deletion not requested."
  }

  # 5) Optional config restore
  if ($RestoreConfig) {
    $cfgOk = Restore-SSHDConfig -Path $ConfigPath
    if (-not $cfgOk) { exit 1 }
  } else {
    Write-Info "Config restore not requested."
  }

  # 6) Optional port verification
  if ($VerifyPort) {
    $okClosed = Verify-Port-Closed -LocalPort $effectivePort
    if (-not $okClosed) { exit 5 }
  } else {
    Write-Info "Port verification not requested."
  }

  Write-Ok "Revert completed."
  exit 0

} catch {
  Write-Err ("Unhandled exception: {0}" -f $_.Exception.Message)
  exit 1
} finally {
  if ($transcriptStarted) {
    try { Stop-Transcript | Out-Null } catch {}
  }
}
