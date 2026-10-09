using System.Net;
using System.Net.Sockets;
using System.Runtime.CompilerServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading.Channels;
using WinDrop.Protocol.Dns;

namespace WinDrop.Protocol.Discovery;

public sealed class BridgeException(string message) : Exception(message);

/// <summary>
/// Authenticated discovery and raw TCP relay to an OWL radio. Each control session
/// owns one radio and returns a random session capability for data and reverse streams.
/// AirDrop TLS stays in the caller. The radio access key is injected by configuration.
/// Loopback integration proves relay behavior; real AWDL and Apple interoperability
/// require hardware qualification.
/// </summary>
public sealed class BridgeTransport : IAirDropTransport
{
    public const int DefaultControlPort = 7788;
    public const int DefaultDataPort = 7789;

    private readonly string _host;
    private readonly string _key;
    private string _session = "";
    private readonly int _controlPort;
    private readonly int _dataPort;

    private readonly Channel<AirDropPeer> _discovered = Channel.CreateBounded<AirDropPeer>(new BoundedChannelOptions(256) { FullMode = BoundedChannelFullMode.DropOldest });
    private readonly Channel<Stream> _inbound = Channel.CreateBounded<Stream>(4);
    private readonly ServiceAssembler _assembler = new("bridge");
    private readonly CancellationTokenSource _shutdown = new();
    private readonly SemaphoreSlim _startGate = new(1, 1);
    private readonly SemaphoreSlim _writeGate = new(1, 1);

    private TcpClient? _control;
    private StreamWriter? _controlWriter;
    private TcpListener? _reverse;
    private AirDropServiceRecord? _advertised;
    private string _bridgeAddress = "";
    private string _bridgeInterface = "";
    private bool _started;
    private Task? _controlLoop, _reverseLoop, _announcementLoop;

    public BridgeTransport(string host, string key, int controlPort = DefaultControlPort, int dataPort = DefaultDataPort)
    {
        if (string.IsNullOrWhiteSpace(host)) throw new ArgumentException("A radio host is required.");
        if (key.Length < 32) throw new ArgumentException("Set WINDROP_RADIO_KEY to a random key of at least 32 characters.");
        _host = host;
        _key = key;
        _controlPort = controlPort;
        _dataPort = dataPort;
    }

    public string Name => "bridge";

    /// <summary>
    /// The only transport that can. Whether a current iPhone still *accepts* one is a
    /// separate and unresolved question — see the addendum in ADR-001.
    /// </summary>
    public bool CanReachAppleDevices => true;

    /// <summary>The link-local address the bridge holds on its AWDL interface.</summary>
    public string BridgeAddress => _bridgeAddress;

    public string BridgeInterface => _bridgeInterface;

    public Task ProbeAsync(CancellationToken ct = default) => EnsureStartedAsync(ct);

    // ---- connection --------------------------------------------------------

