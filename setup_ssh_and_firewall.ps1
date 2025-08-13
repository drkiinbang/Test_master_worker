#requires -RunAsAdministrator
<#
===============================================================================
setup_ssh_and_firewall.ps1
===============================================================================
Purpose:
  Install and configure OpenSSH Server on Windows. Ensure host keys and ACLs,
  optionally normalize sshd_config (NormalizeConfig), validate config, start
  the sshd service, open Windows Firewall, and test local connectivity.

Key features:
  - NormalizeConfig: auto-fix sshd_config encoding and Port lines
  - Robust service start with state check
  - Optional ACL repair for host keys and authorized_keys
  - Firewall rule ensure for the effective port
  - Local TCP reachability test (IPv6/IPv4 loopback)

Usage examples:
  .\setup_ssh_and_firewall.ps1
  .\setup_ssh_and_firewall.ps1 -Port 22 -NormalizeConfig
  .\setup_ssh_and_firewall.ps1 -Port 2222 -NormalizeConfig -IncludeAuthorizedKeysACL
  .\setup_ssh_and_firewall.ps1 -SkipACLRepair -SkipFirewall
  .\setup_ssh_and_firewall.ps1 -VerboseLog

Notes:
  - Run as Administrator.
  - If NormalizeConfig is off and your sshd_config contains a different port,
    this script detects and uses that port for firewall and testing.
  - Comments are ASCII-only.
===============================================================================
#>

param(
  [int]    $Port = 22,                          # Desired SSH listening port when NormalizeConfig is used
  [switch] $NormalizeConfig,                    # If set, normalize sshd_config and enforce Port=$Port
  [string] $ConfigPath = "C:\ProgramData\ssh\sshd_config",
  [switch] $SkipACLRepair,                      # Skip ACL repair for host keys
  [switch] $IncludeAuthorizedKeysACL,           # If set, also repair ACL of current user's authorized_keys
  [switch] $SkipFirewall,                       # Skip firewall rule handling
  [switch] $VerboseLog                          # Start transcript logging
)

# ------------------------------------------------------------------------------
# UI helpers
# ------------------------------------------------------------------------------
function Write-Info ($msg)  { Write-Host "[INFO ] $msg" -ForegroundColor Cyan }
function Write-Ok   ($msg)  { Write-Host "[ OK  ] $msg" -ForegroundColor Green }
function Write-Warn ($msg)  { Write-Host "[WARN ] $msg" -ForegroundColor Yellow }
function Write-Err  ($msg)  { Write-Host "[ERROR] $msg" -ForegroundColor Red }

