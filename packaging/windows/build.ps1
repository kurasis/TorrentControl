param(
    [string]$BuildDirectory = 'build/windows-x64-release',
    [string]$OutputDirectory = 'build/windows-packages'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $IsWindows) { throw 'Windows packaging requires Windows/PowerShell 7 and the pinned MSVC environment' }
. "$PSScriptRoot/common.ps1"
$repo = (Resolve-Path "$PSScriptRoot/../..").Path
$build = (Resolve-Path $BuildDirectory).Path
if (Test-Path $OutputDirectory) { throw 'Choose a fresh package output directory' }
$null = New-Item -ItemType Directory -Path $OutputDirectory
$out = (Resolve-Path $OutputDirectory).Path
$stage = Join-Path $out 'stage'
$cache = Join-Path $out 'prerequisites'
$null = New-Item -ItemType Directory -Path $cache
$pins = Get-Content "$PSScriptRoot/prerequisites.json" -Raw | ConvertFrom-Json
$version = [regex]::Match((Get-Content "$repo/CMakeLists.txt" -Raw), '\bVERSION\s+(\d+\.\d+\.\d+)').Groups[1].Value
$minimum = [regex]::Match((Get-Content "$repo/src/app/runtime_policy.hpp" -Raw), 'minimum_webview_runtime = "([0-9.]+)"').Groups[1].Value
$mutex = [regex]::Match((Get-Content "$repo/src/app/install_identity.hpp" -Raw), 'install_mutex\[\] = L"([^"]+)"').Groups[1].Value.Replace('\\', '\')
if (-not $version -or -not $minimum -or -not $mutex) { throw 'Missing version, Runtime floor or installation identity' }
& cmake --install $build --prefix $stage --component WindowsApp
if ($LASTEXITCODE -ne 0) { throw 'Application staging failed' }

# Only redistributable release DLLs from the pinned MSVC x64 CRT directory.
$crt = Join-Path $env:VCToolsRedistDir 'x64/Microsoft.VC143.CRT'
foreach ($required in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
    if (-not (Test-Path (Join-Path $crt $required))) { throw "Missing MSVC redistributable: $required" }
}
$crtEvidence = @()
foreach ($dll in Get-ChildItem $crt -Filter '*.dll') {
    $signer = Assert-TcMicrosoftSignature $dll.FullName
    Copy-Item -LiteralPath $dll.FullName -Destination $stage
    $crtEvidence += @{ file = $dll.Name; version = $dll.VersionInfo.FileVersion; signer = $signer }
}
$licenses = Join-Path $stage 'licenses'
$null = New-Item -ItemType Directory -Path $licenses
$share = Join-Path $build 'vcpkg_installed/x64-windows-static-md/share'
foreach ($copyright in Get-ChildItem $share -Filter copyright -Recurse -File) {
    $port = $copyright.Directory.Name
    $null = New-Item -ItemType Directory -Force -Path (Join-Path $licenses $port)
    Copy-Item -LiteralPath $copyright.FullName -Destination (Join-Path $licenses "$port/copyright.txt")
}
if (@(Get-ChildItem $licenses -Recurse -File).Count -eq 0) { throw 'Dependency license texts are missing' }
Copy-Item "$repo/THIRD_PARTY_NOTICES.md" $stage
Copy-Item "$PSScriptRoot/DEVELOPMENT_PACKAGE.txt" $stage

$vendor = @{}
foreach ($mode in @('online', 'offline')) {
    $pin = if ($mode -eq 'online') { $pins.webviewOnline } else { $pins.webviewOffline }
    $path = Get-TcPinnedFile $pin (Join-Path $cache "$mode.exe")
    $signer = Assert-TcMicrosoftSignature $path
    $vendor[$mode] = @{ path = $path; sha256 = $pin.sha256; url = $pin.url; signer = $signer; sizeBytes = (Get-Item $path).Length }
}
$inno = Get-TcPinnedFile $pins.innoSetup (Join-Path $cache 'inno-setup.exe')
$compilerFolder = Join-Path $cache 'inno'
$exit = Invoke-TcProcess $inno @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/CURRENTUSER', "/DIR=$compilerFolder")
if ($exit -ne 0) { throw "Pinned Inno Setup installation failed: $exit" }
$compiler = Join-Path $compilerFolder 'ISCC.exe'
$inventory = @(Get-ChildItem $stage -Recurse -File | ForEach-Object {
    @{ path = [IO.Path]::GetRelativePath($stage, $_.FullName).Replace('\','/'); sha256 = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(); sizeBytes = $_.Length }
})
$revision = (& git -C $repo rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Source provenance is unavailable' }
$vendorMetadata = @{}
foreach ($mode in @('online', 'offline')) {
    $vendorMetadata[$mode] = @{ sha256 = $vendor[$mode].sha256; url = $vendor[$mode].url; signer = $vendor[$mode].signer; sizeBytes = $vendor[$mode].sizeBytes }
}
$manifest = @{ version = $version; sourceCommit = $revision; unsigned = $true; architecture = 'x64'; runtimeMinimum = $minimum;
    installMutex = $mutex; appId = '{75613C0D-60D8-4B53-B7F7-01DE3D5E37FE}'; scope = 'development packaging; no clean-desktop OS certification';
    crt = $crtEvidence; prerequisites = $vendorMetadata; files = $inventory; applicationLicense = 'pending owner selection' }
$manifest | ConvertTo-Json -Depth 12 | Set-Content -Encoding utf8 (Join-Path $stage 'package-manifest.json')
# The manifest describes staged files except itself, avoiding a recursive hash.
foreach ($mode in @('online', 'offline')) {
    & $compiler "/DStageRoot=$stage" "/DOutputRoot=$out" "/DAppVersion=$version" "/DInstallMutex=$mutex" "/DRuntimeMinimum=$minimum" "/DPackageMode=$mode" "/DRuntimeSource=$($vendor[$mode].path)" "/DRuntimeSHA256=$($vendor[$mode].sha256)" "$PSScriptRoot/TorrentControl.iss"
    if ($LASTEXITCODE -ne 0) { throw "$mode final installer compilation failed" }
}
Compress-Archive -Path "$stage/*" -DestinationPath (Join-Path $out "TorrentControl-$version-dev-unsigned-windows-x64.zip")
$products = @(Get-ChildItem $out -File | Where-Object Extension -In '.exe','.zip' | ForEach-Object {
    @{ file = $_.Name; sha256 = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(); sizeBytes = $_.Length }
})
$products | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $out 'checksums.json')
Write-Output "Packaged $($products.Count) unsigned development artifacts in $out"
