param([switch]$Publish)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$dependencies = Join-Path $repo '.tools/native'
function Invoke-Checked([string]$executable, [string[]]$arguments) {
    # PowerShell 5 treats native stderr as error records under redirection,
    # including ordinary CMake/Git progress. Exit status is authoritative here.
    $ErrorActionPreference = 'Continue'
    & $executable @arguments
    if ($LASTEXITCODE -ne 0) { throw "Native command failed ($LASTEXITCODE): $executable" }
}
New-Item -ItemType Directory -Force $dependencies | Out-Null
& (Join-Path $PSScriptRoot 'build-driver.ps1')

function Download([string]$url, [string]$name, [string]$hash) {
    $path = Join-Path $dependencies $name
    if (-not (Test-Path -LiteralPath $path)) { Invoke-WebRequest $url -OutFile $path }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $hash) { throw "SHA256 mismatch: $name" }
    return $path
}
$cmakeZip = Download 'https://github.com/Kitware/CMake/releases/download/v3.31.8/cmake-3.31.8-windows-x86_64.zip' 'cmake.zip' '81AA9964DBABD71FE02E7EC50472FD3AD56138C49944515ECE9001EFBFF8D719'
$sevenZip = Download 'https://github.com/ip7z/7zip/releases/download/26.04/7zr.exe' '7zr.exe' '256FECA8E274E5DA655E2A284FABAFD9F554365EB164862089DACD4E8276D282'
$usbArchive = Download 'https://github.com/libusb/libusb/releases/download/v1.0.29/libusb-1.0.29.7z' 'libusb.7z' '964A38152CA9A104CD00EC8D2F0617B89CD814F9B635E29763C68563D951521D'
$wintunZip = Download 'https://www.wintun.net/builds/wintun-0.14.1.zip' 'wintun.zip' '07C256185D6EE3652E09FA55C0B673E2624B565E02C4B9091C79CA7D2F24EF51'
$cmake = Join-Path $dependencies 'cmake-3.31.8-windows-x86_64/bin/cmake.exe'
if (-not (Test-Path -LiteralPath $cmake)) { Expand-Archive -LiteralPath $cmakeZip -DestinationPath $dependencies }
$usb = Join-Path $dependencies 'libusb'
if (-not (Test-Path -LiteralPath (Join-Path $usb 'VS2022/MS64/dll/libusb-1.0.dll'))) {
    New-Item -ItemType Directory -Force $usb | Out-Null
    # Both Windows tar and CMake's libarchive lack this archive's BCJ2 codec.
    # Use the versioned, hash-verified official standalone 7-Zip tool instead.
    Invoke-Checked $sevenZip @('x', $usbArchive, "-o$usb", '-y', '-bso0')
}
$wintun = Join-Path $dependencies 'wintun'
if (-not (Test-Path -LiteralPath $wintun)) { Expand-Archive -LiteralPath $wintunZip -DestinationPath $dependencies }
$dll = Join-Path $wintun 'bin/amd64/wintun.dll'
$signature = Get-AuthenticodeSignature -LiteralPath $dll
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'WireGuard LLC') { throw 'Wintun signature verification failed' }

function Source([string]$name, [string]$url, [string]$revision) {
    $path = Join-Path $dependencies "sources/$name"
    if (-not (Test-Path -LiteralPath $path)) {
        New-Item -ItemType Directory -Force $path | Out-Null
        git -C $path init --quiet
        if ($LASTEXITCODE -ne 0) { throw "Cannot initialize $name source" }
        git -C $path remote add origin $url
        git -C $path fetch --quiet --depth 1 origin $revision
        if ($LASTEXITCODE -ne 0) { throw "Cannot fetch $name source" }
        git -C $path -c core.autocrlf=false checkout --quiet --detach FETCH_HEAD
        if ($LASTEXITCODE -ne 0) { throw "Cannot check out $name source" }
    }
    if ((git -C $path rev-parse HEAD) -ne $revision) { throw "Unexpected $name revision; preserve it and use a fresh dependency directory" }
    return $path
}
function Patch([string]$path, [string]$name) {
    $patch = Join-Path $repo "native/patches/$name.patch"
    $ErrorActionPreference = 'Continue'
    git -C $path apply --reverse --check --ignore-space-change $patch 2>$null
    $ErrorActionPreference = 'Stop'
    if ($LASTEXITCODE -eq 0) { return }
    git -C $path apply --check --ignore-space-change $patch
    if ($LASTEXITCODE -ne 0) { throw "Unexpected source edits prevent $name patch" }
    git -C $path apply --ignore-space-change $patch
    if ($LASTEXITCODE -ne 0) { throw "$name patch failed" }
}
$devourer = Source 'devourer' 'https://github.com/OpenIPC/devourer.git' 'f369aac553c9ce884901115ef9e66509f93c5683'
$owl = Source 'owl' 'https://github.com/jedbillyb/owl.git' '832d70f815c3d4a06a02117bf0fc5e868daa1ff0'
$usbSource = Source 'libusb' 'https://github.com/libusb/libusb.git' '15a7ebb4d426c5ce196684347d2b7cafad862626'
$ErrorActionPreference = 'Continue'
git -C $owl -c core.autocrlf=false submodule update --init --depth 1 radiotap
$ErrorActionPreference = 'Stop'
if ($LASTEXITCODE -ne 0) { throw 'Radiotap source fetch failed' }
Patch $devourer 'devourer-windows'
Patch $owl 'owl-windows'
Patch (Join-Path $owl 'radiotap') 'radiotap-windows'

