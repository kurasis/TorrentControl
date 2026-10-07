param(
    [string]$PackageDirectory = 'build/windows-packages',
    [string]$EvidenceDirectory = 'build/windows-package-evidence',
    [string]$FixtureDirectory = '',
    [switch]$DisposableRunner
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $IsWindows -or -not $DisposableRunner -or $env:GITHUB_ACTIONS -ne 'true') {
    throw 'This default-path install/uninstall proof requires an explicitly disposable GitHub Windows runner'
}
. "$PSScriptRoot/../../packaging/windows/common.ps1"
$repo = (Resolve-Path "$PSScriptRoot/../..").Path
$packages = (Resolve-Path $PackageDirectory).Path
$manifest = Get-Content "$packages/stage/package-manifest.json" -Raw | ConvertFrom-Json
$armHost = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'Arm64'
if ($FixtureDirectory) { $FixtureDirectory = (Resolve-Path $FixtureDirectory).Path }
$null = New-Item -ItemType Directory -Path $EvidenceDirectory
$evidence = (Resolve-Path $EvidenceDirectory).Path
$install = Join-Path $env:LOCALAPPDATA 'Programs/TorrentControl'
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\' + $manifest.appId + '_is1'
$machineKey = 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\' + $manifest.appId + '_is1'
if ((Test-Path $install) -or (Test-Path $uninstallKey) -or (Test-Path $machineKey)) { throw 'Refusing to modify an existing installation' }
$data = Join-Path $env:LOCALAPPDATA 'TorrentControl'
$null = New-Item -ItemType Directory -Force -Path $data
$sentinel = Join-Path $data ('packaging-user-data-' + [guid]::NewGuid().ToString('N') + '.txt')
'User settings must survive reinstall and uninstall.' | Set-Content -Encoding utf8 $sentinel
$sentinelHash = (Get-FileHash $sentinel).Hash.ToLowerInvariant()
$result = @{ passed = $false; platform = [Environment]::OSVersion.VersionString; sourceCommit = $manifest.sourceCommit;
    osArchitecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString(); appArchitecture = 'x64';
    scope = 'GitHub hosted runner with existing Runtime/build tools; no clean disconnected desktop certification'; cases = @() }

function Assert-UserData {
    Assert-TcHash $sentinel $sentinelHash
}
function Assert-InstalledFiles {
    foreach ($file in $manifest.files) { Assert-TcHash (Join-Path $install $file.path) $file.sha256 }
    if ((Get-ItemProperty $uninstallKey).DisplayVersion -ne $manifest.version) { throw 'Incorrect user uninstall/version registration' }
    if (Test-Path $machineKey) { throw 'Application registered a machine-wide installation' }
    if (Get-ChildItem $install -Filter 'tc-proof*' -Recurse) { throw 'A developer tool was shipped' }
    Assert-UserData
}
function Run-InstalledApp([string]$Mode) {
    $exe = Join-Path $install 'TorrentControl.exe'
    $log = Join-Path $evidence "$Mode-app.log"
    $appData = Join-Path $evidence "$Mode-app-data"
    $oldPath = $env:PATH
    $loaded = @{}
    try {
        # Remove toolchain/Python/vcpkg search paths from the app's environment.
        $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
        $process = Start-Process $exe -ArgumentList "--self-test `"$log`" --self-test-data `"$appData`"" -PassThru
        $deadline = [DateTime]::UtcNow.AddSeconds(90)
        while (-not $process.HasExited) {
            if ([DateTime]::UtcNow -ge $deadline) { $process.Kill($true); throw 'Installed app self-test timed out' }
            try {
                foreach ($module in $process.Modules) {
                    if ($module.ModuleName -match '^(msvcp140.*|vcruntime140.*|concrt140.*)\.dll$') {
                        $loaded[$module.ModuleName] = $module.FileName
                    }
                }
            } catch { if (-not $process.HasExited) { throw } }
            Start-Sleep -Milliseconds 20
            $process.Refresh()
        }
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw "Installed app exited $($process.ExitCode)" }
    } finally { $env:PATH = $oldPath }
    $lines = @(Get-Content -Encoding utf8 $log)
    $passes = @($lines | Where-Object { $_.StartsWith('PASS ') })
    if ($passes.Count -ne 1 -or -not ($passes[0].Substring(5) | ConvertFrom-Json).ok) { throw 'Installed app did not confirm actual bridge/UI success' }
    foreach ($required in @('msvcp140.dll', 'vcruntime140.dll')) {
        if (-not $loaded.ContainsKey($required)) { throw "No observed CRT module: $required" }
    }
    foreach ($path in $loaded.Values) {
        if (-not $path.StartsWith($install + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "CRT loaded outside installed app: $path" }
    }
    return @{ passed = $true; mode = $Mode; loadedCrt = $loaded; bridgeSelfTest = $true; sanitizedPath = $true }
}
function Install-Package([string]$File, [string]$Label, [string[]]$Extra = @()) {
    $log = Join-Path $evidence "$Label-setup.log"
    $exit = Invoke-TcProcess $File (@('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', "/LOG=$log") + $Extra) 600
    if ($exit -ne 0) { throw "$Label setup failed: $exit" }
    Assert-InstalledFiles
    return $log
}
function Uninstall-Package([string]$Label) {
    $exit = Invoke-TcProcess (Join-Path $install 'unins000.exe') @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',"/LOG=$(Join-Path $evidence "$Label-uninstall.log")")
    if ($exit -ne 0 -or (Test-Path "$install/TorrentControl.exe") -or (Test-Path $uninstallKey)) { throw 'Uninstall failed or left app registration/files' }
    Assert-UserData
}

