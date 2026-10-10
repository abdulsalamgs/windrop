using System.Diagnostics;
using System.Text.Json;
using System.Net;
using System.Net.Sockets;

namespace WinDrop.Protocol.Tests;

public class CliTests
{
    [Fact]
    public async Task Doctor_rejects_selecting_both_native_driver_and_remote_bridge()
    {
        var result = await RunAsync(["doctor", "--native", "--bridge", "127.0.0.1"], key: null);
        Assert.Equal(1, result.ExitCode);
        Assert.Contains("Choose --native or --bridge", result.Output);
    }

    [Fact]
    public async Task Doctor_native_explains_unavailable_Windows_radio_without_bridge_credentials()
    {
        var result = await RunAsync(["doctor", "--native", "--json"], key: null);
        Assert.Equal(3, result.ExitCode);
        using var json = JsonDocument.Parse(result.Output);
        Assert.False(json.RootElement.GetProperty("ready").GetBoolean());
        Assert.Equal("windows-pcie", json.RootElement.GetProperty("backend").GetString());
        Assert.Contains("Windows", json.RootElement.GetProperty("reason").GetString());
        Assert.DoesNotContain("WINDROP_RADIO_KEY", result.Output);
    }

    [Fact]
    public async Task Public_CLI_send_and_receive_deliver_the_original_file()
    {
        string directory = Path.Combine(Path.GetTempPath(), "windrop-cli-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        string source = Path.Combine(directory, "fixture.txt");
        string downloads = Path.Combine(directory, "received");
        byte[] expected = "CLI transfer fixture: original bytes.\n"u8.ToArray();
        await File.WriteAllBytesAsync(source, expected);
        var reservation = new TcpListener(IPAddress.Loopback, 0);
        reservation.Start(); int port = ((IPEndPoint)reservation.LocalEndpoint).Port; reservation.Stop();
        using var receiver = Process.Start(StartInfo(["receive", "--lan", "--yes", "--dir", downloads, "--port", port.ToString()], null))!;
        var errors = receiver.StandardError.ReadToEndAsync();
        try
        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            while (true)
            {
                string? line = await receiver.StandardOutput.ReadLineAsync(timeout.Token);
                Assert.NotNull(line);
                if (line.Contains("Ctrl+C to stop")) break;
            }
            // Drain logs so the receiver cannot block on its output pipe.
            var output = receiver.StandardOutput.ReadToEndAsync();
            var sent = await RunAsync(["send", "--lan", "--peer", "127.0.0.1", "--port", port.ToString(), source], null);
            Assert.True(sent.ExitCode == 0, sent.Output);
            Assert.Contains("Done.", sent.Output);
            Assert.Equal(expected, await File.ReadAllBytesAsync(Path.Combine(downloads, "fixture.txt"), timeout.Token));
        }
        finally
        {
            if (!receiver.HasExited) receiver.Kill(entireProcessTree: true);
            await receiver.WaitForExitAsync();
            await errors;
            Directory.Delete(directory, recursive: true);
        }
    }

    [Fact]
    public async Task Doctor_reports_missing_radio_configuration_without_exposing_keys()
    {
        var result = await RunAsync(["doctor", "--json"], key: null);
        Assert.Equal(3, result.ExitCode);
        using var json = JsonDocument.Parse(result.Output);
        Assert.False(json.RootElement.GetProperty("ready").GetBoolean());
        Assert.Contains("WINDROP_RADIO_KEY", json.RootElement.GetProperty("reason").GetString());
    }

    internal static async Task<(int ExitCode, string Output)> RunAsync(string[] args, string? key)
    {
        using var process = Process.Start(StartInfo(args, key))!;
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        try { await process.WaitForExitAsync(timeout.Token); }
        catch { process.Kill(entireProcessTree: true); throw; }
        return (process.ExitCode, await output + await error);
    }

    private static ProcessStartInfo StartInfo(string[] args, string? key)
    {
        var start = new ProcessStartInfo(Environment.GetEnvironmentVariable("DOTNET_HOST_PATH") ?? "dotnet")
        { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false };
        start.ArgumentList.Add("exec");
        start.ArgumentList.Add("--runtimeconfig");
        start.ArgumentList.Add(Path.Combine(AppContext.BaseDirectory, "WinDrop.Protocol.Tests.runtimeconfig.json"));
        start.ArgumentList.Add(Path.Combine(AppContext.BaseDirectory, "windrop.dll"));
        foreach (string arg in args) start.ArgumentList.Add(arg);
        start.Environment.Remove("WINDROP_RADIO_KEY");
        start.Environment.Remove("WINDROP_RADIO_HOST");
        if (key is not null) start.Environment["WINDROP_RADIO_KEY"] = key;
        return start;
    }
}
