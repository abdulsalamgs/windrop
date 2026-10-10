param([string]$NativeDirectory = (Join-Path $PSScriptRoot '../build/native/Release'))
$ErrorActionPreference = 'Stop'
$exe = Join-Path $NativeDirectory 'windrop-awdl.exe'
$fixtures = Join-Path (Split-Path -Parent $PSScriptRoot) '.tools/native-tests'
New-Item -ItemType Directory -Force $fixtures | Out-Null
function Bytes([string]$hex) {
    $hex = $hex -replace '\s', ''
    [byte[]] $bytes = for ($i = 0; $i -lt $hex.Length; $i += 2) { [Convert]::ToByte($hex.Substring($i, 2), 16) }
    return ,$bytes
}
# Independent wire fixture: radiotap, QoS MPDU, LLC/SNAP, AWDL data header,
# then a literal IPv6 packet. No Windrop/OWL serializer generates the expected bytes.
# IEEE 802.11 header and OWL tx.c document the field layout; see native/README.md.
$ipv6 = Bytes '60000000 0008 3b40 fe80000000000000 a8bbccfffeddeeff fe80000000000000 000000fffe000001 00010203fcfdfeff'
$frame = (Bytes '00000a002200000000d8 88000000 020000000001 aabbccddeeff 002500ff9473 0000 0000 aaaa030017f20800 03040000000086dd') + $ipv6
$inputFile = Join-Path $fixtures 'data.pcap'
$outputFile = Join-Path $fixtures 'ipv6.bin'
$stream = [IO.File]::Create($inputFile)
$writer = New-Object IO.BinaryWriter($stream)
try {
    $writer.Write((Bytes 'd4c3b2a1 0200 0400 00000000 00000000 ffff0000 7f000000'))
    $writer.Write([uint32]1); $writer.Write([uint32]0)
    $writer.Write([uint32]$frame.Length); $writer.Write([uint32]$frame.Length)
    $writer.Write([byte[]]$frame)
} finally { $writer.Dispose() }
& $exe replay $inputFile $outputFile --peer aabbccddeeff
if ($LASTEXITCODE -ne 0) { throw 'AWDL replay failed' }
$actual = [IO.File]::ReadAllBytes($outputFile)
if ([Convert]::ToBase64String($actual) -ne [Convert]::ToBase64String($ipv6)) {
    throw 'AWDL replay did not deliver the exact IPv6 bytes'
}
Write-Host 'PASS: public AWDL replay CLI delivers exact IPv6 bytes from an independent frame fixture.'

$wrongOui = [byte[]]$frame.Clone()
$wrongOui[40] = 0xff
$capture = [IO.File]::ReadAllBytes($inputFile)
[Array]::Copy($wrongOui, 0, $capture, 40, $wrongOui.Length)
$badInput = Join-Path $fixtures 'non-awdl.pcap'
$badOutput = Join-Path $fixtures 'non-awdl.bin'
[IO.File]::WriteAllBytes($badInput, $capture)
& $exe replay $badInput $badOutput --peer aabbccddeeff
if ($LASTEXITCODE -ne 0 -or (Get-Item $badOutput).Length -ne 0) { throw 'A non-Apple LLC frame was incorrectly delivered as AWDL' }
Write-Host 'PASS: non-Apple LLC frames are dropped.'

$wrongHeader = [byte[]]$frame.Clone()
$wrongHeader[44] = 0xff
$capture = [IO.File]::ReadAllBytes($inputFile)
[Array]::Copy($wrongHeader, 0, $capture, 40, $wrongHeader.Length)
$badInput = Join-Path $fixtures 'bad-awdl-header.pcap'
$badOutput = Join-Path $fixtures 'bad-awdl-header.bin'
[IO.File]::WriteAllBytes($badInput, $capture)
& $exe replay $badInput $badOutput --peer aabbccddeeff
if ($LASTEXITCODE -ne 0 -or (Get-Item $badOutput).Length -ne 0) { throw 'An invalid AWDL data prefix was incorrectly delivered' }
Write-Host 'PASS: invalid AWDL data prefixes are dropped.'

# OWL test_awdl_channel.cpp records this independent iPhone idle sequence.
# A hand-built MIF elects that peer and carries the captured channel values.
$action = Bytes '00000a002200000000d8 d0000000 ffffffffffff aabbccddeeff 002500ff9473 0000 7f0017f208100300 0000000000000000 1502001001 051500 00000000 00 aabbccddeeff 64000000 64000000 0000 122600 0f030003ffff 8480 0000 9580 0000 0000 0000 0000 0000 0651 0000 9580 0000 0000 0000 0000 0000'
$phoneInput = Join-Path $fixtures 'iphone-channels.pcap'
$phoneOutput = Join-Path $fixtures 'iphone-channels.bin'
$writer = New-Object IO.BinaryWriter([IO.File]::Create($phoneInput))
try {
    $writer.Write((Bytes 'd4c3b2a1 0200 0400 00000000 00000000 ffff0000 7f000000'))
    $writer.Write([uint32]1); $writer.Write([uint32]0)
    $writer.Write([uint32]$action.Length); $writer.Write([uint32]$action.Length)
    $writer.Write([byte[]]$action)
} finally { $writer.Dispose() }
$result = & $exe replay $phoneInput $phoneOutput --peer aabbccddeeff
$result | Write-Host
if ($LASTEXITCODE -ne 0 -or $result -notmatch 'channels=0,0,149,0,0,0,0,0,6,0,149,0,0,0,0,0$') {
    throw 'An iPhone infrastructure-channel window did not preserve usable AWDL social windows'
}
Write-Host 'PASS: captured iPhone channel sequence preserves social windows and skips unsupported infrastructure channels.'
