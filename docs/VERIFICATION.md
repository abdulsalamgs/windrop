# Verification

## Software checks

Windows x64, .NET SDK 8.0.425:

```text
dotnet build Windrop.sln -c Release --warnaserror
Build succeeded. 0 warnings. 0 errors.

dotnet test tests/WinDrop.Protocol.Tests -c Release --no-build
Passed: 290. Failed: 0. Skipped: 0.
```

279 tests come from the pinned upstream protocol implementation. Eleven new test cases cover public CLI readiness and file delivery, rejected radio authentication, exclusive discovery ownership/restart, accepted/declined inbound transfers, authenticated outbound delivery and destination restriction, and discovery during controller resets through the actual UDP loop. Native CLI diagnostics explain the unavailable Windows driver without bridge credentials and reject conflicting backend selection. Accepted fixtures arrive byte-for-byte; a declined fixture creates no files. Existing protocol tests cover malformed archives, decompression limits, upload-before-consent, collision preservation, truncated transfers, HTTP, DNS, and plist behavior.

The new relay tests bind to IPv6 loopback using its explicitly requested test mode; the discovery test uses the same UDP receive/forward loop with unicast loopback delivery. Production requires an IPv6 link-local radio address and restricts outbound connections to that interface's link. The Windows CLI transfer test uses explicit `--lan --yes` development options. These tests exercise real sockets and TLS but do not simulate RF, AWDL scheduling, or Apple's implementation.

The required standards/spec reviews found a discovery-pump failure on controller disconnect, a handshake/event race, and stale GUI peer endpoints. The controller-reset regression first failed after disconnect and now passes after per-controller write-error handling and delayed publication of ready controllers. Refreshed GUI peers now update their send endpoint and flags. Outgoing-relay coverage was added following review.

All three shell scripts pass `bash -n`. The Windows GUI was built and launched, and its actual WPF visual tree was rendered to a PNG. Layout was inspected and corrected for sidebar clipping and button text contrast. A self-contained local HTML page displays the final native capture; no hosted artifact was published. The PR includes an after-only preview because this is a new app.

## Native driver build

Visual Studio Build Tools 2022 17.14.41, MSVC 14.44.35207, Microsoft WDK NuGet 10.0.26100.6584:

```text
./scripts/build-driver.ps1
WindropRadio.sys built with /W4 /WX, Spectre mitigation and kernel code analysis.
Signability test complete. Errors: None. Warnings: None.
Catalog generation complete.

infverif /v build/driver/WindropRadio/WindropRadio.inf
INF is VALID.

windrop doctor --native --json
ready: false; backend: windows-pcie; driverLoaded: false; exit code: 3.
```

The catalog and kernel binary are unsigned. The Windrop PCIe driver was not installed or loaded; no physical radio, signing or boot configuration was changed. These build checks do not verify loaded PCI resource handling, DMA or device lifecycle. The native frame/DMA and AWDL implementation is described below. [Native driver scope and remaining qualification](WINDOWS-DRIVER.md).

During this increment, an overlapping app build/test run encountered Windows file locks and one controller-reset assertion failed before its reply type was captured. After separating build and tests, six isolated reset-test runs and three complete serial runs passed (290 tests each). The assertion now includes the reply's error/event type without session credentials so a recurrence can distinguish delayed controller cleanup from event ordering. No production radio change or proven fix for that isolated assertion failure is claimed.

## Native frame and AWDL integration

The native workers build against pinned Devourer, OWL and radiotap sources using
MSVC x64, with `/W4 /WX` for Windrop workers. The kernel driver passes compilation,
code analysis and catalog/INF validation. `build-native.ps1 -Publish` successfully
produces the development ZIP containing the workers, Wintun/libusb DLLs,
self-contained Windows radio bridge, unsigned driver, licenses and full sources.

```text
./scripts/test-native.ps1
PASS: exact 48-byte IPv6 delivery from an independent AWDL wire fixture.
PASS: non-Apple LLC frames dropped.
PASS: invalid AWDL data prefixes dropped.
PASS: captured iPhone idle sequence retains social windows, blanks channel 132.
PASS: standalone tunnel CLI rejects invalid IPv6 configuration before adapter loading.

windrop-awdl run (original Realtek driver still installed)
Driver unavailable; Radio worker stopped; exit code 3.
No awdl0 adapter created.
```

