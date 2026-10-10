param([string]$OutputDirectory = (Join-Path $PSScriptRoot ('../publish/radio-recovery-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))))
$ErrorActionPreference = 'Stop'
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Recovery directory already exists; choose a new path to preserve it.' }
$devices = @(Get-CimInstance Win32_PnPSignedDriver | Where-Object DeviceID -Like 'PCI\VEN_10EC&DEV_C852*')
if ($devices.Count -ne 1 -or $devices[0].DriverProviderName -eq 'Windrop' -or $devices[0].InfName -notmatch '^oem\d+\.inf$') {
    throw 'Expected exactly one RTL8852CE with its original OEM driver. Nothing was exported or changed.'
}
$device = $devices[0]
$backup = Join-Path $output 'original-driver'
New-Item -ItemType Directory -Path $backup | Out-Null
& "$env:SystemRoot/System32/pnputil.exe" /export-driver $device.InfName $backup
if ($LASTEXITCODE -ne 0) { throw 'Original driver export failed; preserve the output for diagnosis.' }
$files = @(Get-ChildItem -LiteralPath $backup -Recurse -File)
if (-not ($files | Where-Object Extension -EQ '.inf') -or -not ($files | Where-Object Extension -EQ '.sys')) { throw 'Export did not contain the original INF and kernel binary.' }
$hashes = [ordered]@{}
foreach ($file in $files) { $hashes[$file.FullName.Substring($output.Length+1)] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash }
[ordered]@{ schema=1; deviceInstanceId=$device.DeviceID; deviceName=$device.DeviceName; originalVersion=$device.DriverVersion; originalStoreInf=$device.InfName; sha256=$hashes } |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output 'recovery.json') -Encoding UTF8
@'
Original RTL8852CE recovery files

Keep this directory offline and outside the Git repository.
If a future Windrop driver test fails, use Device Manager:
1. Find this PCI device (its exact instance ID is in recovery.json).
2. Update driver -> Browse my computer -> Let me pick -> Have Disk.
3. Select the original INF under original-driver, then its Realtek model.
4. Complete the update and reboot only if Windows requires it.

Device Manager may list the device under Windrop PCIe Radio or Other devices
while the experimental driver is bound. This export does not change binding,
install anything, or alter boot/certificate settings. If Windows cannot boot,
use Windows Recovery/Safe Mode before attempting a driver restore. A restore
after an actual Windrop installation is not yet hardware-qualified.
'@ | Set-Content -LiteralPath (Join-Path $output 'RECOVERY.txt') -Encoding UTF8
Write-Host "Exported original driver $($device.DriverVersion) to $output. The installed driver is unchanged."
