# Native Windows radio development

The experimental native backend targets the Realtek RTL8852CE (`10ec:c852`). It now implements register control, kernel-owned AX DMA rings, frame reception/injection, the Devourer 8852C HAL, OWL AWDL and Wintun IPv6. It has compiled and passed offline software checks, but **has never been loaded or qualified on hardware**. The unsigned development package cannot currently replace the signed Realtek driver on the Secure Boot development PC.

## Driver boundary and lifetime

`drivers/Windrop.Radio` is a KMDF replacement function driver, not a filter sharing the Realtek network driver. Its INF restricts its exclusive device interface to SYSTEM and administrators. Binding it removes ordinary Wi-Fi networking on that card while it owns the function. No concurrent Wi-Fi Internet/AWDL support is claimed.

ABI v2 exposes a 24-byte identity/capability query, explicit initialization, bounded aligned BAR2 operations, ring setup, buffered TX/RX and stop. TX accepts at most 4152 descriptor/payload bytes; RX assembles at most 32768 bytes. Register requests cannot supply DMA addresses or overwrite ring bases, sizes, indices or BDRAM ownership. The unaligned HAL register accesses are split into aligned byte operations. Firmware/chip initialization runs in the separately licensed hardware worker.

KMDF owns a 32-bit DMA common buffer for all descriptor rings and payload storage. The driver programs only logical addresses from that allocation, reuses TX pages only after release reports, and retains the allocation through power transitions. Power-managed sequential IOCTLs and an explicit shared hardware mutex serialize IO, file cleanup, PnP and power callbacks. Read-only diagnostics do not initialize or stop the chip on handle close. Pending removal refuses new operations; a canceled removal allows a new explicitly initialized session.

Before accepting mutable operations the driver requires the OS's supported **function-level reset** interface. Missing reset support leaves capabilities unavailable. Initialization resets only this PCI function before the HAL reads/writes chip registers. Teardown stops DMA and verifies idle; a failed stop requests an OS function-level reset and waits for completion. PCI configuration headers belong to Windows and are never written by Windrop. Platform-level resets are not requested. [PCI configuration ownership](https://learn.microsoft.com/en-us/windows-hardware/drivers/pci/accessing-pci-device-configuration-space), [device reset interface](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/working-with-guid-device-reset-interface-standard).

A query-remove that cannot establish quiescence returns `STATUS_DEVICE_HUNG`. A surprise-removal callback can also mean reported device failure; it ends further MMIO access but preserves DMA ownership until an OS reset establishes safety. If teardown or surprise removal cannot stop/reset a device with published DMA addresses, or an asynchronous reset fails to complete within ten seconds, the driver deliberately fail-stops Windows with vendor bugcheck `0xE0574401` instead of freeing memory that hardware may still write. This conservative failure path can also stop Windows after physical removal of an active adapter if the OS cannot confirm a successful reset. Fault-injection qualification is required before installation or release; this is not a production driver.

`windrop doctor --native --json` checks the device interface without bridge credentials. Missing, failed, busy or unsupported drivers return `ready: false` and exit 3. A loaded driver alone also remains unavailable for AirDrop: use regular `doctor` against the authenticated bridge after the AWDL worker is running. Selecting both `--native` and `--bridge` is an error.

## Build without installing

Use Visual Studio 2022 C++ tools, x64 Spectre libraries and WDK integration. Build Tools uses `Component.Microsoft.Windows.DriverKit.BuildTools`; full Visual Studio uses `Microsoft.Windows.DriverKit`. The build restores WDK `10.0.26100.6584` and SDK CPP/x64 `10.0.26100.1`, the VS2022-compatible kit versions. [Kit compatibility](https://learn.microsoft.com/en-us/windows-hardware/drivers/other-wdk-downloads), [Microsoft's integration detection](https://github.com/microsoft/Windows-driver-samples/blob/main/BuildEnvironment.ps1).

```powershell
./scripts/build-driver.ps1
./scripts/build-native.ps1 -Publish
```

The driver script selects 64-bit MSBuild, verifies the Microsoft-signed NuGet executable, compiles with warnings as errors, Spectre mitigation and code analysis, validates the INF and generates the unsigned catalog. Output: `build/driver/WindropRadio`. The native build uses pinned, patched upstream sources and runs public PCAP replay checks. See [native build/run instructions](../native/README.md).

Neither script installs a driver, trusts a signing certificate, changes boot policy or reboots. Passing catalog signability checks is not Microsoft signing. Secure Boot remains enabled on the development PC. Loading requires Windows-compliant signing; production distribution needs Microsoft signing. [Kernel signing policy](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/kernel-mode-code-signing-policy--windows-vista-and-later-).

## Qualification gate

For local development, a self-signed test package can be prepared without an EV
purchase. Loading it requires a separate boot-security and installation decision;
preparation leaves Wi-Fi binding, certificate trust and boot policy unchanged.
See [local testing](LOCAL-DRIVER-TESTING.md).

The source implementation is present; the remaining gate is loaded hardware validation, not another identity-only scaffold. Validate OS function-reset support, firmware startup, physical RX/TX, release-report parsing, regulatory/channel behavior, retune latency and AWDL timing first. Then validate actual iPhone discovery, consent and byte-exact transfers in both directions. Finally test repeated sessions, process crashes, sleep/resume, removal, reset failure and restoration of the original driver under Driver Verifier, with another network connection and a rollback path. No installation is authorized merely by building this package.