# ------------------------------------------------------------------------------
# Admin check
# ------------------------------------------------------------------------------
function Test-IsAdmin {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  $p  = New-Object Security.Principal.WindowsPrincipal($id)
  return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
if (-not (Test-IsAdmin)) {
  Write-Err "This script must be run as Administrator."
  exit 1
}

# ------------------------------------------------------------------------------
# Optional transcript
# ------------------------------------------------------------------------------
$transcriptStarted = $false
if ($VerboseLog) {
  try {
    $ts = Join-Path $env:TEMP ("ssh_setup_" + (Get-Date -Format "yyyyMMdd_HHmmss") + ".log")
    Start-Transcript -Path $ts -ErrorAction Stop | Out-Null
    $transcriptStarted = $true
    Write-Info "Transcript started: $ts"
  } catch {
    Write-Warn "Could not start transcript: $($_.Exception.Message)"
  }
}

# ------------------------------------------------------------------------------
# Capability / install
# ------------------------------------------------------------------------------
function Ensure-OpenSSHServerInstalled {
  Write-Info "Checking OpenSSH Server capability..."
  try {
    $cap = Get-WindowsCapability -Online -Name OpenSSH.Server* -ErrorAction Stop
  } catch {
    Write-Err "Get-WindowsCapability failed. Are you on a supported Windows build?"
    throw
  }

  if ($cap.State -eq 'Installed') {
    Write-Ok "OpenSSH Server is already installed."
    return
  }

  Write-Info "Installing OpenSSH Server capability..."
  try {
    Add-WindowsCapability -Online -Name $cap.Name -ErrorAction Stop | Out-Null
    Write-Ok "OpenSSH Server installed."
  } catch {
    Write-Err "Failed to install OpenSSH Server capability: $($_.Exception.Message)"
    throw
  }
}

# ------------------------------------------------------------------------------
# Host keys generation
# ------------------------------------------------------------------------------
function Ensure-HostKeys {
  $sshDir = Join-Path $env:ProgramData "ssh"
  if (-not (Test-Path $sshDir)) {
    Write-Info "Creating directory: $sshDir"
    New-Item -ItemType Directory -Path $sshDir -Force | Out-Null
  }

  $needKeys = @(
    "ssh_host_ed25519_key",
    "ssh_host_rsa_key"
  ) | ForEach-Object { Join-Path $sshDir $_ }

  $missing = $needKeys | Where-Object { -not (Test-Path $_) }
  if ($missing.Count -gt 0) {
    Write-Info "Generating host keys (ssh-keygen -A)..."
    $keygen = Join-Path $env:SystemRoot "System32\OpenSSH\ssh-keygen.exe"
    if (-not (Test-Path $keygen)) { throw "ssh-keygen.exe not found at $keygen" }
    & $keygen -A
    Write-Ok "Host keys generated."
  } else {
    Write-Ok "Host keys already exist."
  }
}

# ------------------------------------------------------------------------------
# ACL repair for host keys and optional authorized_keys
# ------------------------------------------------------------------------------
function Repair-SSH-ACLs {
  if ($SkipACLRepair) {
    Write-Warn "Skipping ACL repair as requested."
    return
  }

  $psd1 = Join-Path $env:SystemRoot "System32\OpenSSH\OpenSSHUtils.psd1"
  if (-not (Test-Path $psd1)) {
    Write-Warn "OpenSSHUtils.psd1 not found. Skipping ACL repair."
    return
  }

  Write-Info "Importing OpenSSHUtils module..."
  Import-Module $psd1 -Force -ErrorAction SilentlyContinue

  $sshDir = Join-Path $env:ProgramData "ssh"
  $keys = Get-ChildItem (Join-Path $sshDir "ssh_host_*") -ErrorAction SilentlyContinue
  foreach ($k in $keys) {
    try {
      Repair-SshdHostKeyPermission -FilePath $k.FullName -ErrorAction Stop
      Write-Ok "Host key ACL repaired: $($k.Name)"
    } catch {
      Write-Warn "Failed to repair ACL for $($k.Name): $($_.Exception.Message)"
    }
  }

  if ($IncludeAuthorizedKeysACL) {
    $auth = Join-Path $env:USERPROFILE ".ssh\authorized_keys"
    if (Test-Path $auth) {
      try {
        Repair-AuthorizedKeyPermission -FilePath $auth -ErrorAction Stop
        Write-Ok "authorized_keys ACL repaired."
      } catch {
        Write-Warn "Failed to repair authorized_keys ACL: $($_.Exception.Message)"
      }
    } else {
      Write-Warn "authorized_keys not found for current user. Skipping."
    }
  }
}

# ------------------------------------------------------------------------------
# Config: helpers
# ------------------------------------------------------------------------------
function Get-EffectivePortFromConfig {
  param([string] $Path, [int] $Fallback)
  if (-not (Test-Path $Path)) { return $Fallback }
  try {
    # Read as text ignoring previous encoding details
    $lines = Get-Content $Path -ErrorAction Stop
    $ports = @()
    foreach ($ln in $lines) {
      if ($ln -match '^\s*Port\s+(\d+)\s*$' -and ($ln -notmatch '^\s*#')) {
        $ports += [int]$matches[1]
      }
    }
    if ($ports.Count -gt 0) {
      return $ports[-1]  # last one wins
    } else {
      return $Fallback
    }
  } catch {
    return $Fallback
  }
}

function Normalize-SSHDConfig {
  param([string] $Path, [int] $DesiredPort)

  Write-Info "Normalizing sshd_config at: $Path"

  if (-not (Test-Path $Path)) {
    Write-Warn "sshd_config not found. Creating a minimal one."
    $dir = Split-Path $Path -Parent
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    $minimal = @"
# minimal sshd_config created by setup script
Port $DesiredPort
"@
    Set-Content -Path $Path -Value $minimal -Encoding utf8 -Force
    Write-Ok "Created minimal sshd_config with Port $DesiredPort"
    return
  }

  $backup = "$Path.bak_$(Get-Date -Format yyyyMMdd_HHmmss)"
  Copy-Item $Path $backup -Force
  Write-Info "Backup created: $backup"

  $text = Get-Content $Path -Raw
  $text = [regex]::Replace($text, "[\u200B-\u200D\uFEFF]", "")
  $map = @{ '０'='0';'１'='1';'２'='2';'３'='3';'４'='4';'５'='5';'６'='6';'７'='7';'８'='8';'９'='9' }
  foreach($k in $map.Keys){ $text = $text -replace [regex]::Escape($k), $map[$k] }

  $lines = $text -split "`r?`n"

  # comment out every Port line
  for ($i=0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^\s*Port\b' -and $lines[$i] -notmatch '^\s*#') {
      $lines[$i] = "# " + $lines[$i]
    }
  }

  # find first un-commented 'Match' and insert Port before it (global scope)
  $firstMatch = -1
  for ($i=0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^\s*#') { continue }
    if ($lines[$i] -match '^\s*Match\b') { $firstMatch = $i; break }
  }

  $insertAt = if ($firstMatch -ge 0) { $firstMatch } else { $lines.Count }
  if ($insertAt -gt 0 -and $lines[$insertAt-1] -ne "") {
    $lines = $lines[0..($insertAt-1)] + @("") + $lines[$insertAt..($lines.Count-1)]
    $insertAt++
  }

  $toInsert = @("# normalized by setup_ssh script","Port $DesiredPort","")
  $newLines = $lines[0..($insertAt-1)] + $toInsert + $lines[$insertAt..($lines.Count-1)]

  Set-Content -Path $Path -Value ($newLines -join "`r`n") -Encoding utf8 -Force
  Write-Ok "sshd_config normalized and saved as UTF-8 (Port $DesiredPort)."
}

