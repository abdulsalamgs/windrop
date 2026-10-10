param([string]$SourceDirectory = (Join-Path $PSScriptRoot '../build/driver/WindropRadio'))
$ErrorActionPreference = 'Stop'
$prepare = Join-Path $PSScriptRoot 'prepare-test-driver.ps1'
$verify = Join-Path $PSScriptRoot 'verify-test-driver.ps1'
if (-not (Test-Path -LiteralPath $prepare)) { throw 'Test-driver preparation CLI is missing' }
$output = Join-Path (Split-Path -Parent $PSScriptRoot) ('.tools/signing-tests/' + [Guid]::NewGuid().ToString('N'))
$original = (Get-FileHash (Join-Path $SourceDirectory 'WindropRadio.sys')).Hash
$certificate = $null
try {
    & $prepare -SourceDirectory $SourceDirectory -OutputDirectory $output
    $state = Get-Content (Join-Path $output 'package.json') -Raw | ConvertFrom-Json
    $certificate = $state.certificateThumbprint
    & $verify -PackageDirectory $output
    if ((Get-FileHash (Join-Path $SourceDirectory 'WindropRadio.sys')).Hash -ne $original) { throw 'Unsigned source driver was changed' }
    foreach ($store in @('Cert:/CurrentUser/Root','Cert:/LocalMachine/Root','Cert:/CurrentUser/TrustedPublisher','Cert:/LocalMachine/TrustedPublisher')) {
        if (Test-Path -LiteralPath (Join-Path $store $certificate)) { throw 'Preparation unexpectedly trusted the development certificate' }
    }
    Write-Host 'PASS: preparation signs a separate package and leaves its certificate untrusted.'
    $protectedHash = (Get-FileHash (Join-Path $output 'driver/WindropRadio.sys')).Hash
    $rejected = $false
    try { & $prepare -SourceDirectory $SourceDirectory -OutputDirectory $output } catch { $rejected = $true }
    if (-not $rejected -or (Get-FileHash (Join-Path $output 'driver/WindropRadio.sys')).Hash -ne $protectedHash) { throw 'Existing package was not preserved' }
    Write-Host 'PASS: preparation refuses to overwrite an existing package.'
    $path = Join-Path $output 'driver/WindropRadio.sys'
    $signedBytes = [IO.File]::ReadAllBytes($path)
    $bytes = [IO.File]::ReadAllBytes($path); $bytes[512] = $bytes[512] -bxor 1
    [IO.File]::WriteAllBytes($path,$bytes)
    $rejected = $false
    try { & $verify -PackageDirectory $output } catch { $rejected = $true }
    if (-not $rejected) { throw 'Altered signed driver was accepted' }
    Write-Host 'PASS: package verification rejects an altered driver.'
    $state.sha256.'driver/WindropRadio.sys' = (Get-FileHash -LiteralPath $path).Hash
    $state | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output 'package.json') -Encoding UTF8
    $rejected = $false
    try { & $verify -PackageDirectory $output } catch { $rejected = $true }
    if (-not $rejected) { throw 'A rewritten manifest bypassed the embedded signature check' }
    Write-Host 'PASS: an updated manifest cannot bypass a broken embedded signature.'
    [IO.File]::WriteAllBytes($path,$signedBytes)
    $state.sha256.'driver/WindropRadio.sys' = (Get-FileHash -LiteralPath $path).Hash
    $inf = Join-Path $output 'driver/WindropRadio.inf'
    Add-Content -LiteralPath $inf -Value '; altered after catalog signing'
    $state.sha256.'driver/WindropRadio.inf' = (Get-FileHash -LiteralPath $inf).Hash
    $state | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output 'package.json') -Encoding UTF8
    $rejected = $false
    try { & $verify -PackageDirectory $output } catch { $rejected = $true }
    if (-not $rejected) { throw 'A rewritten manifest bypassed signed catalog membership' }
    Write-Host 'PASS: an updated manifest cannot bypass altered catalog members.'
} finally {
    # Only remove the private signing key this test created, never a trusted key.
    if ($certificate -and (Test-Path -LiteralPath "Cert:/CurrentUser/My/$certificate")) {
        Remove-Item -LiteralPath "Cert:/CurrentUser/My/$certificate" -DeleteKey
    }
}
