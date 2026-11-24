<#
.SYNOPSIS
    Installs pipa-pc-agent as an automatic background logon service on Windows
    and configures a strict LAN-only Windows Firewall rule for TCP 18765.
#>
[CmdletBinding()]
param(
    [int]$Port = 18765,
    [string]$AllowedSubnet = "192.168.1.0/24",
    [switch]$NoStart
)

$ErrorActionPreference = "Stop"

$InstallDir = Join-Path $env:USERPROFILE ".pipa-pc-agent"
$AgentDst   = Join-Path $InstallDir "pc_agent.py"
$TokenPath  = Join-Path $InstallDir "token.txt"
$SourceFile = Join-Path $PSScriptRoot "pc_agent.py"

if (-not (Test-Path $SourceFile)) {
    throw "pc_agent.py not found next to install_windows_task.ps1 ($SourceFile)"
}

New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
Copy-Item -Path $SourceFile -Destination $AgentDst -Force

# Locate python.exe and pythonw.exe
$PythonExe = (Get-Command py -ErrorAction SilentlyContinue)
if ($PythonExe) {
    $PyPath = (& py -3 -c "import sys; print(sys.executable)").Trim()
} else {
    $PyCmd = Get-Command python -ErrorAction Stop
    $PyPath = $PyCmd.Source
}
$PythonwPath = Join-Path (Split-Path $PyPath -Parent) "pythonw.exe"
if (-not (Test-Path $PythonwPath)) {
    $PythonwPath = $PyPath
}

# Initialize shared X-Pipa-Token if missing
$Token = (& $PyPath $AgentDst --token-file $TokenPath --print-token).Trim()
Write-Host "==> Installed pc_agent.py to $AgentDst"
Write-Host "==> Token file: $TokenPath"

# Add strict LAN-only Windows Firewall rule (requires Administrator; print command if not elevated)
$RuleName = "Pipa-MyPC-Agent-$Port"
$IsAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator
)

if ($IsAdmin) {
    $Existing = Get-NetFirewallRule -DisplayName $RuleName -ErrorAction SilentlyContinue
    if ($Existing) {
        Remove-NetFirewallRule -DisplayName $RuleName
    }
    New-NetFirewallRule `
        -DisplayName $RuleName `
        -Direction Inbound `
        -Action Allow `
        -Protocol TCP `
        -LocalPort $Port `
        -RemoteAddress $AllowedSubnet `
        -Profile Private | Out-Null
    Write-Host "==> Windows Firewall rule '$RuleName' created (TCP $Port strictly from $AllowedSubnet)"
} else {
    Write-Warning "Not running as Administrator — skipped Windows Firewall rule."
    Write-Host "Run this in an elevated PowerShell to allow strictly $AllowedSubnet on TCP ${Port}:"
    Write-Host "  New-NetFirewallRule -DisplayName '$RuleName' -Direction Inbound -Action Allow -Protocol TCP -LocalPort $Port -RemoteAddress '$AllowedSubnet' -Profile Private"
}

# Register Scheduled Task at logon (fallback to Startup VBS launcher if restricted)
$TaskName = "PipaPCAgent"
$RegisteredTask = $false
try {
    $Action   = New-ScheduledTaskAction -Execute $PythonwPath -Argument "`"$AgentDst`" --port $Port" -WorkingDirectory $InstallDir
    $Trigger  = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
    $Settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit (New-TimeSpan -Days 0)
    Register-ScheduledTask -TaskName $TaskName -Action $Action -Trigger $Trigger -Settings $Settings -Force | Out-Null
    $RegisteredTask = $true
    Write-Host "==> Registered Scheduled Task '$TaskName' (runs silently at logon)"
} catch {
    Write-Warning "Could not register Scheduled Task ($($_.Exception.Message)); falling back to Startup folder shortcut."
    $StartupDir = [Environment]::GetFolderPath("Startup")
    $VbsPath = Join-Path $StartupDir "PipaPCAgent.vbs"
    $Vbs = "CreateObject(`"WScript.Shell`").Run `"`"`"$PythonwPath`"`" `"`"$AgentDst`"`" --port $Port`", 0, False"
    Set-Content -Path $VbsPath -Value $Vbs -Encoding ASCII
    Write-Host "==> Created Startup launcher at $VbsPath"
}

if (-not $NoStart) {
    # Stop any previous instance cleanly before starting
    Get-CimInstance Win32_Process -Filter "CommandLine LIKE '%pc_agent.py%'" -ErrorAction SilentlyContinue | ForEach-Object {
        Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
    }
    Start-Process -FilePath $PythonwPath -ArgumentList "`"$AgentDst`" --port $Port" -WorkingDirectory $InstallDir -WindowStyle Hidden
    Start-Sleep -Milliseconds 700
    try {
        $Health = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/health" -Headers @{ "X-Pipa-Token" = $Token } -TimeoutSec 3
        Write-Host "==> Agent is LIVE: $($Health | ConvertTo-Json -Compress)"
    } catch {
        Write-Warning "Agent started, but /health probe failed: $($_.Exception.Message)"
    }
}

Write-Host ""
Write-Host "Copy this line into ~/.config/pipa-bot/bot.env on Xiaomi Pad 6:"
Write-Host "PC_AGENT_TOKEN=$Token"
