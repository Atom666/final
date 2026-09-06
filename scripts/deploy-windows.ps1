#Requires -RunAsAdministrator
<#
Offline provisioning of the Windows mirror-agent: installs build deps from a
local staging folder, builds mirror-agent.exe, drops config/certs, and wires
the agent to run at every boot via Scheduled Task. Safe to re-run (skips
steps whose result already exists) so it can also be scheduled to run at
startup itself (-InstallStartupTask).

Staging layout expected under -InstallersDir (default C:\deps\installers):
  cmake-*-windows-x86_64.zip     https://cmake.org/download/
  npcap-*.exe                    https://npcap.com/#download  (runtime installer)
  npcap-sdk-*.zip                https://npcap.com/#download  (SDK)
  Win64OpenSSL-*.exe             https://slproweb.com/products/Win32OpenSSL.html
  vs-buildtools-layout\          offline layout dir, see README printed by -DryRun
#>
param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$DepsRoot = "C:\deps",
    [string]$InstallersDir = (Join-Path $DepsRoot "installers"),
    [string]$NpcapSdkDir = (Join-Path $DepsRoot "npcap-sdk"),
    [string]$CMakeDir = (Join-Path $DepsRoot "cmake"),
    [string]$OpenSslDir = "C:\Program Files\OpenSSL-Win64",

    [string]$CertsPath = (Join-Path $DepsRoot "certs"),
    [string]$AgentUuid,
    [string]$ReceiverHost,
    [int]$ReceiverPort,
    [string]$CaptureIface,
    [string]$TlsServerName,

    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release",

    [switch]$Clean,
    [switch]$InstallStartupTask,
    [switch]$StartAgentNow,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$BuildDir = Join-Path $ProjectRoot "build-windows"
$OutDir = Join-Path $BuildDir "bin"
$VsBuildToolsLayout = Join-Path $InstallersDir "vs-buildtools-layout"

function Find-Installer([string]$Pattern) {
    $f = Get-ChildItem -Path $InstallersDir -Filter $Pattern -File -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -First 1
    if (-not $f) { throw "Не найден файл '$Pattern' в $InstallersDir. Скачайте его и положите туда." }
    return $f.FullName
}

function Add-ToMachinePath([string]$Dir) {
    $current = [Environment]::GetEnvironmentVariable("Path", "Machine")
    if ($current -notlike "*$Dir*") {
        [Environment]::SetEnvironmentVariable("Path", "$current;$Dir", "Machine")
    }
    if ($env:Path -notlike "*$Dir*") { $env:Path = "$env:Path;$Dir" }
}

function Test-VsBuildToolsInstalled {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $false }
    $found = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    return [bool]$found
}

function Ensure-CMake {
    $cmakeExe = Join-Path $CMakeDir "bin\cmake.exe"
    if (Test-Path $cmakeExe) { Add-ToMachinePath (Split-Path $cmakeExe); return }
    Write-Host "[cmake] extracting portable zip..."
    $zip = Find-Installer "cmake-*-windows-x86_64.zip"
    Expand-Archive -Path $zip -DestinationPath $DepsRoot -Force
    $extracted = Get-ChildItem $DepsRoot -Directory -Filter "cmake-*-windows-x86_64" | Select-Object -First 1
    if (-not $extracted) { throw "Архив cmake распаковался неожиданно, папка cmake-*-windows-x86_64 не найдена." }
    if (Test-Path $CMakeDir) { Remove-Item $CMakeDir -Recurse -Force }
    Rename-Item $extracted.FullName $CMakeDir
    Add-ToMachinePath (Join-Path $CMakeDir "bin")
}

function Ensure-VsBuildTools {
    if (Test-VsBuildToolsInstalled) { return }
    Write-Host "[vs-buildtools] installing from offline layout..."
    $installer = Join-Path $VsBuildToolsLayout "vs_buildtools.exe"
    if (-not (Test-Path $installer)) {
        throw "Не найден $installer. Соберите офлайн-layout на машине с интернетом:`n" +
            "  vs_buildtools.exe --layout `"$VsBuildToolsLayout`" --lang en-US ``\n" +
            "    --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended`n" +
            "и скопируйте всю папку на узел."
    }
    $p = Start-Process -FilePath $installer -ArgumentList @(
        "--noWeb", "--quiet", "--wait", "--norestart",
        "--add", "Microsoft.VisualStudio.Workload.VCTools", "--includeRecommended"
    ) -Wait -PassThru
    # 3010 = success, reboot required. Both are acceptable outcomes here.
    if ($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010) {
        throw "vs_buildtools.exe завершился с кодом $($p.ExitCode)"
    }
    if ($p.ExitCode -eq 3010) { Write-Warning "VS Build Tools просит перезагрузку." }
}