    private async Task EnsureStartedAsync(CancellationToken ct)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct, _shutdown.Token);
        timeout.CancelAfter(TimeSpan.FromSeconds(5));
        ct = timeout.Token;
        await _startGate.WaitAsync(ct);

        try
        {
            if (_started) return;

            // Bind the reverse listener before saying hello: the bridge needs somewhere
            // to hand inbound connections, and it needs to know that port up front.
            // Any, not Loopback: the bridge is a different machine and has to be
            // able to dial back. Dual-mode so it can reach us over either family.
            _reverse = new TcpListener(IPAddress.IPv6Any, 0);
            _reverse.Server.SetSocketOption(SocketOptionLevel.IPv6, SocketOptionName.IPv6Only, false);
            _reverse.Start();
            int reversePort = ((IPEndPoint)_reverse.LocalEndpoint).Port;

            _control = new TcpClient();
            await _control.ConnectAsync(_host, _controlPort, ct);

            NetworkStream stream = _control.GetStream();
            _controlWriter = new StreamWriter(stream, new UTF8Encoding(false)) { AutoFlush = true };

            await SendAsync(new JsonObject { ["op"] = "hello", ["reversePort"] = reversePort, ["token"] = _key }, ct);

            string? line = await ReadLineAsync(stream, ct)
                ?? throw new BridgeException("Bridge closed the control connection during the handshake.");

            JsonNode hello = JsonNode.Parse(line)
                ?? throw new BridgeException($"Unparseable handshake reply: {Truncate(line)}");

            if (hello["ok"]?.GetValue<bool>() != true)
                throw new BridgeException($"Radio refused the handshake: {hello["error"]?.GetValue<string>() ?? "invalid reply"}");

            _bridgeInterface = hello["interface"]?.GetValue<string>() ?? "";
            _bridgeAddress = hello["address"]?.GetValue<string>() ?? "";
            _session = hello["session"]?.GetValue<string>() ?? "";
            if (_session.Length < 32) throw new BridgeException("Radio did not return an authenticated session.");

            if (string.IsNullOrEmpty(_bridgeAddress))
            {
                throw new BridgeException(
                    $"Bridge reported no address on '{_bridgeInterface}'. Is OWL running and has awdl0 come up?");
            }

            _controlLoop = ControlLoopAsync(stream, _shutdown.Token);
            _reverseLoop = ReverseLoopAsync(_reverse, _shutdown.Token);

            _started = true;
        }
        finally
        {
            _startGate.Release();
        }
    }

    private async Task SendAsync(JsonObject message, CancellationToken ct)
    {
        if (_controlWriter is null) throw new BridgeException("Control connection is not open.");

        await _writeGate.WaitAsync(ct);
        try { await _controlWriter.WriteLineAsync(message.ToJsonString().AsMemory(), ct); }
        finally { _writeGate.Release(); }
    }

    private async Task ControlLoopAsync(Stream reader, CancellationToken ct)
    {
        try
        {
            while (!ct.IsCancellationRequested)
            {
                string? line = await ReadLineAsync(reader, ct);
                if (line is null) break;

                JsonNode? message;

                try
                {
                    message = JsonNode.Parse(line);
                }
                catch (JsonException)
                {
                    continue; // a malformed line is not worth tearing the link down for
                }

                if (message?["event"]?.GetValue<string>() != "mdns") continue;

                string? encoded = message["data"]?.GetValue<string>();
                if (encoded is null) continue;

                HandleDatagram(Convert.FromBase64String(encoded));
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception e) when (e is IOException or FormatException or InvalidOperationException) { }
        finally
        {
            _discovered.Writer.TryComplete(new BridgeException("Radio disconnected. Reconnect to resume discovery."));
            _inbound.Writer.TryComplete(new BridgeException("Radio disconnected. Reconnect to resume receiving."));
        }
    }

    private void HandleDatagram(byte[] payload)
    {
        DnsMessage message;

        try
        {
            message = DnsMessage.Parse(payload);
        }
        catch (DnsFormatException)
        {
            return; // the AWDL link carries other mDNS traffic too
        }

        AirDropServiceRecord? advertised = _advertised;

        // Browse questions, and questions about this receiver's own records, get the whole
        // set: refreshing and resolving need answers too, or the entry expires on the peer.
        if (AirDropRecords.IsQueryForService(message)
            || (advertised is not null && AirDropRecords.AsksAboutInstance(message, advertised, HostFor(advertised))))
        {
            if (advertised is not null) _ = AnnounceAsync(advertised, CancellationToken.None);
            return;
        }

        // Interface index 0: the address is scoped to the bridge's interface, not to
        // anything on this machine, and we never dial it directly — the bridge does.
        foreach (AirDropPeer peer in _assembler.Absorb(message, 0))
            _discovered.Writer.TryWrite(peer);
    }

    private async Task ReverseLoopAsync(TcpListener listener, CancellationToken ct)
    {
        try
        {
            while (!ct.IsCancellationRequested)
            {
                TcpClient client = await listener.AcceptTcpClientAsync(ct);
                NetworkStream stream = client.GetStream();

                // The bridge announces which peer this is before handing over the bytes.
                using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
                timeout.CancelAfter(TimeSpan.FromSeconds(5));
                try
                {
                    string? header = await ReadLineAsync(stream, timeout.Token);
                    if (header is null || JsonNode.Parse(header)?["session"]?.GetValue<string>() != _session)
                    { client.Dispose(); continue; }
                    if (!_inbound.Writer.TryWrite(stream)) client.Dispose();
                }
                catch (Exception e) when (e is IOException or OperationCanceledException or JsonException or InvalidOperationException)
                { client.Dispose(); }
            }
        }
        catch (OperationCanceledException) { }
        catch (SocketException) { }
        catch (ObjectDisposedException) { }
    }

    // ---- advertising and browsing ------------------------------------------

    public async Task<IAsyncDisposable> AdvertiseAsync(AirDropServiceRecord record, CancellationToken ct = default)
    {
        await EnsureStartedAsync(ct);

        _advertised = record;
        _assembler.OwnInstance = record.InstanceName;
        _announcementLoop ??= RefreshAnnouncementAsync(_shutdown.Token);

        // Twice, a second apart, as RFC 6762 (section 8.3) requires: multicast is never
        // acknowledged, and a single announcement is easily lost on awdl0.
        for (int attempt = 0; attempt < 2; attempt++)
        {
            if (attempt > 0) await Task.Delay(TimeSpan.FromSeconds(1), ct);
            await AnnounceAsync(record, ct);
        }

        return new Advertisement(this, record);
    }

    private async Task RefreshAnnouncementAsync(CancellationToken ct)
    {
        try
        {
            while (true)
            {
                await Task.Delay(TimeSpan.FromSeconds(30), ct);
                if (_advertised is { } record) await AnnounceAsync(record, ct);
            }
        }
        catch (Exception e) when (e is OperationCanceledException or IOException or ObjectDisposedException) { }
    }

    private Task AnnounceAsync(AirDropServiceRecord record, CancellationToken ct)
    {
        // The host name and address are the bridge's, because the bridge is what a peer
        // will actually connect to. It then pipes those bytes here.
        IPAddress address = IPAddress.Parse(StripScope(_bridgeAddress));

        DnsMessage announcement = AirDropRecords.BuildAnnouncement(record, HostFor(record), [address]);
        return SendDatagramAsync(announcement, ct);
    }

    private static string HostFor(AirDropServiceRecord record) => $"{record.InstanceName.ToLowerInvariant()}.local";

    private sealed class Advertisement(BridgeTransport owner, AirDropServiceRecord record) : IAsyncDisposable
    {
        public async ValueTask DisposeAsync()
        {
            // Stop answering first, so no query can be answered after the goodbye.
            owner._advertised = null;

            await AirDropRecords.SendGoodbyeAsync(
                AirDropRecords.BuildGoodbye(record, HostFor(record)),
                owner.SendDatagramAsync);
        }
    }

    private Task SendDatagramAsync(DnsMessage message, CancellationToken ct) =>
        SendAsync(new JsonObject
        {
            ["op"] = "mdns",
            ["data"] = Convert.ToBase64String(message.ToBytes()),
        }, ct);

    public async IAsyncEnumerable<AirDropPeer> BrowseAsync([EnumeratorCancellation] CancellationToken ct = default)
    {
        await EnsureStartedAsync(ct);

        _ = Task.Run(async () =>
        {
            while (!ct.IsCancellationRequested)
            {
                try
                {
                    await SendDatagramAsync(AirDropRecords.BuildQuery(), ct);
                    await Task.Delay(TimeSpan.FromSeconds(3), ct);
                }
                catch (OperationCanceledException) { return; }
                catch (Exception) { return; }
            }
        }, ct);

        await foreach (AirDropPeer peer in _discovered.Reader.ReadAllAsync(ct))
            yield return peer;
    }

    // ---- connections -------------------------------------------------------

    public async Task<Stream> ConnectAsync(AirDropPeer peer, CancellationToken ct = default)
    {
        await EnsureStartedAsync(ct);

        var client = new TcpClient();

        try
        {
            await client.ConnectAsync(_host, _dataPort, ct);
            NetworkStream stream = client.GetStream();

            // Send the address without a scope suffix. Any scope in it belongs to the
            // bridge's interface numbering, not ours, so the bridge re-attaches its own.
            var request = new JsonObject
            {
                ["connect"] = StripScope(peer.EndPoint.Address.ToString()),
                ["port"] = peer.EndPoint.Port,
                ["session"] = _session,
            };

            byte[] line = Encoding.UTF8.GetBytes(request.ToJsonString() + "\n");
            await stream.WriteAsync(line, ct);
            await stream.FlushAsync(ct);

            string? reply = await ReadLineAsync(stream, ct)
                ?? throw new BridgeException("Bridge closed the data connection without replying.");

            JsonNode? parsed = JsonNode.Parse(reply);

            if (parsed?["ok"]?.GetValue<bool>() != true)
            {
                throw new BridgeException(
                    $"Bridge could not reach {peer.EndPoint}: {parsed?["error"]?.GetValue<string>() ?? Truncate(reply)}");
            }

            return stream;
        }
        catch
        {
            client.Dispose();
            throw;
        }
    }

    /// <summary>
    /// Inbound transfers. The port is ignored: the bridge listens on the AirDrop port on
    /// its own AWDL interface, because that is the address a peer was told to use, and
    /// dials back here for each connection.
    /// </summary>
    public async IAsyncEnumerable<Stream> AcceptAsync(
        int port,
        [EnumeratorCancellation] CancellationToken ct = default)
    {
        await EnsureStartedAsync(ct);

        await foreach (Stream stream in _inbound.Reader.ReadAllAsync(ct))
            yield return stream;
    }

    // ---- helpers -----------------------------------------------------------

    /// <summary>Reads one newline-terminated line without buffering past it.</summary>
    private static async Task<string?> ReadLineAsync(Stream stream, CancellationToken ct)
    {
        var bytes = new List<byte>(128);
        var one = new byte[1];

        while (bytes.Count < 16384)
        {
            int read = await stream.ReadAsync(one, ct);
            if (read == 0) return bytes.Count == 0 ? null : Encoding.UTF8.GetString(bytes.ToArray());
            if (one[0] == (byte)'\n') return Encoding.UTF8.GetString(bytes.ToArray());
            if (one[0] != (byte)'\r') bytes.Add(one[0]);
        }

        throw new BridgeException("Radio sent a line longer than 16384 bytes.");
    }

    private static string StripScope(string address)
    {
        int percent = address.IndexOf('%');
        return percent < 0 ? address : address[..percent];
    }

    private static string Truncate(string value) => value.Length <= 200 ? value : value[..200] + "...";

    public async ValueTask DisposeAsync()
    {
        await _shutdown.CancelAsync();

        try { _reverse?.Stop(); } catch { /* already gone */ }
        _control?.Dispose();
        foreach (var task in new[] { _controlLoop, _reverseLoop, _announcementLoop })
            if (task is not null) await task;
        while (_inbound.Reader.TryRead(out var stream)) await stream.DisposeAsync();
        _shutdown.Dispose();
        _startGate.Dispose();
        _writeGate.Dispose();
    }
}

