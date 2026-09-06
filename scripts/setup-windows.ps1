# Automates Windows-agent dependency setup and registers mirror-agent.exe
# to run at startup via Task Scheduler. Wraps scripts/build-windows.ps1;
# does not duplicate its CMake logic.
param(
    [string]$VcpkgRoot = "C:\deps\vcpkg",
    [string]$NpcapRoot = "C:\deps\npcap-sdk",
    [string]$NpcapSdkVersion = "1.15",
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release",
    [switch]$SkipNpcapInstaller,
    [switch]$RegisterTask,
    [string]$ConfigPath,
    [string]$TaskName = "MirrorAgent"
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot

# --- 0. elevation -----------------------------------------------------------
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    throw "Run this script from an elevated (Administrator) PowerShell / Developer PowerShell for VS 2022."
}

if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    throw "winget not found. Install 'App Installer' from the Microsoft Store, then re-run."
}

function Install-WingetPackage {
    param([string]$Id, [string[]]$Override)
    $wingetArgs = @("install", "--id", $Id, "-e", "--accept-package-agreements", "--accept-source-agreements", "--silent")
    if ($Override) { $wingetArgs += @("--override", ($Override -join " ")) }
    Write-Host "winget install $Id"
    & winget @wingetArgs
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189) {
        # -1978335189 = APPINSTALLER_CLI_ERROR_PACKAGE_ALREADY_INSTALLED
        throw "winget install failed for $Id (exit $LASTEXITCODE)"
    }
}

# --- 1. build toolchain: Git, CMake, VS Build Tools + C++ workload ----------
Install-WingetPackage -Id "Git.Git"
Install-WingetPackage -Id "Kitware.CMake"
Install-WingetPackage -Id "Microsoft.VisualStudio.2022.BuildTools" -Override @(
    "--passive", "--wait",
    "--add", "Microsoft.VisualStudio.Workload.VCTools",
    "--includeRecommended"
)

# --- 2. vcpkg + OpenSSL ------------------------------------------------------
if (-not (Test-Path (Join-Path $VcpkgRoot "vcpkg.exe"))) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $VcpkgRoot) | Out-Null
    git clone https://github.com/microsoft/vcpkg $VcpkgRoot
    & "$VcpkgRoot\bootstrap-vcpkg.bat"
}
& "$VcpkgRoot\vcpkg.exe" install openssl:x64-windows
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", $VcpkgRoot, "User")
$env:VCPKG_ROOT = $VcpkgRoot

# --- 3. Npcap SDK (headers/import libs — plain zip, no installer) -----------
if (-not (Test-Path (Join-Path $NpcapRoot "Include\pcap.h"))) {
    $sdkZip = Join-Path $env:TEMP "npcap-sdk-$NpcapSdkVersion.zip"
    Write-Host "Downloading Npcap SDK $NpcapSdkVersion"
    Invoke-WebRequest "https://npcap.com/dist/npcap-sdk-$NpcapSdkVersion.zip" -OutFile $sdkZip
    New-Item -ItemType Directory -Force $NpcapRoot | Out-Null
    Expand-Archive -Path $sdkZip -DestinationPath $NpcapRoot -Force
    Remove-Item $sdkZip
}
if (-not (Test-Path (Join-Path $NpcapRoot "Include\pcap.h"))) {
    throw "Npcap SDK extraction did not produce Include\pcap.h under $NpcapRoot"
}

# --- 4. Npcap runtime (driver) ----------------------------------------------
# The free Npcap installer requires interactive EULA acceptance; silent /S
# is OEM-only. We just launch it and wait for you to click through.
$npcapInstalled = Test-Path "$env:SystemRoot\System32\Npcap\wpcap.dll"
if (-not $npcapInstalled -and -not $SkipNpcapInstaller) {
    $npcapExe = Join-Path $env:TEMP "npcap-setup.exe"
    Write-Host "Downloading Npcap runtime installer"
    Invoke-WebRequest "https://npcap.com/dist/npcap-1.80.exe" -OutFile $npcapExe
    Write-Host "Launching Npcap installer - accept the EULA and finish the wizard."
    Start-Process -FilePath $npcapExe -Wait
    Remove-Item $npcapExe -ErrorAction SilentlyContinue
} elseif ($npcapInstalled) {
    Write-Host "Npcap runtime already installed."
}

# --- 5. build via existing script --------------------------------------------
& (Join-Path $PSScriptRoot "build-windows.ps1") -NpcapRoot $NpcapRoot -VcpkgRoot $VcpkgRoot -Configuration $Configuration
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$BinDir = Join-Path $ProjectRoot "build-windows\bin"
$ExePath = Join-Path $BinDir "mirror-agent.exe"
if (-not $ConfigPath) { $ConfigPath = Join-Path $BinDir "mirror-agent.conf" }

# --- 6. register as a Scheduled Task (run-at-startup "agent") ---------------
# mirror-agent.exe is a console app with Ctrl+C handling, not a native
# Windows service (see README "Типовые ошибки Windows") - sc.exe can't host
# it directly. Task Scheduler run-at-startup is the native equivalent.
if ($RegisterTask) {
    Write-Host "Registering scheduled task '$TaskName'"
    $action = New-ScheduledTaskAction -Execute $ExePath -Argument "`"$ConfigPath`"" -WorkingDirectory $BinDir
    $trigger = New-ScheduledTaskTrigger -AtStartup
    $principal = New-ScheduledTaskPrincipal -UserId "SYSTEM" -LogonType ServiceAccount -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) -StartWhenAvailable
    Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null
    Write-Host "Task registered. Start now with: Start-ScheduledTask -TaskName '$TaskName'"
}

Write-Host ""
Write-Host "Done. Before starting the agent, still by hand (machine-specific, see README):"
Write-Host "  1. $ExePath --list-interfaces   (elevated) -> set capture_iface in $ConfigPath"
Write-Host "  2. place ca.crt/client.crt/client.key next to $ExePath, set agent_uuid to match the cert CN"
Write-Host "  3. set receiver_host/receiver_port/tls_server_name in $ConfigPath"
if (-not $RegisterTask) {
    Write-Host "Re-run with -RegisterTask once the config is filled in, to auto-start at boot."
}