function Ensure-Npcap {
    if (Get-Service -Name npcap -ErrorAction SilentlyContinue) { return }
    Write-Host "[npcap] installing runtime..."
    $exe = Find-Installer "npcap-*.exe"
    $p = Start-Process -FilePath $exe -ArgumentList "/S" -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "Npcap runtime установился с кодом $($p.ExitCode)" }
}

function Ensure-NpcapSdk {
    if (Test-Path (Join-Path $NpcapSdkDir "Include\pcap.h")) { return }
    Write-Host "[npcap-sdk] extracting..."
    $zip = Find-Installer "npcap-sdk-*.zip"
    if (Test-Path $NpcapSdkDir) { Remove-Item $NpcapSdkDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $NpcapSdkDir | Out-Null
    Expand-Archive -Path $zip -DestinationPath $NpcapSdkDir -Force
    if (-not (Test-Path (Join-Path $NpcapSdkDir "Include\pcap.h"))) {
        throw "После распаковки $zip не найден Include\pcap.h в $NpcapSdkDir"
    }
}

function Find-OpenSslDll([string]$Prefix) {
    return Get-ChildItem -Path (Join-Path $OpenSslDir "bin") -Filter "$Prefix-*-x64.dll" -ErrorAction SilentlyContinue |
        Select-Object -First 1
}

function Ensure-OpenSSL {
    if (Find-OpenSslDll "libssl") { return }
    Write-Host "[openssl] installing..."
    $exe = Find-Installer "Win64OpenSSL-*.exe"
    $p = Start-Process -FilePath $exe -ArgumentList @(
        "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/DIR=`"$OpenSslDir`""
    ) -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "OpenSSL установился с кодом $($p.ExitCode)" }
    if (-not (Find-OpenSslDll "libssl")) { throw "OpenSSL установился, но libssl-*-x64.dll не найден в $OpenSslDir\bin" }
}

function Build-Agent {
    $exePath = Join-Path $OutDir "mirror-agent.exe"
    if ($Clean -and (Test-Path $BuildDir)) { Remove-Item -Recurse -Force $BuildDir }
    if (-not $Clean -and (Test-Path $exePath)) { Write-Host "[build] уже собрано, пропуск."; return }

    Write-Host "[build] configuring..."
    & cmake -S $ProjectRoot -B $BuildDir -G "Visual Studio 17 2022" -A x64 `
        -DNPCAP_ROOT="$NpcapSdkDir" -DOPENSSL_ROOT_DIR="$OpenSslDir" `
        -DBUILD_TESTING=OFF -DMIRROR_BUILD_RECEIVER=OFF -DMIRROR_BUILD_TEST_TOOLS=OFF
    if ($LASTEXITCODE -ne 0) { throw "cmake configure завершился с кодом $LASTEXITCODE" }

    Write-Host "[build] building ($Configuration)..."
    & cmake --build $BuildDir --config $Configuration
    if ($LASTEXITCODE -ne 0) { throw "cmake build завершился с кодом $LASTEXITCODE" }

    $sslDll = Find-OpenSslDll "libssl"
    $cryptoDll = Find-OpenSslDll "libcrypto"
    if (-not $sslDll -or -not $cryptoDll) { throw "libssl-*-x64.dll/libcrypto-*-x64.dll не найдены в $OpenSslDir\bin" }
    Copy-Item $sslDll.FullName $OutDir -Force
    Copy-Item $cryptoDll.FullName $OutDir -Force
}

function Configure-Agent {
    $configPath = Join-Path $OutDir "mirror-agent.conf"
    if (-not (Test-Path $configPath)) {
        Copy-Item (Join-Path $ProjectRoot "examples\agent-windows.conf") $configPath
    }
    Copy-Item (Join-Path $PSScriptRoot "start-agent.ps1") $OutDir -Force

    if ($CertsPath) {
        foreach ($name in "ca.crt", "client.crt", "client.key") {
            $src = Join-Path $CertsPath $name
            if (Test-Path $src) { Copy-Item $src $OutDir -Force }
            else { Write-Warning "Не найден $src, пропущено." }
        }
        $keyPath = Join-Path $OutDir "client.key"
        if (Test-Path $keyPath) {
            icacls $keyPath /inheritance:r | Out-Null
            icacls $keyPath /grant:r "SYSTEM:F" "*S-1-5-32-544:F" | Out-Null
        }
    }

    $replacements = @{}
    if ($AgentUuid) { $replacements["agent_uuid"] = $AgentUuid }
    if ($ReceiverHost) { $replacements["receiver_host"] = $ReceiverHost }
    if ($ReceiverPort) { $replacements["receiver_port"] = $ReceiverPort }
    if ($CaptureIface) { $replacements["capture_iface"] = $CaptureIface }
    if ($TlsServerName) { $replacements["tls_server_name"] = $TlsServerName }
    if ($replacements.Count -eq 0) { return }

    $lines = Get-Content $configPath | ForEach-Object {
        $line = $_
        foreach ($key in $replacements.Keys) {
            if ($line -match "^\s*$key\s*=") { $line = "$key = $($replacements[$key])" }
        }
        $line
    }
    Set-Content -Path $configPath -Value $lines
}

function Register-AgentTask {
    $taskName = "TraffMirrorAgent"
    $action = New-ScheduledTaskAction -Execute "powershell.exe" `
        -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $OutDir 'start-agent.ps1')`" -CertsPath `"$CertsPath`"" `
        -WorkingDirectory $OutDir
    $trigger = New-ScheduledTaskTrigger -AtStartup
    $principal = New-ScheduledTaskPrincipal -UserId "SYSTEM" -LogonType ServiceAccount -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
        -StartWhenAvailable -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) `
        -ExecutionTimeLimit ([TimeSpan]::Zero)
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
    Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger `
        -Principal $principal -Settings $settings | Out-Null
    Write-Host "[task] $taskName зарегистрирован (автозапуск агента при старте)."
    if ($StartAgentNow) { Start-ScheduledTask -TaskName $taskName }
}