function Test-SSHDConfig {
  param([string] $Path)
  $sshd = Join-Path $env:SystemRoot "System32\OpenSSH\sshd.exe"
  if (-not (Test-Path $sshd)) { throw "sshd.exe not found at $sshd" }

  if (Test-Path $Path) {
    Write-Info "Validating sshd_config syntax..."
    $p = Start-Process -FilePath $sshd -ArgumentList @("-t","-f",$Path) -NoNewWindow -PassThru -Wait
    if ($p.ExitCode -ne 0) {
      Write-Err "sshd_config validation failed. Please fix the reported errors."
      return $false
    } else {
      Write-Ok "sshd_config syntax is valid."
      return $true
    }
  } else {
    Write-Warn "sshd_config not found. Using defaults."
    return $true
  }
}

# ------------------------------------------------------------------------------
# Service configuration and start
# ------------------------------------------------------------------------------
function Configure-And-Start-SSHD {
  Write-Info "Configuring sshd service..."
  try { sc.exe query sshd | Out-Null } catch {
    Write-Err "sshd service not found. Installation may have failed."
    throw
  }

  try { Set-Service -Name sshd -StartupType Automatic -ErrorAction Stop }
  catch { Write-Err "Failed to set sshd StartupType Automatic: $($_.Exception.Message)"; throw }

  Write-Info "Starting sshd service..."
  try { Start-Service sshd -ErrorAction Stop }
  catch { Write-Err "Failed to start sshd: $($_.Exception.Message)"; throw }

  $ok = $false
  for ($i=0; $i -lt 15; $i++) {
    Start-Sleep -Milliseconds 300
    try {
      $svc = Get-Service sshd -ErrorAction Stop
      if ($svc.Status -eq 'Running') { $ok = $true; break }
    } catch { }
  }
  if (-not $ok) {
    Write-Err "sshd did not reach Running state."
    return $false
  }

  Write-Ok "sshd is running and set to Automatic."
  return $true
}

# ------------------------------------------------------------------------------
# Firewall rule
# ------------------------------------------------------------------------------
function Ensure-Firewall {
  param([int] $LocalPort)

  if ($SkipFirewall) {
    Write-Warn "Skipping firewall rule as requested."
    return $true
  }

  $ruleName = "OpenSSH Server (TCP $LocalPort)"
  Write-Info "Checking Windows Firewall inbound rule for TCP $LocalPort..."

  $exists = Get-NetFirewallRule -ErrorAction SilentlyContinue |
            Where-Object { $_.DisplayName -eq $ruleName -and $_.Direction -eq 'Inbound' }

  if ($exists) {
    $ports = Get-NetFirewallPortFilter -AssociatedNetFirewallRule $exists
    $hasPort = $ports | Where-Object { $_.LocalPort -eq "$LocalPort" -and $_.Protocol -eq 'TCP' }
    if ($hasPort) {
      Write-Ok "Firewall rule already exists: $ruleName"
      return $true
    } else {
      Write-Info "Existing rule name found but port filter differs. Creating a new rule."
    }
  }

  try {
    New-NetFirewallRule -DisplayName $ruleName `
                        -Direction Inbound `
                        -Protocol TCP `
                        -LocalPort $LocalPort `
                        -Action Allow `
                        -Profile Any `
                        -ErrorAction Stop | Out-Null
    Write-Ok "Firewall rule created: $ruleName"
    return $true
  } catch {
    Write-Err "Failed to create firewall rule: $($_.Exception.Message)"
    return $false
  }
}

