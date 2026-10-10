param()
$ErrorActionPreference = 'Stop'

# Compile and validate only. This script never installs a driver, replaces the
# Realtek driver, changes boot policy, generates a signing certificate, or reboots.
$repoRoot = Split-Path -Parent $PSScriptRoot
$toolsPath = Join-Path $repoRoot '.tools'
$packagePath = Join-Path $toolsPath 'driver-packages'
$nugetPath = Join-Path $toolsPath 'nuget.exe'
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswherePath)) {
    throw 'Install Visual Studio 2022 C++ Build Tools and x64 Spectre libraries. See docs/WINDOWS-DRIVER.md.'
}
$vsPath = & $vswherePath -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'Visual Studio 2022 C++ Build Tools were not found.' }
$msbuildPath = Join-Path $vsPath 'MSBuild/Current/Bin/amd64/MSBuild.exe'
$toolsetPath = Join-Path $vsPath 'MSBuild/Microsoft/VC/v170/Platforms/x64/PlatformToolsets/WindowsKernelModeDriver10.0'
if (-not (Test-Path -LiteralPath $toolsetPath)) {
    throw 'Install the Visual Studio Windows Driver Kit component. See docs/WINDOWS-DRIVER.md.'
}
New-Item -ItemType Directory -Force -Path $toolsPath | Out-Null
if (-not (Test-Path -LiteralPath $nugetPath)) {
    Invoke-WebRequest 'https://dist.nuget.org/win-x86-commandline/v6.14.0/nuget.exe' -OutFile $nugetPath
}
$signature = Get-AuthenticodeSignature -LiteralPath $nugetPath
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Microsoft Corporation') {
    throw 'NuGet executable Microsoft signature verification failed.'
}
& $nugetPath install Microsoft.Windows.WDK.x64 -Version 10.0.26100.6584 -OutputDirectory $packagePath -NonInteractive -Source 'https://api.nuget.org/v3/index.json' -Verbosity quiet
if ($LASTEXITCODE -ne 0) { throw 'WDK package restore failed.' }
& $msbuildPath (Join-Path $repoRoot 'drivers/Windrop.Radio/WindropRadio.vcxproj') /m /nologo /verbosity:minimal /p:Configuration=Release /p:Platform=x64
if ($LASTEXITCODE -ne 0) { throw 'Driver build or validation failed.' }
Write-Host 'Built the unsigned development driver in build/driver. It has NOT been installed or hardware-tested.'
