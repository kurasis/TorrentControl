param(
    [string]$Proof = 'build/windows-x64-release/tools/tc-proof/tc-workflow-proof.exe',
    [string]$Gui = 'build/windows-x64-release/src/app/windows/TorrentControl.exe',
    [string]$ToolsRoot = 'build/windows-smb-tools',
    [string]$Report = 'build/windows-smb-evidence/windows-smb-faults.json',
    [switch]$DisposableRunner
)
$ErrorActionPreference = 'Stop'
$testExit = 1
try {
    & "$PSScriptRoot/prepare-smb-vm.ps1" -Root $ToolsRoot -DisposableRunner:$DisposableRunner
    & python "$PSScriptRoot/windows_smb_faults.py" --proof $Proof --gui $Gui --vm-config "$ToolsRoot/vm.json" --report $Report
    $testExit = $LASTEXITCODE
} finally {
    $configFile = Join-Path $ToolsRoot 'vm.json'
    if (Test-Path $configFile) {
        $config = Get-Content -Raw -Encoding utf8 $configFile | ConvertFrom-Json
        $device = @(Get-CimInstance Win32_NetworkAdapter | Where-Object { $_.PNPDeviceID -eq $config.deviceId })
        if ($device.Count -ne 1 -or ([guid]$device[0].GUID) -ne ([guid]$config.adapterGuid)) {
            throw 'The device identity changed; refusing to remove another adapter'
        }
        & $config.devcon remove ('@' + $config.deviceId)
        $removed = $LASTEXITCODE -eq 0
        if ($removed) {
            $deadline = [DateTime]::UtcNow.AddSeconds(30)
            do {
                $remaining = @(Get-NetAdapter -IncludeHidden | Where-Object { ([guid]$_.InterfaceGuid) -eq ([guid]$config.adapterGuid) })
                if ($remaining.Count -eq 0) { break }
                Start-Sleep -Milliseconds 100
            } while ([DateTime]::UtcNow -lt $deadline)
            $removed = $remaining.Count -eq 0
        }
        if (Test-Path $Report) {
            $result = Get-Content -Raw -Encoding utf8 $Report | ConvertFrom-Json
            $result | Add-Member -NotePropertyName adapterRemoved -NotePropertyValue $removed -Force
            $result.fixtureCleaned = $removed -and $result.vmStopped -and $result.connectionRemoved
            if (-not $result.fixtureCleaned) { $result.passed = $false }
            $result | ConvertTo-Json -Depth 20 | Set-Content -Encoding utf8 $Report
        }
        if (-not $removed) { throw 'The task-owned TAP device was not removed' }
    }
}
if ($testExit -ne 0) { throw "Windows SMB fault evidence failed with exit code $testExit" }