The first three public CLI checks were developed red/green: missing replay
implementation, accepting the wrong LLC OUI, and accepting the wrong AWDL prefix
each failed before the corresponding implementation/fix. The channel regression
uses the independent iPhone idle sequence recorded in pinned OWL's tests inside a
hand-built MIF: before review correction replay advertised channel 132; afterward
it advertises `0,0,149,0,0,0,0,0,6,0,149,0,0,0,0,0`. No Windrop serializer generates
the expected fixture. The independent MIT tunnel worker was added after its public
CLI check failed with a missing worker; that check now passes without creating an
adapter. These are offline checks, not a captured full iPhone transfer.

The standards review identified unsupported PCI-header writes, writable DMA
ownership registers and hardware mutation during diagnostic cleanup. Fixes use
the OS function-level reset interface, protect descriptor configuration and track
explicit hardware initialization. The spec review identified required unaligned
HAL accesses and unsupported infrastructure windows in normal iPhone sequences;
aligned splitting and social-window filtering address those findings. Further
review corrected unsynchronized removal callbacks, an unsafe assumption about
surprise removal and incomplete transmit initialization. Final Standards and
Spec rechecks found no remaining substantive source findings. Reset/DMA failure
paths and physical-radio scheduling still require loaded fault-injection tests. Wintun
is now owned by a generic MIT tunnel worker, rather than dynamically loaded into
the GPL3 AWDL program. A versioned, hash-pinned 7-Zip extractor fixes fresh Windows
CI extraction of the libusb BCJ2/LZMA archive; Windows tar and CMake libarchive
both failed on that codec before the correction.

The public standalone tunnel was also tested against a real temporary signed
Wintun adapter on Windows 11, without initializing the physical radio:

```text
./scripts/test-tunnel.ps1
./scripts/test-tunnel.ps1 -Shutdown Eof
PASS: only the explicitly configured link-local address is exposed.
PASS: Windows UDP reaches tunnel stdout with exact payload bytes.
PASS: tunnel stdin reaches Windows UDP with exact payload bytes.
PASS: Stop exits with an undrained, saturated output pipe.
PASS: EOF exits with an undrained, saturated output pipe.
Owned temporary adapters are removed after each test.
```

Live testing first failed at `SetIpInterfaceEntry` with Windows error 87.
Preserving Windows' link-local behavior fixes that error; disabling link-local
behavior was the unsupported change. The next run reproduced the review's
blocked-output shutdown defect. A bounded cancellable writer fixes Stop/EOF.
The address-selection check then failed with two link-local addresses; removing
only Windows' automatically generated link-local entry on the helper's own new
adapter leaves the configured EUI-64 address. Each defect was observed before
its correction. Packet checks exercise ordinary Windows UDP and independently
constructed IPv6/UDP bytes, including the mandatory UDP checksum. They qualify
this virtual-interface path only. The signed Wintun virtual driver was loaded;
the unsigned Windrop PCIe driver remains uninstalled and no RF test was made.
CI now runs both tunnel shutdown variants as well as offline replay.

The final serial .NET build passed with zero warnings/errors, and all 290 existing
tests passed. The native pipeline was attempted without installing its driver and
stopped at missing-driver diagnostics. Secure Boot and the original Wi-Fi driver
remain in place; no native hardware readiness or successful iPhone transfer is
claimed.

## Limits

- No dedicated USB adapter or Apple device was available. This build has not completed a real iPhone send or receive. Discovery over `awdl0`, USB passthrough, radio throughput, and iOS version compatibility remain unverified.
- The optional WSL kernel helper was syntax checked but not compiled or installed. Its generated kernel affects all WSL distributions only if the user explicitly configures it.
- The disconnected GUI state was visually inspected. Live peer selection, Windows firewall prompting, and a real-device approval dialog still need hardware acceptance testing.
- Self-contained Windows and Linux radio packages are unsigned. Setup consists of source build and the radio runbook; there is no automatic driver installation or turnkey hardware installer.
- Radio access-key control messages require a trusted local or private/VPN connection. Everyone mode does not authenticate Apple identities; Contacts Only is unsupported.

See [RADIO-SETUP.md](RADIO-SETUP.md#first-real-device-qualification) for the remaining hardware qualification procedure.
