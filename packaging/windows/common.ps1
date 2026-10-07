Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-TcHash([string]$Path, [string]$Sha256) {
    if ($Sha256 -notmatch '^[0-9a-f]{64}$' -or (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant() -ne $Sha256) {
        throw "SHA-256 verification failed: $Path"
    }
}

function Get-TcPinnedFile($Pin, [string]$Destination) {
    if (([uri]$Pin.url).Scheme -ne 'https') { throw 'Prerequisites require HTTPS' }
    if (-not (Test-Path -LiteralPath $Destination)) {
        $partial = $Destination + '.' + [guid]::NewGuid().ToString('N') + '.partial'
        try {
            $ProgressPreference = 'SilentlyContinue'
            Invoke-WebRequest -Uri $Pin.url -OutFile $partial
            Assert-TcHash $partial $Pin.sha256
            Move-Item -LiteralPath $partial -Destination $Destination
        } finally {
            if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial }
        }
    }
    Assert-TcHash $Destination $Pin.sha256
    return $Destination
}

function Assert-TcMicrosoftSignature([string]$Path) {
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne 'Valid' -or $null -eq $signature.SignerCertificate -or
        $signature.SignerCertificate.Subject -notmatch '(^|,\s*)O=Microsoft Corporation(,|$)') {
        throw "A valid Microsoft Authenticode signature is required: $Path ($($signature.Status))"
    }
    return @{ subject = $signature.SignerCertificate.Subject; thumbprint = $signature.SignerCertificate.Thumbprint }
}

function Invoke-TcProcess([string]$File, [string[]]$Arguments, [int]$TimeoutSeconds = 300) {
    $quoted = @($Arguments | ForEach-Object {
        if ($_.Contains('"') -or $_.Contains("`n") -or $_.Contains("`r")) { throw 'Unsupported command-line quote/control character' }
        '"' + $_ + '"'
    }) -join ' '
    $process = Start-Process -FilePath $File -ArgumentList $quoted -PassThru
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $process.Kill($true)
        $process.WaitForExit()
        throw "Process exceeded ${TimeoutSeconds}s: $File"
    }
    return $process.ExitCode
}
