using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography.X509Certificates;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using Microsoft.Win32;
using WinDrop.Protocol;
using WinDrop.Protocol.Discovery;
using WinDrop.Protocol.Tls;

namespace Windrop.App;

public partial class MainWindow : Window
{
    private sealed record PeerItem(AirDropPeer Peer)
    {
        public string DisplayName => PeerText.Printable(Peer.InstanceName);
        public DateTime Seen { get; set; } = DateTime.UtcNow;
    }
    private readonly ObservableCollection<PeerItem> _peers = [];
    private readonly List<string> _files = [];
    private readonly List<Task> _tasks = [];
    private readonly SemaphoreSlim _consentGate = new(1, 1);
    private readonly string _downloads = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads", "Windrop");
    private CancellationTokenSource? _session;
    private BridgeTransport? _transport;
    private IAsyncDisposable? _advertisement;
    private X509Certificate2? _certificate;
    private TaskCompletionSource<bool>? _consent;
    private bool _busy, _sending, _allowClose;

    public MainWindow()
    {
        InitializeComponent();
        HostInput.Text = RadioSettings.Host;
        KeyInput.Password = RadioSettings.Key;
        PeerList.ItemsSource = _peers;
        Loaded += async (_, _) =>
        {
            var args = Environment.GetCommandLineArgs();
            int capture = Array.IndexOf(args, "--capture");
            if (capture >= 0 && capture + 1 < args.Length)
            {
                await Task.Delay(200);
                Capture(args[capture + 1]);
                _allowClose = true; Close();
            }
        };
    }

    private async void ToggleConnection(object sender, RoutedEventArgs e)
    {
        if (_busy) return;
        _busy = true; ConnectButton.IsEnabled = false;
        try
        {
            if (_session is not null) { await DisconnectAsync(); return; }
            _session = new CancellationTokenSource();
            _transport = new BridgeTransport(HostInput.Text.Trim(), KeyInput.Password);
            await _transport.ProbeAsync(_session.Token);
            _certificate = AirDropCertificate.CreateSelfSigned("Windrop");
            var flags = AirDropReceiverFlags.SupportsDvZip | AirDropReceiverFlags.SupportsMixedTypes;
            var record = new AirDropServiceRecord($"windrop-{Guid.NewGuid():N}"[..20], 8770, flags);
            _advertisement = await _transport.AdvertiseAsync(record, _session.Token);
            _tasks.Add(BrowseAsync(_session.Token));
            _tasks.Add(ReceiveAsync(flags, _session.Token));
            _tasks.Add(ExpirePeersAsync(_session.Token));
            ConnectionBadge.Text = "Connected · discoverable";
            ReceiveHeading.Text = "Ready to receive";
            RadioMessage.Text = "Connected. Keep your Apple device nearby with AirDrop enabled.";
            ConnectButton.Content = "Disconnect radio";
            HostInput.IsEnabled = KeyInput.IsEnabled = false;
            StatusText.Text = "Looking for devices. Incoming transfers need your approval.";
        }
        catch (Exception ex)
        {
            await DisconnectAsync();
            RadioMessage.Text = ex is ArgumentException ? ex.Message : "Radio unavailable. Check its host and key, then start OWL and the Windrop radio bridge.";
        }
        finally { _busy = false; ConnectButton.IsEnabled = true; UpdateSendButton(); }
    }

