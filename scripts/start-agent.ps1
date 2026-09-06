<#
Launcher run by the TraffMirrorAgent scheduled task on every boot. Cloned VMs
get a fresh NPF adapter GUID each time (sysprep/cloud re-detects the vNIC as
new hardware), so capture_iface baked into the image at build time goes stale
after cloning. This re-resolves it from --list-interfaces before each start.

Also syncs per-instance identity (ca.crt/client.crt/client.key/mirror-agent.uuid)
from $CertsPath into $OutDir on every boot, so provisioning a clone is just
"drop these 4 files in CertsPath" without re-running deploy-windows.ps1.
#>
param([string]$OutDir = $PSScriptRoot, [string]$CertsPath = "C:\deps\certs")

$exe = Join-Path $OutDir "mirror-agent.exe"
$conf = Join-Path $OutDir "mirror-agent.conf"

if (Test-Path $CertsPath) {
    foreach ($name in "ca.crt", "client.crt", "client.key", "mirror-agent.uuid") {
        $src = Join-Path $CertsPath $name
        if (Test-Path $src) { Copy-Item $src $OutDir -Force }
    }
    $keyPath = Join-Path $OutDir "client.key"
    if (Test-Path $keyPath) {
        icacls $keyPath /inheritance:r | Out-Null
        icacls $keyPath /grant:r "SYSTEM:F" "*S-1-5-32-544:F" | Out-Null
    }
}

# ponytail: exact-name allowlist for known pseudo-adapters; if a real second
# NIC shows up, add its description here or the ambiguous case below fires.
$pseudoAdapterPattern = "WAN Miniport|Loopback adapter for loopback traffic capture"

$listing = & $exe --list-interfaces
$candidates = $listing | Where-Object { $_ -and ($_ -notmatch $pseudoAdapterPattern) }

if ($candidates.Count -eq 1) {
    $device = ($candidates[0] -split "`t")[0]
    (Get-Content $conf) -replace '^\s*capture_iface\s*=.*', "capture_iface = $device" |
        Set-Content $conf
} elseif ($candidates.Count -eq 0) {
    Write-Warning "No non-pseudo network adapter found; keeping existing capture_iface in $conf."
} else {
    $candidateList = $candidates -join "`n"
    Write-Warning "Multiple candidate adapters found; keeping existing capture_iface in $conf. Candidates:`n$candidateList"
}

& $exe "mirror-agent.conf"
