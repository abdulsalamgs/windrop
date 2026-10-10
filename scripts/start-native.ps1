param(
    [string]$NativeDirectory = (Join-Path $PSScriptRoot '../publish/native'),
    [string]$AppPath = (Join-Path $PSScriptRoot '../publish/windows/Windrop.exe')
)
$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this script in an administrator PowerShell after installing a properly signed Windrop driver.' }
foreach ($path in @($AppPath, (Join-Path $NativeDirectory 'windrop-hardware.exe'), (Join-Path $NativeDirectory 'windrop-awdl.exe'), (Join-Path $NativeDirectory 'radio/windrop-radio.exe'))) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing package file: $path. See native/README.md." }
}
& (Join-Path $NativeDirectory 'windrop-hardware.exe') doctor
if ($LASTEXITCODE -ne 0) { throw 'The signed RTL8852CE driver is not ready. No driver installation or boot changes were attempted.' }
if (Get-NetAdapter -Name awdl0 -ErrorAction SilentlyContinue) { throw 'An awdl0 adapter already exists. Stop its owner before starting a new session.' }
$oldKey = $env:WINDROP_RADIO_KEY
$oldHost = $env:WINDROP_RADIO_HOST
$processes = @()
try {
    $random = New-Object byte[] 32
    $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($random) } finally { $rng.Dispose() }
    $env:WINDROP_RADIO_KEY = ([BitConverter]::ToString($random)).Replace('-', '')
    $env:WINDROP_RADIO_HOST = '127.0.0.1'
    $awdl = Start-Process (Join-Path $NativeDirectory 'windrop-awdl.exe') -ArgumentList 'run' -WindowStyle Hidden -PassThru
    $processes += $awdl
    $deadline = [DateTime]::UtcNow.AddMinutes(2)
    while (-not (Get-NetIPAddress -InterfaceAlias awdl0 -AddressFamily IPv6 -ErrorAction SilentlyContinue | Where-Object IPAddress -Like 'fe80:*')) {
        if ($awdl.HasExited) { throw "AWDL startup failed with exit $($awdl.ExitCode)." }
        if ([DateTime]::UtcNow -gt $deadline) { throw 'AWDL initialization timed out.' }
        Start-Sleep -Milliseconds 100
    }
    $bridge = Start-Process (Join-Path $NativeDirectory 'radio/windrop-radio.exe') -ArgumentList @('--interface','awdl0','--listen','127.0.0.1') -WindowStyle Hidden -PassThru
    $processes += $bridge
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        if ($awdl.HasExited -or $bridge.HasExited) { throw 'The native radio stopped during startup.' }
        $client = New-Object Net.Sockets.TcpClient
        try { $client.Connect('127.0.0.1',7788); $connected = $true } catch { $connected = $false } finally { $client.Dispose() }
        if (-not $connected) { Start-Sleep -Milliseconds 100 }
        if ([DateTime]::UtcNow -gt $deadline) { throw 'Radio bridge startup timed out.' }
    } while (-not $connected)
    $app = Start-Process -FilePath $AppPath -PassThru
    $processes += $app
    while (-not $app.HasExited) {
        if ($awdl.HasExited -or $bridge.HasExited) { throw 'The native radio stopped; the desktop session is being closed.' }
        Start-Sleep -Milliseconds 250
    }
} finally {
    foreach ($process in $processes) { if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue } }
    $env:WINDROP_RADIO_KEY = $oldKey
    $env:WINDROP_RADIO_HOST = $oldHost
}