# ------------------------------------------------------------------------------
# Local TCP test
# ------------------------------------------------------------------------------
function Test-LocalPort {
  param([int] $LocalPort)
  Write-Info "Testing localhost:$LocalPort ..."
  $success = $false

  try {
    $client6 = New-Object System.Net.Sockets.TcpClient
    $ar6 = $client6.BeginConnect("::1", $LocalPort, $null, $null)
    $ok6 = $ar6.AsyncWaitHandle.WaitOne(1500)
    if ($ok6 -and $client6.Connected) { $success = $true }
    $client6.Close()
  } catch {
    Write-Warn "TCP connect to (::1 : $LocalPort) failed"
  }

  if (-not $success) {
    try {
      $client4 = New-Object System.Net.Sockets.TcpClient
      $ar4 = $client4.BeginConnect("127.0.0.1", $LocalPort, $null, $null)
      $ok4 = $ar4.AsyncWaitHandle.WaitOne(1500)
      if ($ok4 -and $client4.Connected) { $success = $true }
      $client4.Close()
    } catch {
      Write-Warn "TCP connect to (127.0.0.1 : $LocalPort) failed"
    }
  }

  if ($success) {
    Write-Ok "TCP $LocalPort reachable locally."
    return $true
  } else {
    Write-Warn "TCP $LocalPort not reachable locally. Check sshd, port usage, or firewall."
    return $false
  }
}

# ------------------------------------------------------------------------------
# Main flow
# ------------------------------------------------------------------------------
try {
  Ensure-OpenSSHServerInstalled
  Ensure-HostKeys
  Repair-SSH-ACLs

  # Normalize config if requested
  if ($NormalizeConfig) {
    Normalize-SSHDConfig -Path $ConfigPath -DesiredPort $Port
  }

  # Validate config
  $cfgOk = Test-SSHDConfig -Path $ConfigPath
  if (-not $cfgOk) {
    Write-Err "Aborting due to sshd_config syntax errors."
    exit 2
  }

  # Determine effective port
  $effectivePort = if ($NormalizeConfig) { $Port } else { Get-EffectivePortFromConfig -Path $ConfigPath -Fallback $Port }
  Write-Info ("Effective SSH port: {0}" -f $effectivePort)

  # Start service
  $svcOk = Configure-And-Start-SSHD
  if (-not $svcOk) {
    Write-Err "Aborting because sshd service failed to start."
    exit 3
  }

  # Firewall
  $fwOk = Ensure-Firewall -LocalPort $effectivePort
  if (-not $fwOk) {
    Write-Err "Firewall configuration failed."
    $fwFail = $true
  } else {
    $fwFail = $false
  }

  # Local reachability test
  $tcpOk = Test-LocalPort -LocalPort $effectivePort
  if (-not $tcpOk) {
    Write-Info "Quick hints:"
    Write-Info "  1) netstat -aon | findstr :$effectivePort"
    Write-Info "  2) Event Viewer -> Applications and Services Logs -> OpenSSH -> Operational"
    Write-Info "  3) Check ListenAddress or AddressFamily in sshd_config"
    Write-Info "  4) Restart-Service sshd"
    if ($fwFail) { exit 4 } else { exit 5 }
  }

  # Display example connection string
  Write-Info "SSH connect example:"
  $hostname = (Get-NetIPAddress -AddressFamily IPv4 -PrefixOrigin Dhcp,Manual -ErrorAction SilentlyContinue |
               Where-Object { $_.IPAddress -notlike '169.254.*' } |
               Select-Object -First 1 -ExpandProperty IPAddress)
  if (-not $hostname) { $hostname = "your_host_ip" }
  $user = $env:USERNAME
  Write-Host ("  ssh {0}@{1} -p {2}" -f $user, $hostname, $effectivePort)

  Write-Ok "Done."
  exit 0

} catch {
  Write-Err ("Unhandled exception: {0}" -f $_.Exception.Message)
  exit 1
} finally {
  if ($transcriptStarted) {
    try { Stop-Transcript | Out-Null } catch {}
  }
}
