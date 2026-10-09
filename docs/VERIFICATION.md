# Verification

## Software checks

Windows x64, .NET SDK 8.0.425:

```text
dotnet build Windrop.sln -c Release --warnaserror
Build succeeded. 0 warnings. 0 errors.

dotnet test tests/WinDrop.Protocol.Tests -c Release --no-build
Passed: 285. Failed: 0. Skipped: 0.
```

279 tests come from the pinned upstream protocol implementation. Six new test cases cover public CLI readiness and file delivery, rejected radio authentication, exclusive discovery ownership/restart, and accepted/declined inbound transfers through the actual socket relay. The accepted fixture arrives byte-for-byte; a declined fixture creates no files. Existing protocol tests cover malformed archives, decompression limits, upload-before-consent, collision preservation, truncated transfers, HTTP, DNS, and plist behavior.

The new relay tests bind to IPv6 loopback using its explicitly requested test mode. Production requires an IPv6 link-local radio address and restricts outbound connections to that interface's link. The Windows CLI transfer test uses explicit `--lan --yes` development options. These tests exercise real sockets and TLS but do not simulate RF, AWDL scheduling, or Apple's implementation.

All three shell scripts pass `bash -n`. The Windows GUI was built and launched, and its actual WPF visual tree was rendered to a PNG. Layout was inspected and corrected for sidebar clipping and button text contrast. A self-contained local HTML page displays the final native capture; no hosted artifact was published. The PR includes an after-only preview because this is a new app.

## Limits

- No dedicated USB adapter or Apple device was available. This build has not completed a real iPhone send or receive. Discovery over `awdl0`, USB passthrough, radio throughput, and iOS version compatibility remain unverified.
- The optional WSL kernel helper was syntax checked but not compiled or installed. Its generated kernel affects all WSL distributions only if the user explicitly configures it.
- The disconnected GUI state was visually inspected. Live peer selection, Windows firewall prompting, and a real-device approval dialog still need hardware acceptance testing.
- Self-contained Windows and Linux radio packages are unsigned. Setup consists of source build and the radio runbook; there is no automatic driver installation or turnkey hardware installer.
- Radio access-key control messages require a trusted local or private/VPN connection. Everyone mode does not authenticate Apple identities; Contacts Only is unsupported.

See [RADIO-SETUP.md](RADIO-SETUP.md#first-real-device-qualification) for the remaining hardware qualification procedure.
