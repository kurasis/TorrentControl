param([string]$OutputDirectory = 'build/windows-compatibility-inputs')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../../packaging/windows/common.ps1"
if (Test-Path $OutputDirectory) { throw 'Choose a fresh compatibility input directory' }
$null = New-Item -ItemType Directory -Path "$OutputDirectory/fixtures"
$out = (Resolve-Path $OutputDirectory).Path
$package = Get-Content 'build/windows-packages/stage/package-manifest.json' -Raw | ConvertFrom-Json
Copy-Item 'build/windows-x64-release/tests/tc_unit_tests.exe' $out
foreach ($name in @('tc-proof', 'tc-network-proof', 'tc-native-proof', 'tc-workflow-proof')) {
    Copy-Item "build/windows-x64-release/tools/tc-proof/$name.exe" $out
}
foreach ($crt in $package.crt) { Copy-Item "build/windows-packages/stage/$($crt.file)" $out }
$fixtures = @(Get-ChildItem 'build/windows-package-evidence/negative-fixtures' -Filter '*-setup.exe')
if ($fixtures.Count -ne 3) { throw 'Expected all three verified prerequisite refusal fixtures' }
foreach ($fixture in $fixtures) { Copy-Item $fixture.FullName "$out/fixtures" }
$inventory = @(Get-ChildItem $out -Recurse -File | ForEach-Object {
    @{ path = [IO.Path]::GetRelativePath($out, $_.FullName).Replace('\','/');
       sha256 = (Get-FileHash $_.FullName).Hash.ToLowerInvariant(); sizeBytes = $_.Length }
})
@{ sourceCommit = $package.sourceCommit; architecture = 'x64'; scope = 'developer test inputs; never a product package'; files = $inventory } |
    ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 "$out/input-manifest.json"
