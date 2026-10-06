param(
    [Parameter(Mandatory=$true)][string]$Root,
    [switch]$DisposableRunner
)
# Creates one task-owned TAP device. Run only on a disposable Windows CI/lab
# machine; no physical interface, SMB policy or existing share is changed.
$ErrorActionPreference = 'Stop'
if (-not $DisposableRunner) { throw 'The SMB VM fixture requires -DisposableRunner' }
New-Item -ItemType Directory -Force -Path $Root | Out-Null
$rootPath = (Resolve-Path $Root).Path
$tools = Join-Path $rootPath 'tools'
New-Item -ItemType Directory -Force -Path $tools | Out-Null

function Get-VerifiedFile([string]$url, [string]$destination, [string]$algorithm, [string]$expected) {
    Invoke-WebRequest -Uri $url -OutFile $destination
    if ((Get-FileHash $destination -Algorithm $algorithm).Hash.ToLowerInvariant() -ne $expected) {
        throw "Checksum mismatch for $url"
    }
}
function Invoke-Checked([string]$exe, [string[]]$arguments) {
    & $exe @arguments | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "$exe failed with exit code $LASTEXITCODE" }
}

# Checksums are from the pinned upstream release downloads, not a CI-generated
# checksum of whatever arrived. TLS and Windows driver signature checks stay on.
$qemuArchive = Join-Path $tools 'qemu-20260811.exe'
Get-VerifiedFile 'https://qemu.weilnetz.de/w64/qemu-w64-setup-20260811.exe' $qemuArchive 'SHA512' '5bcf9eed634e8575a37b74f445af41a2fe4106da512d0c30c368301d4c105037fdfab40a5287367a28a957624cddebbc8c07e16c88ab6634f554cdf3d16bf543'
$qemu = Join-Path $tools 'qemu'
Invoke-Checked '7z' @('x', '-y', "-o$qemu", $qemuArchive, 'qemu-system-x86_64.exe', '*.dll', 'share/*')

$iso = Join-Path $tools 'alpine-virt-3.22.6-x86_64.iso'
Get-VerifiedFile 'https://dl-cdn.alpinelinux.org/alpine/v3.22/releases/x86_64/alpine-virt-3.22.6-x86_64.iso' $iso 'SHA256' 'f1e3bbfd700709a8a931748152cec16c539c45b8fa0a04e95a3d38cf2231da98'
$boot = Join-Path $tools 'boot'
Invoke-Checked '7z' @('e', '-y', "-o$boot", $iso, 'boot/vmlinuz-virt', 'boot/initramfs-virt')

$tapArchive = Join-Path $tools 'tap-windows6-9.27.0.zip'
Get-VerifiedFile 'https://github.com/OpenVPN/tap-windows6/releases/download/9.27.0/dist.win10.zip' $tapArchive 'SHA256' '36e2609b7ceefedcb978ce5c48caf9e0e5af83423717c4e2e3c1d7ebca8f62a5'
Expand-Archive -Path $tapArchive -DestinationPath (Join-Path $tools 'tap')
$driver = Join-Path $tools 'tap/dist.win10/amd64'
foreach ($file in @('devcon.exe', 'tap0901.cat')) {
    $signature = Get-AuthenticodeSignature (Join-Path $driver $file)
    if ($signature.Status -ne 'Valid') { throw "Invalid signature on $file : $($signature.Status)" }
    if ($signature.SignerCertificate.Subject -notmatch 'Microsoft') { throw "Unexpected driver signer on $file" }
}
$devcon = Join-Path $driver 'devcon.exe'
$before = @(Get-NetAdapter -IncludeHidden | ForEach-Object { $_.InterfaceGuid.ToString() })
$device = $null
try {
    Invoke-Checked $devcon @('install', (Join-Path $driver 'OemVista.inf'), 'tap0901')
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        $created = @(Get-NetAdapter -IncludeHidden | Where-Object {
            $_.InterfaceGuid.ToString() -notin $before -and $_.InterfaceDescription -like 'TAP-Windows Adapter*'
        })
        if ($created.Count -eq 1) { $device = $created[0]; break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if (-not $device) { throw 'Exactly one new TAP device was not discovered' }
    $name = 'tc-smb-' + [guid]::NewGuid().ToString('N').Substring(0,8)
    Rename-NetAdapter -InputObject $device -NewName $name
    # This disposable address belongs only to the active network stack. The
    # default New-NetIPAddress persistent store can still have DHCP enabled
    # while the active store has already disabled it on a new TAP device.
    Set-NetIPInterface -InterfaceIndex $device.InterfaceIndex -AddressFamily IPv4 -Dhcp Disabled -PolicyStore ActiveStore
    New-NetIPAddress -InterfaceIndex $device.InterfaceIndex -IPAddress '192.168.240.1' -PrefixLength 30 -PolicyStore ActiveStore | Out-Null
    $pnp = Get-CimInstance Win32_NetworkAdapter | Where-Object { $_.GUID -eq ([guid]$device.InterfaceGuid).ToString('B') }
    if (-not $pnp -or -not $pnp.PNPDeviceID) { throw 'The owned TAP device has no PnP identity' }
    $config = @{
        qemu = Join-Path $qemu 'qemu-system-x86_64.exe'; iso = $iso
        kernel = Join-Path $boot 'vmlinuz-virt'; initrd = Join-Path $boot 'initramfs-virt'
        adapterName = $name; adapterGuid = $device.InterfaceGuid.ToString()
        deviceId = $pnp.PNPDeviceID; devcon = $devcon
        hostIp = '192.168.240.1'; serverIp = '192.168.240.2'
        provenance = @{
            alpine = '3.22.6'; alpineSha256 = (Get-FileHash $iso).Hash.ToLowerInvariant()
            qemuSha512 = (Get-FileHash $qemuArchive -Algorithm SHA512).Hash.ToLowerInvariant()
            tap = '9.27.0'; tapSha256 = (Get-FileHash $tapArchive).Hash.ToLowerInvariant()
            driverSignaturesValid = $true
        }
    }
    $config | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 (Join-Path $rootPath 'vm.json')
} catch {
    # Remove only the new device identified by this invocation. Never glob
    # existing adapters or remove a driver package another device could use.
    if ($device) {
        $pnp = Get-CimInstance Win32_NetworkAdapter | Where-Object { $_.GUID -eq ([guid]$device.InterfaceGuid).ToString('B') }
        if ($pnp) { & $devcon remove ('@' + $pnp.PNPDeviceID) | Out-Null }
    }
    throw
}
