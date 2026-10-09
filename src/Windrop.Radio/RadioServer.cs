using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using System.Collections.Concurrent;
using WinDrop.Protocol.Discovery;

namespace Windrop.Radio;

/// <summary>An authenticated byte relay. AirDrop TLS terminates in the Windows app.</summary>
public sealed class RadioServer : IAsyncDisposable
{
    private readonly string _interface;
    private readonly IPAddress _address;
    private readonly byte[] _key;
    private readonly bool _loopbackTest;
    private readonly TcpListener _control, _data, _airdrop;
    private readonly CancellationTokenSource _shutdown = new();
    private readonly List<Task> _loops = [];
    private readonly object _gate = new();
    private Controller? _controller;
    private readonly SemaphoreSlim _slots = new(32, 32);
    private readonly ConcurrentDictionary<TcpClient, byte> _clients = new();
    private UdpClient? _mdns;

    private sealed class Controller(TcpClient client, int reversePort, string session)
    {
        public TcpClient Client { get; } = client;
        public int ReversePort { get; } = reversePort;
        public string Session { get; } = session;
        public SemaphoreSlim WriteGate { get; } = new(1, 1);
        public CancellationTokenSource Lifetime { get; } = new();
    }

    public RadioServer(string interfaceName, IPAddress radioAddress, IPAddress listenAddress,
        string key, int controlPort = 7788, int dataPort = 7789, int airdropPort = 8770,
        bool loopbackTest = false)
    {
        if (key.Length < 32) throw new ArgumentException("Radio key must contain at least 32 characters.");
        if (!radioAddress.IsIPv6LinkLocal && !(loopbackTest && IPAddress.IsLoopback(radioAddress)))
            throw new ArgumentException("The radio must have an IPv6 link-local address.");
        _interface = interfaceName;
        _address = radioAddress;
        _key = Encoding.UTF8.GetBytes(key);
        _loopbackTest = loopbackTest;
        _control = new TcpListener(listenAddress, controlPort);
        _data = new TcpListener(listenAddress, dataPort);
        _airdrop = new TcpListener(radioAddress, airdropPort);
    }

    public int ControlPort => ((IPEndPoint)_control.LocalEndpoint).Port;
    public int DataPort => ((IPEndPoint)_data.LocalEndpoint).Port;
    public int AirDropPort => ((IPEndPoint)_airdrop.LocalEndpoint).Port;

    public Task StartAsync(CancellationToken ct = default)
    {
        ct.ThrowIfCancellationRequested();
        try
        {
            _control.Start(); _data.Start(); _airdrop.Start();
            if (!_loopbackTest)
            {
                _mdns = new UdpClient(AddressFamily.InterNetworkV6);
                _mdns.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
                _mdns.Client.Bind(new IPEndPoint(IPAddress.IPv6Any, 5353));
                _mdns.JoinMulticastGroup((int)_address.ScopeId, IPAddress.Parse("ff02::fb"));
                _mdns.Client.SetSocketOption(SocketOptionLevel.IPv6, SocketOptionName.MulticastInterface, (int)_address.ScopeId);
                _loops.Add(MdnsLoopAsync(_shutdown.Token));
            }
            _loops.Add(AcceptLoopAsync(_control, HandleControlAsync));
            _loops.Add(AcceptLoopAsync(_data, HandleDataAsync));
            _loops.Add(AcceptLoopAsync(_airdrop, HandleInboundAsync));
            return Task.CompletedTask;
        }
        catch
        {
            _control.Stop(); _data.Stop(); _airdrop.Stop(); _mdns?.Dispose();
            throw;
        }
    }

    private async Task AcceptLoopAsync(TcpListener listener, Func<TcpClient, Task> handle)
    {
        try
        {
            while (!_shutdown.IsCancellationRequested)
            {
                var client = await listener.AcceptTcpClientAsync(_shutdown.Token);
                if (!_slots.Wait(0)) { client.Dispose(); continue; }
                _clients.TryAdd(client, 0);
                _ = RunClientAsync(client, handle);
            }
        }
        catch (Exception e) when (e is OperationCanceledException or SocketException or ObjectDisposedException) { }
    }

