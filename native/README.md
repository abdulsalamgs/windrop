# Native Windows AWDL backend

This is an experimental RTL8852CE (`10ec:c852`) backend. The kernel frame
transport and Windows OWL integration compile and pass offline frame checks.
They have **not been loaded or qualified on hardware**. The development driver
is unsigned, so this package cannot currently connect the development PC to an
iPhone with Secure Boot enabled. It never replaces a driver or changes boot
policy automatically.

## Build

Install Visual Studio 2022 C++ Build Tools, x64 Spectre libraries, WDK integration
and the .NET 8 SDK as described in [WINDOWS-DRIVER.md](../docs/WINDOWS-DRIVER.md).
Run from the repository root:

```powershell
./scripts/build.ps1 -Publish
./scripts/build-native.ps1 -Publish
```

The native build pins and verifies its dependencies, restores Microsoft's SDK
and WDK through NuGet, builds both workers and the unsigned driver, and runs the
public replay checks. Output: `publish/Windrop-native-development-x64.zip`.
Its `sources/windrop` directory contains the complete build sources and scripts;
the other source archives contain the pinned upstream code. Apply the included
patches through the build script. The native programs require the Microsoft
Visual C++ 2015–2022 x64 runtime, installed by Visual Studio or the official
[redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).

## Architecture

`WindropRadio.sys` owns a KMDF common buffer containing the AX TX/RX descriptor
rings. Buffered IOCTLs copy bounded packets; user space cannot supply DMA
addresses. The SYSTEM/admin interface is exclusive. OS function-level reset
support is required before initializing DMA. The Realtek network driver
and Windrop cannot own this PCI function simultaneously.

`windrop-hardware.exe` uses the pinned Devourer 8852C HAL for firmware, monitor
initialization, injection, RX descriptor parsing and channel changes. It wraps
bare MPDUs in radiotap with FCS and RSSI metadata. `doctor` queries the driver;
`serve` initializes the radio and speaks binary stdio.

`windrop-awdl.exe` links OWL's protocol core: action/data RX/TX, synchronization,
election, peer expiry and channel sequences. A Windows loop replaces Linux
libev/pcap/netlink. Retunes require acknowledgements before sending, and queued
transmits expire. Wintun exposes an IPv6-only `awdl0` with the radio's EUI-64
link-local address and MTU 1450. The existing authenticated .NET radio bridge
then carries discovery and AirDrop traffic for the desktop and CLI.

The GPL2 hardware worker and GPL3 AWDL worker are separate executables with a
generic raw-frame pipe protocol. No Devourer code is linked into the AWDL worker.
See [THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md).

## Commands

```powershell
./build/native/Release/windrop-hardware.exe doctor
./build/native/Release/windrop-awdl.exe --help
./build/native/Release/windrop-awdl.exe replay capture.pcap packets.bin --peer aabbccddeeff
./scripts/start-native.ps1
```

Replay accepts classic little-endian PCAP v2.4, radiotap link type 127, with
frames up to 4096 bytes. It initializes no hardware and overwrites the explicitly
named output with concatenated decoded IPv6 packets. The replay peer argument
registers a known sender for offline decoding only; live peers come from RF.

After a correctly signed driver has been installed and hardware qualified,
`start-native.ps1` runs in administrator PowerShell. It checks the driver,
starts AWDL, creates a fresh injected bridge key, starts the localhost bridge
and opens Windrop. Closing the app stops the session. Existing `awdl0` adapters
are refused. The script does not install a kernel driver. For a running session,
use regular `windrop doctor`; `doctor --native` checks the exclusive driver and
can report busy while the hardware worker owns it.

## IPC

Anonymous pipes inherit only stdin, stdout and stderr into a child contained in
a kill-on-close job. Each record has little-endian uint32 type and length, then
up to 32768 bytes. Types: Hello(1, six MAC bytes), Frame(2, radiotap MPDU),
Channel(3, uint32), Transmit(4, uint64 GetTickCount64 expiry followed by radiotap
MPDU), Stop(5, empty), Fault(6, diagnostic text), ChannelAck(7, uint32).
Incoming and outgoing queues are bounded; RX overflow drops packets. Control
overflow, failed retunes and broken pipes terminate the session.

## Qualification still required

Only 20 MHz social channels 6, 36, 44 and 149 are accepted. Regulatory approval,
Windows scheduler timing, firmware initialization, actual frame RX/TX,
channel transitions, DMA teardown, sleep/resume, removal and original-driver
restoration require hardware testing. No shared normal Wi-Fi networking,
Contacts Only, DFS, 6 GHz or production reliability is claimed.

The acceptance gate is real iPhone discovery and byte-exact transfers in both
directions with consent, followed by repeated stop/start and power/removal tests
under Driver Verifier. Offline PCAP replay and loopback transfer tests do not
establish that gate.
