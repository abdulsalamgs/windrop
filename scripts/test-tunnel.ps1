param(
    [string]$NativeDirectory = (Join-Path $PSScriptRoot '../build/native/Release'),
    [ValidateSet('Stop','Eof')][string]$Shutdown = 'Stop'
)
$ErrorActionPreference = 'Stop'
# Explicit local networking check: creates and removes its own temporary,
# signed Wintun IPv6 adapter. Does not initialize a physical radio.
$name = 'WindropTest' + [Guid]::NewGuid().ToString('N').Substring(0,8)
$start = New-Object Diagnostics.ProcessStartInfo
$start.FileName = Join-Path $NativeDirectory 'windrop-tunnel.exe'
$start.Arguments = "serve --name $name --address fe80::200:ff:fe00:1 --mtu 1450"
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardInput = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$process = New-Object Diagnostics.Process
$process.StartInfo = $start
$socket = $null
$started = $false
function Read-Bytes([int]$count) {
    $bytes = New-Object byte[] $count
    $offset = 0
    while ($offset -lt $count) {
        $read = $process.StandardOutput.BaseStream.ReadAsync($bytes,$offset,$count-$offset)
        if (-not $read.Wait(5000) -or $read.Result -eq 0) { throw 'Tunnel packet read timed out or ended' }
        $offset += $read.Result
    }
    return ,$bytes
}
function Write-Record([uint32]$type,[byte[]]$bytes) {
    $header = [BitConverter]::GetBytes($type) + [BitConverter]::GetBytes([uint32]$bytes.Length)
    $process.StandardInput.BaseStream.Write($header,0,8)
    $process.StandardInput.BaseStream.Write($bytes,0,$bytes.Length)
    $process.StandardInput.BaseStream.Flush()
}
try {
    $started = $process.Start()
    if (-not $started) { throw 'Tunnel process did not start' }
    $hello = New-Object byte[] 8
    $offset = 0
    while ($offset -lt 8) {
        $read = $process.StandardOutput.BaseStream.ReadAsync($hello,$offset,8-$offset)
        if (-not $read.Wait(120000)) { throw 'Tunnel readiness timed out' }
        if ($read.Result -eq 0) { throw "Tunnel did not become ready: $($process.StandardError.ReadToEnd())" }
        $offset += $read.Result
    }
    if ([BitConverter]::ToUInt32($hello,0) -ne 1 -or [BitConverter]::ToUInt32($hello,4) -ne 0) { throw 'Invalid tunnel readiness record' }
    $adapter = Get-NetAdapter -Name $name
    $addresses = @(Get-NetIPAddress -InterfaceAlias $name -AddressFamily IPv6 | Where-Object IPAddress -Like 'fe80:*')
    if ($addresses.Count -ne 1 -or ($addresses[0].IPAddress -split '%')[0] -ne 'fe80::200:ff:fe00:1') {
        throw "The tunnel must expose only its configured link-local address so discovery selects it: $($addresses.IPAddress -join ',')"
    }
    $local = [Net.IPAddress]::Parse("fe80::200:ff:fe00:1%$($adapter.InterfaceIndex)")
    $remote = [Net.IPAddress]::Parse("fe80::2%$($adapter.InterfaceIndex)")
    $socket = New-Object Net.Sockets.UdpClient([Net.Sockets.AddressFamily]::InterNetworkV6)
    $socket.Client.Bind((New-Object Net.IPEndPoint($local,0)))
    $destination = New-Object Net.IPEndPoint($remote,49161)
    $sent = [Text.Encoding]::ASCII.GetBytes('windrop-outgoing-packet')
    $socket.Send($sent,$sent.Length,$destination) | Out-Null
    $found = $false
    for ($i=0; $i -lt 64; $i++) {
        $header = Read-Bytes 8
        $length = [BitConverter]::ToUInt32($header,4)
        if ([BitConverter]::ToUInt32($header,0) -ne 2 -or $length -gt 32768) { throw 'Invalid packet record' }
        $packet = Read-Bytes $length
        if ($packet.Length -ge 48 -and $packet[6] -eq 17 -and $packet[42] -eq 192 -and $packet[43] -eq 9) {
            if ([Convert]::ToBase64String($packet[48..($packet.Length-1)]) -ne [Convert]::ToBase64String($sent)) { throw 'Outgoing IPv6 packet payload changed' }
            $found = $true
            break
        }
    }
    if (-not $found) { throw 'No outbound UDP packet reached the tunnel CLI' }
    Write-Host 'PASS: Windows UDP reaches tunnel stdout with exact payload bytes.'
    # An independent UDP/IPv6 reply, including the RFC 8200 pseudo-header
    # checksum, exercises packet injection into the real Windows IP stack.
    $expected = [Text.Encoding]::ASCII.GetBytes('windrop-incoming-packet')
    $port = ([Net.IPEndPoint]$socket.Client.LocalEndPoint).Port
    $udpLength = 8 + $expected.Length
    [byte[]]$udp = @(192,9,($port -shr 8),($port -band 255),($udpLength -shr 8),($udpLength -band 255),0,0) + $expected
    [byte[]]$pseudo = $remote.GetAddressBytes() + $local.GetAddressBytes() + @(0,0,($udpLength -shr 8),($udpLength -band 255),0,0,0,17) + $udp
    [uint32]$sum = 0
    for ($i=0; $i -lt $pseudo.Length; $i+=2) {
        $word = [uint32]$pseudo[$i] -shl 8
        if ($i+1 -lt $pseudo.Length) { $word += $pseudo[$i+1] }
        $sum += $word
    }
    while ($sum -gt 65535) { $sum = ($sum -band 65535) + ($sum -shr 16) }
    $checksum = (-bnot $sum) -band 65535
    if ($checksum -eq 0) { $checksum = 65535 }
    $udp[6] = $checksum -shr 8; $udp[7] = $checksum -band 255
    [byte[]]$reply = @(96,0,0,0,($udpLength -shr 8),($udpLength -band 255),17,64) + $remote.GetAddressBytes() + $local.GetAddressBytes() + $udp
    $receive = $socket.ReceiveAsync()
    Write-Record 2 $reply
    if (-not $receive.Wait(5000) -or [Convert]::ToBase64String($receive.Result.Buffer) -ne [Convert]::ToBase64String($expected)) {
        throw 'Injected tunnel packet did not reach Windows UDP with exact bytes'
    }
    Write-Host 'PASS: tunnel stdin reaches Windows UDP with exact payload bytes.'
    $payload = New-Object byte[] 1000
    # Do not drain stdout: fill its pipe while the virtual adapter keeps sending.
    for ($i=0; $i -lt 1000; $i++) { $socket.Send($payload,$payload.Length,$destination) | Out-Null }
    Start-Sleep -Milliseconds 250
    if ($Shutdown -eq 'Stop') {
        Write-Record 5 @()
    }
    $process.StandardInput.Close()
    if (-not $process.WaitForExit(5000)) { throw 'Tunnel Stop/EOF hung behind an undrained output pipe' }
    if ($process.ExitCode -ne 0) { throw "Tunnel failed: $($process.StandardError.ReadToEnd())" }
    Write-Host "PASS: public tunnel $Shutdown exits with an undrained, saturated output pipe."
} finally {
    if ($socket) { $socket.Dispose() }
    if ($started -and -not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
    $process.Dispose()
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while (Get-NetAdapter -Name $name -ErrorAction SilentlyContinue) {
        if ([DateTime]::UtcNow -gt $deadline) { throw "Owned test adapter $name was not removed" }
        Start-Sleep -Milliseconds 100
    }
}
