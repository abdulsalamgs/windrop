using System.Net;
using System.Net.Sockets;
using System.Text;
using Windrop.Radio;
using WinDrop.Protocol.Discovery;
using WinDrop.Protocol;
using WinDrop.Protocol.Tls;

namespace WinDrop.Protocol.Tests;

public class RadioTests
{
    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task Incoming_transfer_through_radio_requires_consent_and_preserves_bytes(bool consent)
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        string directory = Path.Combine(Path.GetTempPath(), "windrop-radio-test-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        string source = Path.Combine(directory, "sample.txt");
        byte[] expected = Encoding.UTF8.GetBytes("AirDrop radio fixture.\nExact bytes: 0123456789\n");
        await File.WriteAllBytesAsync(source, expected, timeout.Token);
        string downloads = Path.Combine(directory, "downloads");
        Directory.CreateDirectory(downloads);
        try
        {
            string key = new('b', 64);
            await using var radio = new RadioServer("lo", IPAddress.IPv6Loopback, IPAddress.Loopback, key, 0, 0, 0, loopbackTest: true);
            await radio.StartAsync(timeout.Token);
            await using var transport = new BridgeTransport("127.0.0.1", key, radio.ControlPort, radio.DataPort);
            await transport.ProbeAsync(timeout.Token);
            using var certificate = AirDropCertificate.CreateSelfSigned("radio-test");
            bool asked = false;
            var receiver = new AirDropReceiver(new AirDropReceiverOptions
            {
                DownloadDirectory = downloads,
                EarlyAskReply = false,
                ConsentHandler = (_, _) => { asked = true; return Task.FromResult(consent); }
            });
            var receiving = Task.Run(async () =>
            {
                await using var accepted = transport.AcceptAsync(8770, timeout.Token).GetAsyncEnumerator(timeout.Token);
                Assert.True(await accepted.MoveNextAsync());
                await using var stream = accepted.Current;
                await using var tls = await AirDropTls.AuthenticateAsServerAsync(stream, certificate, timeout.Token);
                return await receiver.HandleConnectionAsync(tls, timeout.Token);
            }, timeout.Token);
            using (var apple = new TcpClient(AddressFamily.InterNetworkV6))
            {
                await apple.ConnectAsync(IPAddress.IPv6Loopback, radio.AirDropPort, timeout.Token);
                await using var tls = await AirDropTls.AuthenticateAsClientAsync(apple.GetStream(), certificate, timeout.Token);
                await using var sender = new AirDropSenderSession(tls, AirDropReceiverFlags.SupportsDvZip);
                var file = AirDropOutgoingFile.FromPath(source);
                await sender.DiscoverAsync(timeout.Token);
                bool accepted = await sender.AskAsync(new AirDropAskRequest("Apple fixture", "iPhone", "test", AirDropAskRequest.FinderBundleId, [file.ToEntry()]), timeout.Token);
                Assert.Equal(consent, accepted);
                if (accepted) await sender.UploadAsync([file], ct: timeout.Token);
            }
            await receiving.WaitAsync(timeout.Token);
            Assert.True(asked);
            if (consent) Assert.Equal(expected, await File.ReadAllBytesAsync(Path.Combine(downloads, "sample.txt"), timeout.Token));
            else Assert.Empty(Directory.EnumerateFileSystemEntries(downloads));
        }
        finally { Directory.Delete(directory, recursive: true); }
    }

    [Fact]
    public async Task Radio_ownership_is_released_when_discovery_stops()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        string key = new('a', 64);
        await using var radio = new RadioServer("lo", IPAddress.IPv6Loopback, IPAddress.Loopback,
            key, 0, 0, 0, loopbackTest: true);
        await radio.StartAsync(timeout.Token);
        var record = new AirDropServiceRecord("windrop-test", 8770, AirDropReceiverFlags.SupportsDvZip);
        await using (var transport = new BridgeTransport("127.0.0.1", key, radio.ControlPort, radio.DataPort))
        {
            await using var advertisement = await transport.AdvertiseAsync(record, timeout.Token);
            await using var other = new BridgeTransport("127.0.0.1", key, radio.ControlPort, radio.DataPort);
            await Assert.ThrowsAsync<BridgeException>(() => other.AdvertiseAsync(record, timeout.Token));
        }
        // A fresh controller must become discoverable after the first disconnects.
        await Task.Delay(100, timeout.Token);
        await using var restarted = new BridgeTransport("127.0.0.1", key, radio.ControlPort, radio.DataPort);
        await using var again = await restarted.AdvertiseAsync(record, timeout.Token);
        Assert.Equal("lo", restarted.BridgeInterface);
    }

    [Fact]
    public async Task Radio_refuses_an_unauthenticated_controller()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        await using var radio = new RadioServer("lo", IPAddress.IPv6Loopback, IPAddress.Loopback,
            new string('a', 64), 0, 0, 0, loopbackTest: true);
        await radio.StartAsync(timeout.Token);
        using var client = new TcpClient();
        await client.ConnectAsync(IPAddress.Loopback, radio.ControlPort, timeout.Token);
        await client.GetStream().WriteAsync(Encoding.UTF8.GetBytes("{\"op\":\"hello\",\"token\":\"wrong\",\"reversePort\":1234}\n"), timeout.Token);
        using var reader = new StreamReader(client.GetStream());
        Assert.Contains("unauthorized", await reader.ReadLineAsync(timeout.Token));
    }
}
