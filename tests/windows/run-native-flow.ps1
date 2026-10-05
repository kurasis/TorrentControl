param(
    [string]$Executable = "build/windows-x64-release/src/app/windows/TorrentControl.exe",
    [string]$EvidenceDirectory = "build/windows-native-evidence"
)

$ErrorActionPreference = "Stop"
$exe = (Resolve-Path $Executable).Path
New-Item -ItemType Directory -Force -Path $EvidenceDirectory | Out-Null
$evidence = (Resolve-Path $EvidenceDirectory).Path
$data = Join-Path $evidence ("data-" + [guid]::NewGuid().ToString("N"))

function Invoke-SelfTest([string]$switch, [string]$log, [int]$expectedExit = 0) {
    $p = Start-Process -FilePath $exe -ArgumentList $switch, "`"$log`"", "--self-test-data", "`"$data`"" -PassThru
    if (-not $p.WaitForExit(180000)) {
        $p.Kill($true)
        throw "$switch did not finish within 180 seconds"
    }
    $lines = @(Get-Content -Encoding UTF8 $log)
    $lines | Write-Output
    if ($p.ExitCode -ne $expectedExit) { throw "$switch exited $($p.ExitCode), expected $expectedExit" }
    if ($expectedExit -eq 0) {
        $pass = @($lines | Where-Object { $_.StartsWith("PASS ") })
        if ($pass.Count -ne 1) { throw "$switch did not write exactly one PASS result" }
        $result = $pass[0].Substring(5) | ConvertFrom-Json
        if (-not $result.ok) { throw "$switch did not confirm success" }
        $result | ConvertTo-Json -Depth 10 | Set-Content -Encoding UTF8 ($log + ".json")
    }
}

Invoke-SelfTest "--self-test-flow" (Join-Path $evidence "flow.log")
Invoke-SelfTest "--self-test-settings" (Join-Path $evidence "restart.log")

# Independently hash the payload selected by the Windows dialogs. This checks
# the actual files produced through WebView2, not a tc-proof substitute.
foreach ($format in @("v1", "v2", "hybrid", "reopened")) {
    $torrent = Join-Path $data "output/$format.torrent"
    $report = & python tools/reference/verify_torrent.py $torrent --root (Join-Path $data "payload")
    $code = $LASTEXITCODE
    $report | Set-Content -Encoding UTF8 (Join-Path $evidence "$format-reference.json")
    if ($code -ne 0) { throw "Independent verifier rejected $format" }
}

# Force the loader to inspect a nonexistent fixed runtime, without uninstalling
# the shared Evergreen runtime or changing machine-wide configuration.
$previousRuntime = $env:WEBVIEW2_BROWSER_EXECUTABLE_FOLDER
try {
    $env:WEBVIEW2_BROWSER_EXECUTABLE_FOLDER = Join-Path $evidence "missing-runtime"
    Invoke-SelfTest "--self-test" (Join-Path $evidence "missing-runtime.log") 3
} finally {
    $env:WEBVIEW2_BROWSER_EXECUTABLE_FOLDER = $previousRuntime
}
