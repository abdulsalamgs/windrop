# Windrop

Direct Apple AirDrop from a Windows desktop, with a native interface for nearby devices, file selection, sending, and approval of incoming transfers. No browser or iPhone app is involved.

**Hardware is required.** Windows runs the AirDrop protocol; a dedicated USB Wi-Fi adapter in WSL2, or a Linux radio host, supplies the AWDL link through OWL. A built-in Windows Wi-Fi card is not sufficient by itself. This project has passed software integration tests; **this Windrop build has not yet been tested against a real iPhone or an AWDL radio**.

The protocol implementation is adapted from MIT-licensed [UvejsGj/WinDrop](https://github.com/UvejsGj/WinDrop). See [third-party notices](THIRD-PARTY-NOTICES.md) for the pinned source and the separate OWL license.

## Build and run

Install the [.NET 8 SDK](https://dotnet.microsoft.com/download/dotnet/8.0) on Windows, then:

```powershell
./scripts/build.ps1 -Publish
$env:WINDROP_RADIO_HOST = '127.0.0.1'
# Set WINDROP_RADIO_KEY to the same random key as the radio. Do not commit it.
./publish/windows/Windrop.exe
```

The desktop package is `publish/Windrop-windows-x64.zip`. It includes .NET and runs without installing a runtime. It is unsigned. The build also produces a Windows CLI and a self-contained Linux x64 radio bridge. CI supplies a Windows build artifact on each successful workflow run.

Read [radio setup and hardware selection](docs/RADIO-SETUP.md) before expecting a phone to appear. With the radio connected, turn on Wi-Fi and Bluetooth on the iPhone and choose AirDrop's **Everyone for 10 Minutes**. Select Windrop from the native Share → AirDrop menu and approve the transfer on Windows.

## CLI

```powershell
./publish/cli/windrop.exe doctor --json
./publish/cli/windrop.exe browse
./publish/cli/windrop.exe receive
./publish/cli/windrop.exe send --to 'device name' photo.jpg
```

`WINDROP_RADIO_HOST` defaults to `127.0.0.1`. `--bridge <host>` overrides it. `WINDROP_RADIO_KEY` is required; keys never appear in diagnostics. CLI `--yes` explicitly skips consent for scripted testing. `--lan` explicitly enables ordinary-network development testing; it does not reach an iPhone and is never selected automatically.

Downloads default to `Downloads\Windrop`. CLI `--dir` selects another folder. Incoming uploads are staged and committed only after successful completion. Conflicting existing names are preserved. Sender display names are unverified, and Everyone mode does not authenticate an Apple identity; approve only expected transfers. Contacts Only is unsupported.

## Verification

`dotnet test tests/WinDrop.Protocol.Tests` exercises the protocol plus real-socket CLI/radio behavior. [Verification evidence and limits](docs/VERIFICATION.md) distinguishes loopback results from Apple interoperability. The architecture and agreed scope are recorded in [SPEC.md](docs/SPEC.md).
