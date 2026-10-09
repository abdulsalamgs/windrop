using System.Net.NetworkInformation;

namespace WinDrop.Protocol.Discovery;

public sealed record RadioReadiness(bool Ready, string Reason, string Host, string[] Adapters);

public static class RadioSettings
{
    public static string Host => Environment.GetEnvironmentVariable("WINDROP_RADIO_HOST") ?? "127.0.0.1";
    public static string Key => Environment.GetEnvironmentVariable("WINDROP_RADIO_KEY") ?? "";

    public static async Task<RadioReadiness> CheckAsync(string? host = null, string? key = null, CancellationToken ct = default)
    {
        host ??= Host; key ??= Key;
        string[] adapters = NetworkInterface.GetAllNetworkInterfaces()
            .Where(n => n.NetworkInterfaceType == NetworkInterfaceType.Wireless80211)
            .Select(n => n.Description).ToArray();
        if (key.Length < 32)
            return new(false, "Set WINDROP_RADIO_KEY to a random key shared with windrop-radio. Direct AirDrop needs OWL and a compatible dedicated USB Wi-Fi adapter in WSL2 or Linux.", host, adapters);
        try
        {
            await using var transport = new BridgeTransport(host, key);
            await transport.ProbeAsync(ct);
            return new(true, $"Radio connected on {transport.BridgeInterface}. iPhone interoperability requires a real-device transfer.", host, adapters);
        }
        catch (Exception e) when (e is System.Net.Sockets.SocketException or BridgeException or IOException or OperationCanceledException)
        {
            ct.ThrowIfCancellationRequested();
            return new(false, "Radio unavailable. Start OWL, then windrop-radio with the same key; check the host, firewall, and USB passthrough.", host, adapters);
        }
    }
}