function Register-ProvisionTask {
    $taskName = "TraffMirrorProvision"
    $action = New-ScheduledTaskAction -Execute "powershell.exe" `
        -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    $trigger = New-ScheduledTaskTrigger -AtStartup
    $principal = New-ScheduledTaskPrincipal -UserId "SYSTEM" -LogonType ServiceAccount -RunLevel Highest
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
    Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Principal $principal | Out-Null
    Write-Host "[task] $taskName зарегистрирован (сам себя перезапускает при каждом старте, идемпотентно)."
}

if ($DryRun) {
    Write-Host "=== Проверка стейджинга (ничего не устанавливается) ==="
    $checks = @(
        @{ Name = "CMake";        Ok = (Test-Path (Join-Path $CMakeDir "bin\cmake.exe")) -or (Get-ChildItem $InstallersDir -Filter "cmake-*-windows-x86_64.zip" -EA SilentlyContinue) }
        @{ Name = "VS BuildTools"; Ok = (Test-VsBuildToolsInstalled) -or (Test-Path (Join-Path $VsBuildToolsLayout "vs_buildtools.exe")) }
        @{ Name = "Npcap runtime"; Ok = (Get-Service npcap -EA SilentlyContinue) -or (Get-ChildItem $InstallersDir -Filter "npcap-*.exe" -EA SilentlyContinue) }
        @{ Name = "Npcap SDK";     Ok = (Test-Path (Join-Path $NpcapSdkDir "Include\pcap.h")) -or (Get-ChildItem $InstallersDir -Filter "npcap-sdk-*.zip" -EA SilentlyContinue) }
        @{ Name = "OpenSSL";       Ok = (Find-OpenSslDll "libssl") -or (Get-ChildItem $InstallersDir -Filter "Win64OpenSSL-*.exe" -EA SilentlyContinue) }
    )
    foreach ($c in $checks) {
        Write-Host ("  [{0}] {1}" -f $(if ($c.Ok) { "OK" } else { "MISSING" }), $c.Name)
    }
    return
}

New-Item -ItemType Directory -Force -Path (Join-Path $DepsRoot "logs") | Out-Null
$logFile = Join-Path $DepsRoot "logs\deploy-$(Get-Date -Format yyyyMMdd-HHmmss).log"
Start-Transcript -Path $logFile | Out-Null
try {
    Ensure-CMake
    Ensure-VsBuildTools
    Ensure-Npcap
    Ensure-NpcapSdk
    Ensure-OpenSSL
    Build-Agent
    Configure-Agent
    Register-AgentTask
    if ($InstallStartupTask) { Register-ProvisionTask }

    Write-Host ""
    Write-Host "Готово: $(Join-Path $OutDir 'mirror-agent.exe')"
    if (-not $CaptureIface) {
        Write-Host "Осталось вручную: '$OutDir\mirror-agent.exe' --list-interfaces (админ), вписать capture_iface в mirror-agent.conf."
    }
    if (-not $CertsPath) {
        Write-Host "Осталось вручную: положить ca.crt/client.crt/client.key в $OutDir."
    }
}
finally {
    Stop-Transcript | Out-Null
}
