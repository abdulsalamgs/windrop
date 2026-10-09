using System.Net;
using System.Net.NetworkInformation;
using System.Runtime.InteropServices;

namespace Windrop.Radio;

public static class RadioCommand
{
    public static async Task<int> RunAsync(string[] args)
    {
        if (args.Contains("--help"))
        {
            Console.WriteLine("windrop-radio --interface awdl0 [--listen 0.0.0.0]\nRequires WINDROP_RADIO_KEY (32+ characters). OWL must be running first.");
            return 0;
        }
        try
        {
            string name = Value(args, "--interface") ?? "awdl0";
            string key = Environment.GetEnvironmentVariable("WINDROP_RADIO_KEY")
                ?? throw new ArgumentException("Set WINDROP_RADIO_KEY to a random 32-byte hex key shared with the Windows app.");
            var device = NetworkInterface.GetAllNetworkInterfaces().FirstOrDefault(n => n.Name == name)
                ?? throw new ArgumentException($"No {name} interface. Start OWL on a compatible dedicated USB Wi-Fi adapter first.");
            var address = device.GetIPProperties().UnicastAddresses.Select(a => a.Address).FirstOrDefault(a => a.IsIPv6LinkLocal)
                ?? throw new ArgumentException($"{name} has no IPv6 link-local address. Wait for OWL to bring the radio up.");
            address.ScopeId = device.GetIPProperties().GetIPv6Properties().Index;
            await using var server = new RadioServer(name, address, IPAddress.Parse(Value(args, "--listen") ?? "127.0.0.1"), key);
            using var stopping = new CancellationTokenSource();
            using var termination = OperatingSystem.IsWindows() ? null : PosixSignalRegistration.Create(PosixSignal.SIGTERM,
                context => { context.Cancel = true; stopping.Cancel(); });
            Console.CancelKeyPress += (_, e) => { e.Cancel = true; stopping.Cancel(); };
            await server.StartAsync(stopping.Token);
            Console.WriteLine($"Radio ready on {name}. Control {server.ControlPort}, data {server.DataPort}. Press Ctrl+C to stop.");
            try { await Task.Delay(Timeout.Infinite, stopping.Token); } catch (OperationCanceledException) { }
            return 0;
        }
        catch (Exception e) when (e is ArgumentException or System.Net.Sockets.SocketException or InvalidOperationException)
        {
            Console.Error.WriteLine(e.Message); return 1;
        }
    }

    private static string? Value(string[] args, string name)
    {
        int index = Array.IndexOf(args, name);
        if (index < 0) return null;
        if (index + 1 == args.Length) throw new ArgumentException($"{name} requires a value.");
        return args[index + 1];
    }
}