    private async Task RunClientAsync(TcpClient client, Func<TcpClient, Task> handle)
    {
        using (client)
        {
            try { await handle(client); }
            catch (Exception e) when (e is IOException or SocketException or OperationCanceledException
                or System.Text.Json.JsonException or FormatException or InvalidOperationException or ArgumentException or ObjectDisposedException) { }
            finally { _clients.TryRemove(client, out _); _slots.Release(); }
        }
    }

    private async Task HandleControlAsync(TcpClient client)
    {
        using var handshake = CancellationTokenSource.CreateLinkedTokenSource(_shutdown.Token);
        handshake.CancelAfter(TimeSpan.FromSeconds(5));
        var stream = client.GetStream();
        var hello = await ReadMessageAsync(stream, handshake.Token);
        if (hello?["op"]?.GetValue<string>() != "hello" || !HasKey(hello?["token"]?.GetValue<string>()))
        {
            await WriteMessageAsync(stream, new JsonObject { ["ok"] = false, ["error"] = "unauthorized" }, handshake.Token);
            return;
        }
        int port = hello?["reversePort"]?.GetValue<int>() ?? 0;
        if (port is < 1 or > 65535) return;
        var controller = new Controller(client, port, Convert.ToHexString(RandomNumberGenerator.GetBytes(32)));
        lock (_gate)
        {
            if (_controller is not null) controller = null!;
            else _controller = controller;
        }
        if (controller is null)
        {
            await WriteMessageAsync(stream, new JsonObject { ["ok"] = false, ["error"] = "radio busy" }, handshake.Token);
            return;
        }
        try
        {
            await WriteMessageAsync(stream, new JsonObject
            {
                ["ok"] = true, ["interface"] = _interface, ["address"] = _address.ToString(),
                ["dataPort"] = DataPort, ["airdropPort"] = AirDropPort, ["session"] = controller.Session
            }, handshake.Token);
            while (!_shutdown.IsCancellationRequested)
            {
                var message = await ReadMessageAsync(stream, _shutdown.Token);
                if (message is null) break;
                if (message["op"]?.GetValue<string>() != "mdns") continue;
                byte[] payload = Convert.FromBase64String(message["data"]!.GetValue<string>());
                if (payload.Length > 9000) return;
                // In test mode, echo real DNS bytes through the control connection.
                if (_loopbackTest) await SendControlAsync(controller, payload);
                else await _mdns!.SendAsync(payload, new IPEndPoint(IPAddress.Parse("ff02::fb%" + _address.ScopeId), 5353), _shutdown.Token);
            }
        }
        finally
        {
            controller.Lifetime.Cancel();
            lock (_gate) { if (_controller == controller) _controller = null; }
        }
    }

    private bool HasKey(string? candidate) => candidate is not null &&
        CryptographicOperations.FixedTimeEquals(Encoding.UTF8.GetBytes(candidate), _key);

    private Controller? CurrentController() { lock (_gate) return _controller; }

    private async Task SendControlAsync(Controller owner, byte[] payload)
    {
        await owner.WriteGate.WaitAsync(_shutdown.Token);
        try
        {
            await WriteMessageAsync(owner.Client.GetStream(), new JsonObject
            { ["event"] = "mdns", ["data"] = Convert.ToBase64String(payload) }, _shutdown.Token);
        }
        finally { owner.WriteGate.Release(); }
    }

    private async Task MdnsLoopAsync(CancellationToken ct)
    {
        try
        {
            while (!ct.IsCancellationRequested)
            {
                var packet = await _mdns!.ReceiveAsync(ct);
                if (packet.RemoteEndPoint.Address.ScopeId != _address.ScopeId) continue;
                var owner = CurrentController();
                if (owner is not null && packet.Buffer.Length <= 9000)
                    await SendControlAsync(owner, packet.Buffer);
            }
        }
        catch (Exception e) when (e is OperationCanceledException or IOException or SocketException or ObjectDisposedException) { }
    }

