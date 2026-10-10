using System.Net;
using System.Net.Sockets;
using System.Text;
using Windrop.Radio;
using WinDrop.Protocol.Discovery;
using WinDrop.Protocol;
using WinDrop.Protocol.Tls;
using System.Text.Json.Nodes;
using WinDrop.Protocol.Dns;

namespace WinDrop.Protocol.Tests;

public class RadioTests
{
    [Fact]
    public async Task Outgoing_transfer_through_authenticated_radio_delivers_original_bytes()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        string directory = Path.Combine(Path.GetTempPath(), "windrop-outgoing-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        var listener = new TcpListener(IPAddress.IPv6Loopback, 0);
        listener.Start();
        try
        {
            byte[] expected = "Outgoing radio transfer: original bytes.\n"u8.ToArray();
            string source = Path.Combine(directory, "fixture.txt");
            await File.WriteAllBytesAsync(source, expected, timeout.Token);
            string downloads = Path.Combine(directory, "received");
            string key = new('d', 64);
            await using var radio = new RadioServer("lo", IPAddress.IPv6Loopback, IPAddress.Loopback, key, 0, 0, 0, loopbackTest: true);
            await radio.StartAsync(timeout.Token);
            await using var transport = new BridgeTransport("127.0.0.1", key, radio.ControlPort, radio.DataPort);
            using var certificate = AirDropCertificate.CreateSelfSigned("fixture-peer");
            var receiver = new AirDropReceiver(new AirDropReceiverOptions { DownloadDirectory = downloads, ConsentHandler = (_, _) => Task.FromResult(true) });
            var receiving = Task.Run(async () =>
            {
                using var client = await listener.AcceptTcpClientAsync(timeout.Token);
                await using var tls = await AirDropTls.AuthenticateAsServerAsync(client.GetStream(), certificate, timeout.Token);
                return await receiver.HandleConnectionAsync(tls, timeout.Token);
            }, timeout.Token);
            var target = new AirDropPeer("fixture-peer", (IPEndPoint)listener.LocalEndpoint, AirDropReceiverFlags.SupportsDvZip, "bridge");
            await using (var raw = await transport.ConnectAsync(target, timeout.Token))
            await using (var tls = await AirDropTls.AuthenticateAsClientAsync(raw, certificate, timeout.Token))
            await using (var sender = new AirDropSenderSession(tls, target.Flags))
            {
                var file = AirDropOutgoingFile.FromPath(source);
                await sender.DiscoverAsync(timeout.Token);
                Assert.True(await sender.AskAsync(new AirDropAskRequest("fixture", "Windows", "test", AirDropAskRequest.FinderBundleId, [file.ToEntry()]), timeout.Token));
                await sender.UploadAsync([file], ct: timeout.Token);
            }
            await receiving.WaitAsync(timeout.Token);
            Assert.Equal(expected, await File.ReadAllBytesAsync(Path.Combine(downloads, "fixture.txt"), timeout.Token));
        }
        finally { listener.Stop(); Directory.Delete(directory, recursive: true); }
    }

    [Fact]
    public async Task Authenticated_controller_cannot_use_the_radio_as_an_arbitrary_network_proxy()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        string key = new('e', 64);
        await using var radio = new RadioServer("lo", IPAddress.IPv6Loopback, IPAddress.Loopback, key, 0, 0, 0, loopbackTest: true);
        await radio.StartAsync(timeout.Token);
        await using var transport = new BridgeTransport("127.0.0.1", key, radio.ControlPort, radio.DataPort);
        var target = new AirDropPeer("forbidden", new IPEndPoint(IPAddress.Parse("192.0.2.1"), 80), AirDropReceiverFlags.None, "bridge");
        var error = await Assert.ThrowsAsync<BridgeException>(() => transport.ConnectAsync(target, timeout.Token));
        Assert.Contains("destination must be on AWDL", error.Message);
    }

    [Fact]
    public async Task Discovery_survives_controller_resets_and_live_packets_during_handshake()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        string key = new('c', 64);
        await using var radio = new RadioServer("lo", IPAddress.IPv6Loopback, IPAddress.Loopback, key, 0, 0, 0, loopbackTest: true);
        await radio.StartAsync(timeout.Token);
        var message = AirDropRecords.BuildAnnouncement(new AirDropServiceRecord("fixture-phone", 8770, AirDropReceiverFlags.SupportsDvZip), "fixture.local", [IPAddress.IPv6Loopback]);
        message.Additionals.Add(new TxtRecord("fixture-phone." + AirDropServiceRecord.ServiceType,
            new Dictionary<string, string> { ["padding"] = new string('x', 200), ["flags"] = "2" }));
        byte[] announcement = message.ToBytes();
        using var udp = new UdpClient(AddressFamily.InterNetworkV6);
        using var flood = CancellationTokenSource.CreateLinkedTokenSource(timeout.Token);
        var packets = Task.Run(async () =>
        {
            try
            {
                while (!flood.IsCancellationRequested)
                {
                    await udp.SendAsync(announcement, new IPEndPoint(IPAddress.IPv6Loopback, radio.MdnsPort), flood.Token);
                    await Task.Delay(1, flood.Token);
                }
            }
            catch (OperationCanceledException) { }
        }, timeout.Token);
        try
        {
            for (int attempt = 0; attempt < 30; attempt++)
            {
                using var client = new TcpClient { ReceiveBufferSize = 1024 };
                await client.ConnectAsync(IPAddress.Loopback, radio.ControlPort, timeout.Token);
                await client.GetStream().WriteAsync(Encoding.UTF8.GetBytes(new JsonObject
                    { ["op"] = "hello", ["token"] = key, ["reversePort"] = 1234 }.ToJsonString() + "\n"), timeout.Token);
                using var reader = new StreamReader(client.GetStream());
                var reply = JsonNode.Parse((await reader.ReadLineAsync(timeout.Token))!);
                Assert.True(reply?["ok"]?.GetValue<bool>() == true,
                    $"A hello reply must precede discovery events. Attempt {attempt}; error={reply?["error"]}; event={reply?["event"]}.");
                // Let the real discovery writer fill this slow controller's receive window.
                await Task.Delay(80, timeout.Token);
                client.Client.LingerState = new LingerOption(true, 0);
                client.Close();
                await Task.Delay(10, timeout.Token);
            }
            await Task.Delay(100, timeout.Token);
            await using var restarted = new BridgeTransport("127.0.0.1", key, radio.ControlPort, radio.DataPort);
            await using var browse = restarted.BrowseAsync(timeout.Token).GetAsyncEnumerator(timeout.Token);
            Assert.True(await browse.MoveNextAsync());
            Assert.Equal("fixture-phone", browse.Current.InstanceName);
        }
        finally { await flood.CancelAsync(); await packets; }
    }

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