try {
    $online = Join-Path $packages "TorrentControl-$($manifest.version)-dev-unsigned-windows-x64-online-setup.exe"
    $offline = Join-Path $packages "TorrentControl-$($manifest.version)-dev-unsigned-windows-x64-offline-setup.exe"
    $null = Install-Package $online 'online'
    $result.cases += Run-InstalledApp 'online'

    # Setup and Uninstall must both refuse while a real GUI process is alive.
    $running = Start-Process (Join-Path $install 'TorrentControl.exe') -PassThru
    try {
        $deadline = [DateTime]::UtcNow.AddSeconds(30)
        while (-not $running.HasExited -and $running.MainWindowHandle -eq 0) {
            if ([DateTime]::UtcNow -ge $deadline) { throw 'Live application did not create its window' }
            Start-Sleep -Milliseconds 50
            $running.Refresh()
        }
        if ($running.HasExited) { throw 'Application exited before the install-lock control' }
        foreach ($file in @($online, (Join-Path $install 'unins000.exe'))) {
            $code = Invoke-TcProcess $file @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/SP-') 30
            if ($code -eq 0 -or $running.HasExited) { throw 'Setup/uninstall did not protect the live application' }
            Assert-InstalledFiles
        }
        if (-not $running.CloseMainWindow() -or -not $running.WaitForExit(30000) -or $running.ExitCode -ne 0) { throw 'Controlled GUI shutdown failed' }
    } finally { if (-not $running.HasExited) { $running.Kill($true); $running.WaitForExit() } }
    $result.cases += @{ passed = $true; case = 'live-process-install-and-uninstall-refused' }

    # Reinstall/repair restores package bytes while preserving user data.
    'damaged' | Set-Content -Encoding utf8 (Join-Path $install 'frontend/app.css')
    $repairMode = if ($armHost) { 'online' } else { 'offline' }
    $repairPackage = if ($armHost) { $online } else { $offline }
    $null = Install-Package $repairPackage "$repairMode-repair"
    $result.cases += Run-InstalledApp "$repairMode-repair"
    Uninstall-Package 'first'
    $result.cases += @{ passed = $true; case = 'repair-and-user-data-preserving-uninstall' }

    # ARM64 uses the architecture-selecting online bootstrapper. The offline
    # product carries only an x64 Runtime and must refuse an ARM64 host.
    if ($armHost) {
        $exit = Invoke-TcProcess $offline @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/SP-',"/LOG=$(Join-Path $evidence 'offline-architecture-refusal.log')")
        if ($exit -eq 0 -or (Test-Path "$install/TorrentControl.exe") -or (Test-Path $uninstallKey)) { throw 'x64-only offline installer did not refuse ARM64' }
        Assert-UserData
        $result.cases += @{ passed = $true; case = 'offline-x64-runtime-refuses-arm64'; exitCode = $exit; noAppInstalled = $true }
    }
    # Execute the genuine bundled prerequisite, even with an existing Runtime.
    # This is a repair control, not a cold/offline OS claim.
    $runtimeLog = Install-Package $repairPackage "$repairMode-runtime-repair" @('/INSTALLRUNTIME=1')
    if (-not (Select-String $runtimeLog -Pattern 'TC_RUNTIME compatible-after-install' -SimpleMatch)) { throw 'Bundled offline prerequisite did not run and verify' }
    $result.cases += Run-InstalledApp "$repairMode-runtime-repair"
    Uninstall-Package $repairMode
    $result.cases += @{ passed = $true; case = "official-$repairMode-prerequisite-executed"; cleanOfflineMachine = $false }

    # Disposable negative fixtures use the same installer code. They are never
    # uploaded as product packages and cannot change any Runtime registry key.
    $fixtures = Join-Path $evidence 'negative-fixtures'
    $null = New-Item -ItemType Directory -Path $fixtures
    $compiler = Join-Path $packages 'prerequisites/inno/ISCC.exe'
    foreach ($control in @('hash-mismatch', 'vendor-failed', 'success-without-runtime')) {
        $setupName = "TorrentControl-$($manifest.version)-dev-unsigned-windows-x64-$control-setup.exe"
        if (-not $FixtureDirectory) {
            $probe = Join-Path $fixtures "$control.exe"
            $probeExit = if ($control -eq 'vendor-failed') { 23 } else { 0 }
            & cl /nologo /MT "/DTC_PROBE_EXIT=$probeExit" "/Fe:$probe" "/Fo:$(Join-Path $fixtures "$control.obj")" "$PSScriptRoot/installer-prerequisite-probe.cpp"
            if ($LASTEXITCODE -ne 0) { throw 'Prerequisite control build failed' }
            $hash = if ($control -eq 'hash-mismatch') { '0' * 64 } else { (Get-FileHash $probe).Hash.ToLowerInvariant() }
            & $compiler "/DStageRoot=$packages/stage" "/DOutputRoot=$fixtures" "/DAppVersion=$($manifest.version)" "/DInstallMutex=$($manifest.installMutex)" '/DRuntimeMinimum=65535.0.0.0' "/DPackageMode=$control" "/DRuntimeSource=$probe" "/DRuntimeSHA256=$hash" "$repo/packaging/windows/TorrentControl.iss"
            if ($LASTEXITCODE -ne 0) { throw 'Prerequisite negative fixture compilation failed' }
        }
        $setup = if ($FixtureDirectory) { Join-Path $FixtureDirectory $setupName } else { Join-Path $fixtures $setupName }
        $log = Join-Path $evidence "$control-setup.log"
        $exit = Invoke-TcProcess $setup @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/SP-',"/LOG=$log")
        $marker = switch ($control) { 'hash-mismatch' { 'TC_RUNTIME hash-mismatch' }; 'vendor-failed' { 'TC_RUNTIME exit=23' }; default { 'TC_RUNTIME still-incompatible' } }
        if ($exit -eq 0 -or (Test-Path "$install/TorrentControl.exe") -or (Test-Path $uninstallKey) -or -not (Select-String $log -Pattern $marker -SimpleMatch)) {
            throw "Prerequisite refusal was not verified: $control"
        }
        Assert-UserData
        $result.cases += @{ passed = $true; case = $control; exitCode = $exit; noAppInstalled = $true }
    }
    $result.userDataPreserved = $true
    $result.passed = $true
} finally {
    $result | ConvertTo-Json -Depth 12 | Set-Content -Encoding utf8 (Join-Path $evidence 'summary.json')
    # Keep failed installs for inspection on this disposable runner; do not
    # pretend forced cleanup was a successful user uninstall.
    if (Test-Path $sentinel) { Remove-Item -LiteralPath $sentinel }
}
Write-Output 'Windows package installation, app-local CRT, repair, live-job guard and uninstall checks passed.'