    private async Task HandleDataAsync(TcpClient client)
    {
        using var handshake = CancellationTokenSource.CreateLinkedTokenSource(_shutdown.Token);
        handshake.CancelAfter(TimeSpan.FromSeconds(5));
        var request = await ReadMessageAsync(client.GetStream(), handshake.Token);
        var owner = CurrentController();
        if (owner is null || request?["session"]?.GetValue<string>() != owner.Session)
        {
            await WriteMessageAsync(client.GetStream(), new JsonObject { ["ok"] = false, ["error"] = "unauthorized" }, handshake.Token);
            return;
        }
        if (!IPAddress.TryParse(request?["connect"]?.GetValue<string>(), out var address)
            || !(address.IsIPv6LinkLocal || (_loopbackTest && IPAddress.IsLoopback(address))))
        {
            await WriteMessageAsync(client.GetStream(), new JsonObject { ["ok"] = false, ["error"] = "destination must be on AWDL" }, handshake.Token);
            return;
        }
        int port = request?["port"]?.GetValue<int>() ?? 0;
        if (port is < 1 or > 65535) return;
        if (address.IsIPv6LinkLocal) address.ScopeId = _address.ScopeId;
        using var remote = new TcpClient(address.AddressFamily);
        using var lifetime = CancellationTokenSource.CreateLinkedTokenSource(_shutdown.Token, owner.Lifetime.Token);
        using var connectTimeout = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token);
        connectTimeout.CancelAfter(TimeSpan.FromSeconds(15));
        try { await remote.ConnectAsync(address, port, connectTimeout.Token); }
        catch (SocketException)
        {
            await WriteMessageAsync(client.GetStream(), new JsonObject { ["ok"] = false, ["error"] = "peer unavailable" }, handshake.Token);
            return;
        }
        await WriteMessageAsync(client.GetStream(), new JsonObject { ["ok"] = true }, handshake.Token);
        await RelayAsync(client, remote, lifetime.Token);
    }

    private async Task HandleInboundAsync(TcpClient peer)
    {
        var owner = CurrentController();
        if (owner is null) return;
        var controllerAddress = ((IPEndPoint)owner.Client.Client.RemoteEndPoint!).Address;
        using var reverse = new TcpClient(controllerAddress.AddressFamily);
        using var lifetime = CancellationTokenSource.CreateLinkedTokenSource(_shutdown.Token, owner.Lifetime.Token);
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token);
        timeout.CancelAfter(TimeSpan.FromSeconds(5));
        await reverse.ConnectAsync(controllerAddress, owner.ReversePort, timeout.Token);
        await WriteMessageAsync(reverse.GetStream(), new JsonObject
        { ["peer"] = ((IPEndPoint)peer.Client.RemoteEndPoint!).Address.ToString(), ["session"] = owner.Session }, timeout.Token);
        await RelayAsync(peer, reverse, lifetime.Token);
    }

    private static async Task RelayAsync(TcpClient a, TcpClient b, CancellationToken ct)
    {
        using var lifetime = CancellationTokenSource.CreateLinkedTokenSource(ct);
        var first = a.GetStream().CopyToAsync(b.GetStream(), lifetime.Token);
        var second = b.GetStream().CopyToAsync(a.GetStream(), lifetime.Token);
        await Task.WhenAny(first, second);
        await lifetime.CancelAsync();
        a.Close(); b.Close();
        try { await Task.WhenAll(first, second); }
        catch (Exception e) when (e is IOException or OperationCanceledException or ObjectDisposedException) { }
    }

    private static async Task<JsonNode?> ReadMessageAsync(Stream stream, CancellationToken ct)
    {
        using var line = new MemoryStream();
        var one = new byte[1];
        while (line.Length < 16384)
        {
            if (await stream.ReadAsync(one, ct) == 0) return null;
            if (one[0] == '\n') return JsonNode.Parse(line.ToArray());
            line.WriteByte(one[0]);
        }
        throw new IOException("Radio message too large.");
    }

    private static ValueTask WriteMessageAsync(Stream stream, JsonObject message, CancellationToken ct) =>
        stream.WriteAsync(Encoding.UTF8.GetBytes(message.ToJsonString() + "\n"), ct);

    public async ValueTask DisposeAsync()
    {
        await _shutdown.CancelAsync();
        _control.Stop(); _data.Stop(); _airdrop.Stop(); _mdns?.Dispose();
        CurrentController()?.Client.Dispose();
        await Task.WhenAll(_loops);
        foreach (var client in _clients.Keys) client.Dispose();
        for (int i = 0; i < 32; i++) await _slots.WaitAsync();
        _slots.Dispose();
        _shutdown.Dispose();
    }
}
