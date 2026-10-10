# Native Windows radio development

The first Windows PCIe driver increment builds for the Realtek RTL8852CE (`10ec:c852`). It is **not an AirDrop radio yet**. Its only operation is a read-only identity/capability query. The desktop and transfer CLI still use the authenticated Linux OWL bridge. There is no automatic selection of this unfinished driver.

## What is implemented

`drivers/Windrop.Radio` is a KMDF function driver with a PnP INF for this chip, a SYSTEM/administrator-only device interface, exclusive access, and a 24-byte buffered query. During hardware preparation it reads PCI configuration through the bus interface, validates the vendor/device identity, and locates the chip's BAR2 register resource. It releases its bus-interface reference immediately and clears its cached identity during hardware release.

The driver does not expose physical addresses, arbitrary memory access, or register writes to applications. It does not map BAR2, change bus mastering, allocate DMA buffers, initialize firmware, or receive/transmit frames. Its capability field is zero. Successful installation would establish device ownership and identity only, not AWDL readiness. This code has not been loaded on hardware.

`windrop doctor --native --json` enumerates the driver's device interface, queries it if available, and explains what is missing without contacting the Linux bridge or requiring its key. Missing/failed drivers, unsupported operating systems and the current identity-only driver all return `ready: false` and exit code 3. Selecting both `--native` and `--bridge` is an error.

## Build without installing

Use Visual Studio 2022 C++ tools, x64 Spectre libraries, and its Windows Driver Kit integration. The Build Tools integration component is `Component.Microsoft.Windows.DriverKit.BuildTools`; full Visual Studio uses `Microsoft.Windows.DriverKit`. The build restores Microsoft's pinned WDK `10.0.26100.6584`, whose dependencies are SDK CPP/x64 `10.0.26100.1`. These are the supported VS2022 kit versions. [Microsoft's kit compatibility table](https://learn.microsoft.com/en-us/windows-hardware/drivers/other-wdk-downloads), [Microsoft's integration-component detection](https://github.com/microsoft/Windows-driver-samples/blob/main/BuildEnvironment.ps1).

For a fresh Build Tools installation:

```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --silent --accept-package-agreements --accept-source-agreements --override '--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Runtimes.x86.x64.Spectre --add Component.Microsoft.Windows.DriverKit.BuildTools'
./scripts/build-driver.ps1
```

If Build Tools is already installed, add the driver and Spectre components through its installer before building. `build-driver.ps1` selects 64-bit MSBuild, restores WDK packages through the Microsoft-signed NuGet executable, runs compilation with warnings as errors and code analysis, and generates the unsigned catalog. Output is under `build/driver`, including the package in `build/driver/WindropRadio`.

The script does not install the kernel driver, replace the Realtek network driver, generate/trust a certificate, change boot policy, or reboot. A generated unsigned catalog passing signability checks is not a Microsoft signature.

## Remaining native AirDrop work

1. Implement bounded kernel register operations and Windows-managed DMA for the RTL8852CE's AX rings; preserve device/power/removal lifetime and stop DMA before freeing memory. Devourer's pinned Linux PCIe transport and AX HAL are the implementation reference, not a Windows binary. [Source investigation](BUILTIN-WIFI-RESEARCH.md).
2. Connect chip initialization, firmware, channel changes, frame RX/TX and radio recovery to the AWDL engine. OWL's Linux `nl80211`, pcap injection and TUN integration need Windows counterparts.
3. Provide the AWDL IPv6 interface and connect Windrop's transfer transport to it. A virtual IP interface alone cannot control the physical radio.
4. Qualify discovery, consent, byte-exact transfers in both directions, repeated start/stop, power transitions and restoration of the original network driver with real hardware and an iPhone.

## Loading and signing

Secure Boot is enabled on the development PC. No signing or firmware settings were changed. A kernel driver must satisfy Windows' signing policy before it can be loaded. Production distribution needs Microsoft signing; development test signing requires an appropriate test configuration and may require boot/firmware changes and a reboot. Those are separate from compiling source. [Microsoft kernel signing policy](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/kernel-mode-code-signing-policy--windows-vista-and-later-), [test-signing configuration](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/the-testsigning-boot-configuration-option).

This is a replacement function driver, not a filter sharing the card with Realtek's network driver. Binding it would remove ordinary Wi-Fi networking on that card while it owns the device. Hardware testing needs a verified rollback path and another network connection. No concurrent Wi-Fi Internet/AWDL support is claimed.
