param(
    [string]$PackageDirectory = 'build/windows11-input/windows-packages',
    [string]$TestDirectory = 'build/windows11-test-input',
    [string]$EvidenceDirectory = 'build/windows11-evidence'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $IsWindows -or $env:GITHUB_ACTIONS -ne 'true') { throw 'Requires a disposable Windows 11 GitHub runner' }
. "$PSScriptRoot/../../packaging/windows/common.ps1"
$null = New-Item -ItemType Directory -Path $EvidenceDirectory
$evidence = (Resolve-Path $EvidenceDirectory).Path
$packages = (Resolve-Path $PackageDirectory).Path
$inputs = (Resolve-Path $TestDirectory).Path
$os = Get-CimInstance Win32_OperatingSystem
$architecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
$info = Get-ComputerInfo | Select-Object WindowsProductName, OsName, OsArchitecture, OsBuildNumber
$info | Format-List | Out-String | Write-Output
if ($os.ProductType -ne 1 -or [int]$os.BuildNumber -lt 22000 -or $os.Caption -notmatch 'Windows 11' -or $architecture -ne 'Arm64') {
    throw 'Expected actual Windows 11 ARM64 desktop, not Windows Server or an x64 runner'
}
$revision = (& git rev-parse HEAD).Trim()
$manifest = Get-Content "$inputs/input-manifest.json" -Raw | ConvertFrom-Json
$package = Get-Content "$packages/stage/package-manifest.json" -Raw | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or $manifest.sourceCommit -ne $revision -or $package.sourceCommit -ne $revision -or $manifest.architecture -ne 'x64') {
    throw 'Compatibility inputs/packages do not match this checkout'
}
foreach ($file in $manifest.files) { Assert-TcHash (Join-Path $inputs $file.path) $file.sha256 }
$products = @(Get-Content "$packages/checksums.json" -Raw | ConvertFrom-Json)
if ($products.Count -ne 3) { throw 'Expected both product installers and the application ZIP' }
foreach ($file in $products) { Assert-TcHash (Join-Path $packages $file.file) $file.sha256 }
$result = @{ passed = $false; sourceCommit = $revision; caption = $os.Caption; buildNumber = $os.BuildNumber;
    windowsProductName = $info.WindowsProductName; osArchitecture = $architecture; appArchitecture = 'x64';
    execution = 'Windows 11 ARM64 x64 emulation'; scope = 'hosted image with existing Runtime/tools; no disconnected clean machine certification'; cases = @() }
try {
    # Catch2's normal all-tests invocation covers every case, including expected
    # platform skips. Preserve and inspect the JUnit report, not just exit zero.
    $junit = Join-Path $evidence 'unit-tests.xml'
    $exit = Invoke-TcProcess (Join-Path $inputs 'tc_unit_tests.exe') @('--reporter','junit','--out',$junit) 600
    [xml]$report = Get-Content $junit -Raw
    $cases = $report.SelectNodes('//testcase').Count
    $skipped = $report.SelectNodes('//testcase/skipped').Count
    if ($exit -notin @(0,4) -or $cases -le 0 -or $report.SelectNodes('//testcase/failure | //testcase/error').Count -ne 0 -or
        ($exit -eq 4 -and $skipped -eq 0)) { throw "Windows 11 unit suite failed: $exit" }
    $result.cases += @{ case = 'all-x64-unit-tests'; passed = $true; tests = $cases; skipped = $skipped }
    $env:TC_PROOF = Join-Path $inputs 'tc-proof.exe'
    $env:TC_NETWORK_PROOF = Join-Path $inputs 'tc-network-proof.exe'
    $env:TC_VERIFIER = (Resolve-Path 'tools/reference/verify_torrent.py').Path
    & python -m unittest discover -s tests/integration -p 'test_proof.py' -v *> "$evidence/headless-integration.log"
    if ($LASTEXITCODE -ne 0) { throw 'Windows 11 headless/reference integration failed' }
    $result.cases += @{ case = 'headless-independent-reference-integration'; passed = $true }
    & python -m unittest discover -s tests/integration -p 'test_network.py' -v *> "$evidence/network-integration.log"
    if ($LASTEXITCODE -ne 0) { throw 'Windows 11 network integration failed' }
    $result.cases += @{ case = 'network-integration'; passed = $true }
    & "$PSScriptRoot/run-packaging.ps1" -PackageDirectory $packages -EvidenceDirectory "$evidence/packaging" -FixtureDirectory "$inputs/fixtures" -DisposableRunner
    $result.cases += @{ case = 'installed-app-repair-runtime-controls-and-uninstall'; passed = $true }
    $app = Join-Path $evidence 'application'
    $zip = @($products | Where-Object { $_.file.EndsWith('.zip') })
    if ($zip.Count -ne 1) { throw 'Expected one application archive' }
    Expand-Archive -LiteralPath (Join-Path $packages $zip[0].file) -DestinationPath $app
    foreach ($file in $package.files) { Assert-TcHash (Join-Path $app $file.path) $file.sha256 }
    & "$PSScriptRoot/run-native-flow.ps1" -Executable "$app/TorrentControl.exe" -EvidenceDirectory "$evidence/native"
    $result.cases += @{ case = 'real-dialogs-jobs-editor-renderer-restart-and-runtime-refusal'; passed = $true }
    $result.passed = $true
} finally {
    $result | ConvertTo-Json -Depth 10 | Set-Content -Encoding utf8 "$evidence/summary.json"
}
Write-Output 'Windows 11 ARM64: actual x64 unit/integration, installed-app and WebView2 workflow checks passed.'
