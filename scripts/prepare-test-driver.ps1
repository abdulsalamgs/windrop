param(
    [string]$SourceDirectory = (Join-Path $PSScriptRoot '../build/driver/WindropRadio'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot ('../publish/test-driver-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')))
)
$ErrorActionPreference = 'Stop'
# Prepare files only. No certificate trust, driver installation or boot changes.
$repo = Split-Path -Parent $PSScriptRoot
$source = (Resolve-Path -LiteralPath $SourceDirectory).Path
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Output already exists; choose a new directory to preserve the existing package.' }
foreach ($name in @('WindropRadio.inf','WindropRadio.sys')) {
    if (-not (Test-Path -LiteralPath (Join-Path $source $name))) { throw "Missing unsigned build file: $name" }
}
$signTool = Join-Path $repo '.tools/driver-packages/Microsoft.Windows.SDK.CPP.10.0.26100.1/c/bin/10.0.26100.0/x64/signtool.exe'
$inf2cat = Join-Path $repo '.tools/driver-packages/Microsoft.Windows.WDK.x64.10.0.26100.6584/c/bin/10.0.26100.0/x86/Inf2Cat.exe'
foreach ($tool in @($signTool,$inf2cat)) { if (-not (Test-Path -LiteralPath $tool)) { throw 'Run build-driver.ps1 first to restore the signing tools.' } }
function Invoke-Checked([string]$tool,[string[]]$arguments) {
    $ErrorActionPreference = 'Continue'
    & $tool @arguments
    if ($LASTEXITCODE -ne 0) { throw "Signing command failed ($LASTEXITCODE): $tool" }
}
$driver = Join-Path $output 'driver'
New-Item -ItemType Directory -Path $driver | Out-Null
Copy-Item -LiteralPath (Join-Path $source 'WindropRadio.inf'),(Join-Path $source 'WindropRadio.sys') -Destination $driver
$certificate = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=Windrop Development' -FriendlyName 'Windrop local development signing' -CertStoreLocation 'Cert:/CurrentUser/My' -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(1)
try {
    # Signing SYS first changes its bytes. Generate the catalog afterward, then
    # sign that catalog so its package hashes cover the embedded SYS signature.
    Invoke-Checked $signTool @('sign','/s','My','/sha1',$certificate.Thumbprint,'/fd','SHA256',(Join-Path $driver 'WindropRadio.sys'))
    Invoke-Checked $inf2cat @("/driver:$driver",'/os:10_CO_X64','/uselocaltime')
    Invoke-Checked $signTool @('sign','/s','My','/sha1',$certificate.Thumbprint,'/fd','SHA256',(Join-Path $driver 'WindropRadio.cat'))
    Export-Certificate -Cert $certificate -FilePath (Join-Path $output 'WindropDevelopment.cer') | Out-Null
    $hashes = [ordered]@{}
    foreach ($relative in @('driver/WindropRadio.inf','driver/WindropRadio.sys','driver/WindropRadio.cat','WindropDevelopment.cer')) {
        $hashes[$relative] = (Get-FileHash -LiteralPath (Join-Path $output $relative) -Algorithm SHA256).Hash
    }
    [ordered]@{ schema=1; purpose='local-development-only'; certificateThumbprint=$certificate.Thumbprint; expiresUtc=$certificate.NotAfter.ToUniversalTime().ToString('o'); sha256=$hashes } |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output 'package.json') -Encoding UTF8
    & (Join-Path $PSScriptRoot 'verify-test-driver.ps1') -PackageDirectory $output
    Write-Host "Prepared $output. Certificate is NOT trusted; driver is NOT installed. Boot policy is unchanged."
} catch {
    # Preserve failed output for diagnosis, but remove only this newly created key.
    Remove-Item -LiteralPath "Cert:/CurrentUser/My/$($certificate.Thumbprint)" -DeleteKey
    throw
}
