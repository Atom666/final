param(
    [Parameter(Mandatory = $true)]
    [string]$NpcapRoot,

    [string]$VcpkgRoot = $env:VCPKG_ROOT,

    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release",

    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $ProjectRoot "build-windows"

if (-not $VcpkgRoot) {
    throw "Set VCPKG_ROOT or pass -VcpkgRoot."
}

$Toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path $Toolchain)) {
    throw "vcpkg toolchain not found: $Toolchain"
}
if (-not (Test-Path (Join-Path $NpcapRoot "Include\pcap.h"))) {
    throw "Npcap SDK header not found below: $NpcapRoot"
}

if ($Clean -and (Test-Path $BuildDir)) {
    Remove-Item -Recurse -Force $BuildDir
}

cmake -S $ProjectRoot -B $BuildDir `
    -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE="$Toolchain" `
    -DNPCAP_ROOT="$NpcapRoot" `
    -DBUILD_TESTING=OFF `
    -DMIRROR_BUILD_RECEIVER=OFF `
    -DMIRROR_BUILD_TEST_TOOLS=OFF
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $BuildDir --config $Configuration
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$OutputDir = Join-Path $BuildDir "bin"
$ConfigPath = Join-Path $OutputDir "mirror-agent.conf"
if (-not (Test-Path $ConfigPath)) {
    Copy-Item (Join-Path $ProjectRoot "examples\agent-windows.conf") $ConfigPath
}

Write-Host ""
Write-Host "Built: $(Join-Path $OutputDir 'mirror-agent.exe')"
Write-Host "List adapters from elevated PowerShell:"
Write-Host "  & '$(Join-Path $OutputDir 'mirror-agent.exe')' --list-interfaces"