    private async Task BrowseAsync(CancellationToken ct)
    {
        try
        {
            await foreach (var peer in _transport!.BrowseAsync(ct))
            {
                var existing = _peers.FirstOrDefault(p => p.Peer.InstanceName == peer.InstanceName);
                if (existing is not null) existing.Seen = DateTime.UtcNow;
                else _peers.Add(new PeerItem(peer));
                EmptyPeers.Visibility = _peers.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception) { ReportLostRadio(); }
    }

    private async Task ExpirePeersAsync(CancellationToken ct)
    {
        try
        {
            while (true)
            {
                await Task.Delay(TimeSpan.FromSeconds(10), ct);
                foreach (var peer in _peers.Where(p => DateTime.UtcNow - p.Seen > TimeSpan.FromSeconds(120)).ToArray()) _peers.Remove(peer);
                EmptyPeers.Visibility = _peers.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
            }
        }
        catch (OperationCanceledException) { }
    }

    private async Task ReceiveAsync(AirDropReceiverFlags flags, CancellationToken ct)
    {
        var receiver = new AirDropReceiver(new AirDropReceiverOptions
        { ComputerName = "Windrop", DownloadDirectory = _downloads, Flags = flags, ConsentHandler = AskConsentAsync });
        using var slots = new SemaphoreSlim(4, 4);
        var active = new List<Task>();
        try
        {
            await foreach (var stream in _transport!.AcceptAsync(8770, ct))
            {
                try { await slots.WaitAsync(ct); } catch { stream.Dispose(); throw; }
                active.RemoveAll(t => t.IsCompleted);
                active.Add(HandleAsync(stream));
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception) { ReportLostRadio(); }
        finally { await Task.WhenAll(active); }

        async Task HandleAsync(Stream stream)
        {
            try
            {
                await using (stream)
                {
                    await using var tls = await AirDropTls.AuthenticateAsServerAsync(stream, _certificate!, ct);
                    var result = await receiver.HandleConnectionAsync(tls, ct);
                    if (result is not null) await Dispatcher.InvokeAsync(() => StatusText.Text = $"Received {result.Files.Count} file(s). Open downloads to find them.");
                }
            }
            catch (OperationCanceledException) { }
            catch (Exception) { await Dispatcher.InvokeAsync(() => StatusText.Text = "Incoming transfer ended before completion. No partial files were saved."); }
            finally { slots.Release(); }
        }
    }

    private async Task<bool> AskConsentAsync(AirDropAskRequest request, CancellationToken ct)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
        timeout.CancelAfter(TimeSpan.FromSeconds(55));
        try
        {
            await _consentGate.WaitAsync(timeout.Token);
            try
            {
                _consent = new(TaskCreationOptions.RunContinuationsAsynchronously);
                ConsentText.Text = $"{PeerText.Printable(request.SenderComputerName)} wants to share {request.Files.Count} item(s):\n\n" +
                    string.Join("\n", request.Files.Take(8).Select(f => PeerText.Printable(f.FileName)));
                ConsentPanel.Visibility = Visibility.Visible;
                return await _consent.Task.WaitAsync(timeout.Token);
            }
            finally { ConsentPanel.Visibility = Visibility.Collapsed; _consent = null; _consentGate.Release(); }
        }
        catch (OperationCanceledException) { return false; }
    }

    private void ReportLostRadio()
    {
        ConnectionBadge.Text = "Radio disconnected";
        ReceiveHeading.Text = "Receive with approval";
        _session?.Cancel();
        RadioMessage.Text = "The radio disconnected. Disconnect and reconnect to resume.";
        _peers.Clear(); EmptyPeers.Visibility = Visibility.Visible;
        SendButton.IsEnabled = false;
    }

    private async Task DisconnectAsync()
    {
        _consent?.TrySetResult(false);
        if (_advertisement is not null) { await _advertisement.DisposeAsync(); _advertisement = null; }
        if (_session is not null) await _session.CancelAsync();
        if (_transport is not null) await _transport.DisposeAsync();
        await Task.WhenAll(_tasks); _tasks.Clear();
        _transport = null; _certificate?.Dispose(); _certificate = null;
        _session?.Dispose(); _session = null;
        _peers.Clear(); EmptyPeers.Visibility = Visibility.Visible;
        ConnectionBadge.Text = "Radio disconnected"; ConnectButton.Content = "Connect radio";
        ReceiveHeading.Text = "Receive with approval";
        HostInput.IsEnabled = KeyInput.IsEnabled = true;
        RadioMessage.Text = "Connect your AirDrop radio to send and receive.";
        UpdateSendButton();
    }

    private void ChooseFiles(object sender, RoutedEventArgs e)
    {
        var picker = new OpenFileDialog { Multiselect = true, Title = "Choose files to AirDrop" };
        if (picker.ShowDialog() == true) SetFiles(picker.FileNames);
    }
    private void OnDrop(object sender, DragEventArgs e)
    { if (e.Data.GetData(DataFormats.FileDrop) is string[] paths) SetFiles(paths); }
    private void SetFiles(IEnumerable<string> paths)
    {
        if (_sending) return;
        _files.Clear(); _files.AddRange(paths.Where(p => File.Exists(p) || Directory.Exists(p)));
        SelectionText.Text = _files.Count == 1 ? PeerText.Printable(Path.GetFileName(_files[0])) : $"{_files.Count} items selected";
        UpdateSendButton();
    }
    private void SelectionChanged(object sender, SelectionChangedEventArgs e) => UpdateSendButton();
    private void UpdateSendButton() => SendButton.IsEnabled = !_busy && !_sending && _transport is not null && _files.Count > 0 && PeerList.SelectedItem is PeerItem;

    private async void SendFiles(object sender, RoutedEventArgs e)
    {
        if (PeerList.SelectedItem is not PeerItem target || _transport is null || _session is null) return;
        _sending = true; UpdateSendButton();
        Task send = SendAsync(target, _session.Token); _tasks.Add(send);
        try { await send; } finally { _sending = false; UpdateSendButton(); }
    }
    private async Task SendAsync(PeerItem target, CancellationToken ct)
    {
        try
        {
            var files = _files.Select(AirDropOutgoingFile.FromPath).ToArray();
            long total = files.Sum(f => f.TotalBytes);
            await using var raw = await _transport!.ConnectAsync(target.Peer, ct);
            await using var tls = await AirDropTls.AuthenticateAsClientAsync(raw, _certificate!, ct);
            await using var sender = new AirDropSenderSession(tls, target.Peer.Flags);
            await sender.DiscoverAsync(ct);
            var ask = new AirDropAskRequest("Windrop", "Windows", Guid.NewGuid().ToString(), AirDropAskRequest.FinderBundleId, files.Select(f => f.ToEntry()).ToList());
            StatusText.Text = "Waiting for the other device to accept…";
            if (!await sender.AskAsync(ask, ct)) { StatusText.Text = "The other device declined."; return; }
            var progress = new Progress<long>(sent => SendProgress.Value = total > 0 ? Math.Min(100, sent * 100.0 / total) : 0);
            await sender.UploadAsync(files, progress, ct);
            SendProgress.Value = 100; StatusText.Text = "Files sent. A little closer.";
        }
        catch (OperationCanceledException) { StatusText.Text = "Transfer stopped."; }
        catch (Exception ex) { StatusText.Text = $"Transfer failed: {PeerText.Printable(ex.Message)}"; }
    }

    private void AcceptTransfer(object sender, RoutedEventArgs e) => _consent?.TrySetResult(true);
    private void DeclineTransfer(object sender, RoutedEventArgs e) => _consent?.TrySetResult(false);
    private void OpenDownloads(object sender, RoutedEventArgs e)
    { Directory.CreateDirectory(_downloads); Process.Start(new ProcessStartInfo(_downloads) { UseShellExecute = true }); }

    private async void OnClosing(object? sender, CancelEventArgs e)
    {
        if (_allowClose) return;
        e.Cancel = true;
        if (_busy) return;
        _busy = true;
        await DisconnectAsync(); _allowClose = true; Close();
    }

    private void Capture(string path)
    {
        // Render the real WPF window for local evidence, without exposing radio credentials.
        HostInput.Text = "127.0.0.1"; KeyInput.Password = "";
        UpdateLayout();
        var content = (FrameworkElement)Content;
        var bitmap = new RenderTargetBitmap((int)content.ActualWidth, (int)content.ActualHeight, 96, 96, PixelFormats.Pbgra32);
        bitmap.Render(content);
        var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(bitmap));
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        using var output = File.Create(path); encoder.Save(output);
    }
}