# Keep the SDK in NuGet: this avoids changing the system's installed Windows SDK.
$packages = Join-Path $repo '.tools/driver-packages'
$props = @"
<Project>
  <Import Project="$packages/Microsoft.Windows.SDK.CPP.x64.10.0.26100.1/build/native/Microsoft.Windows.SDK.cpp.x64.props" />
  <Import Project="$packages/Microsoft.Windows.SDK.CPP.10.0.26100.1/build/native/Microsoft.Windows.SDK.cpp.props" />
</Project>
"@
[IO.File]::WriteAllText((Join-Path $repo 'build/Directory.Build.props'), $props)
$output = Join-Path $repo 'build/native'
Invoke-Checked $cmake @('-S', (Join-Path $repo 'native'), '-B', $output, '-G', 'Visual Studio 17 2022', '-A', 'x64', "-DDEVOURER_SOURCE=$devourer", "-DOWL_SOURCE=$owl", "-DLIBUSB_SOURCE=$usb", "-DWINTUN_SOURCE=$wintun")
Invoke-Checked $cmake @('--build', $output, '--config', 'Release', '--target', 'windrop-awdl', 'windrop-hardware', 'windrop-tunnel', '--parallel', '6')
$release = Join-Path $output 'Release'
Copy-Item -LiteralPath $dll -Destination $release
Copy-Item -LiteralPath (Join-Path $usb 'VS2022/MS64/dll/libusb-1.0.dll') -Destination $release
& (Join-Path $PSScriptRoot 'test-native.ps1') -NativeDirectory $release
Write-Host 'Built native frame/AWDL workers and unsigned kernel driver. No driver or adapter was installed.'
if ($Publish) {
    $dotnet = Join-Path $repo '.tools/dotnet/dotnet.exe'
    if (-not (Test-Path -LiteralPath $dotnet)) { $dotnet = 'dotnet' }
    $package = Join-Path $repo 'publish/native'
    New-Item -ItemType Directory -Force $package | Out-Null
    Copy-Item (Join-Path $release 'windrop-*.exe'), (Join-Path $release '*.dll') -Destination $package
    $driverPackage = Join-Path $package 'driver'
    New-Item -ItemType Directory -Force $driverPackage | Out-Null
    Copy-Item (Join-Path $repo 'build/driver/WindropRadio/*') -Destination $driverPackage -Recurse -Force
    & $dotnet publish (Join-Path $repo 'src/Windrop.Radio') -c Release -r win-x64 --self-contained true -o (Join-Path $package 'radio') --nologo
    if ($LASTEXITCODE -ne 0) { throw 'Windows radio bridge publish failed' }
    $licenses = Join-Path $package 'licenses'
    New-Item -ItemType Directory -Force $licenses | Out-Null
    Copy-Item (Join-Path $repo 'native/licenses/*') -Destination $licenses
    Copy-Item (Join-Path $wintun 'LICENSE.txt') -Destination (Join-Path $licenses 'Wintun-binaries.txt')
    Copy-Item (Join-Path $usbSource 'COPYING') -Destination (Join-Path $licenses 'libusb-LGPL-2.1.txt')
    Copy-Item (Join-Path $repo 'LICENSE') -Destination (Join-Path $licenses 'Windrop-MIT.txt')
    # Supply pristine pinned upstream source archives plus the exact port patches.
    $sources = Join-Path $package 'sources'
    New-Item -ItemType Directory -Force $sources | Out-Null
    foreach ($entry in @(@($devourer, 'devourer'), @($owl, 'owl'), @((Join-Path $owl 'radiotap'), 'radiotap'), @($usbSource, 'libusb'))) {
        $archivePath = Join-Path $sources ($entry[1] + '.zip')
        git -C $entry[0] archive --format=zip "--output=$archivePath" HEAD
        if ($LASTEXITCODE -ne 0) { throw 'Corresponding source archive failed' }
    }
    # Include exact working-tree contents of all tracked Windrop sources,
    # including the self-contained .NET bridge, rather than only C++ sources.
    foreach ($relative in (git -C $repo ls-files)) {
        $sourceFile = Join-Path $repo $relative
        if (-not (Test-Path -LiteralPath $sourceFile -PathType Leaf)) { continue }
        $targetFile = Join-Path $sources "windrop/$relative"
        New-Item -ItemType Directory -Force (Split-Path -Parent $targetFile) | Out-Null
        Copy-Item -LiteralPath $sourceFile -Destination $targetFile -Force
    }
    Copy-Item (Join-Path $repo 'THIRD-PARTY-NOTICES.md') -Destination $package
    $packageReadme = (Get-Content (Join-Path $repo 'native/README.md') -Raw).Replace('](../', '](sources/windrop/')
    [IO.File]::WriteAllText((Join-Path $package 'README.md'), $packageReadme, (New-Object Text.UTF8Encoding($false)))
    Compress-Archive -Path (Join-Path $package '*') -DestinationPath (Join-Path $repo 'publish/Windrop-native-development-x64.zip') -Force
    Write-Host 'Published native development package with corresponding source. The driver is unsigned and not installed.'
}
