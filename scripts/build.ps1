param([switch]$Publish)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
Set-Location $taskRoot
dotnet build Windrop.sln -c Release --nologo
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
dotnet test tests/WinDrop.Protocol.Tests -c Release --no-build --nologo
if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
if ($Publish) {
    dotnet publish src/Windrop.App -c Release -r win-x64 --self-contained true -o publish/windows
    if ($LASTEXITCODE -ne 0) { throw 'Desktop publish failed' }
    dotnet publish src/WinDrop.Cli -c Release -r win-x64 --self-contained true -o publish/cli
    if ($LASTEXITCODE -ne 0) { throw 'CLI publish failed' }
    dotnet publish src/Windrop.Radio -c Release -r linux-x64 --self-contained true -o publish/radio
    if ($LASTEXITCODE -ne 0) { throw 'Radio publish failed' }
    Copy-Item LICENSE,THIRD-PARTY-NOTICES.md -Destination publish/windows
    Compress-Archive -Path publish/windows/* -DestinationPath publish/Windrop-windows-x64.zip -Force
}
