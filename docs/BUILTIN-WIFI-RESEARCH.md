# Built-in Wi-Fi research

Research date: 2026-10-10. Target: Realtek RTL8852CE PCIe Wi-Fi, Windows driver `6001.16.172.0`. The user requires Windows to remain running. A reversible monitor-mode probe was performed with Wi-Fi disconnected and Ethernet active; the original mode was restored. No driver was replaced.

Inspected Devourer revision: `f369aac553c9ce884901115ef9e66509f93c5683`.

## Finding

The RTL8852CE is **not inherently incapable of raw Wi-Fi transmission**. OpenIPC's Devourer project reports monitor reception and packet injection on this exact PCIe chip (`10ec:c852`) on 5 and 6 GHz. This is a credible hardware lead, not a successful Windrop/iPhone test. Its PCIe backend currently runs on Linux; Windows support in the same project is for USB devices. [Devourer hardware and platform support](https://github.com/OpenIPC/devourer/tree/f369aac553c9ce884901115ef9e66509f93c5683#supported-hardware).

The current Windows driver remains the immediate obstacle. The coordinator's reversible Native Wi-Fi probe returned error 50 (`ERROR_NOT_SUPPORTED`) when requesting Network Monitor mode; the original and restored mode were Extensible Station (`4`). This demonstrates a failure of that exposed driver/API path, not a proof that every possible Windows driver is incapable. Microsoft's query API distinguishes the station and monitor modes. [WlanQueryInterface](https://learn.microsoft.com/en-us/windows/win32/api/wlanapi/nf-wlanapi-wlanqueryinterface).

## Routes using this card

| Route | Evidence and next step | Present limitation |
| --- | --- | --- |
| Linux boot using the existing PCIe card | Devourer already implements the card's raw radio transport. Qualify reception, AWDL action and data transmission, channel switching, and real iPhone delivery. | Requires leaving the Windows session; Devourer does not itself implement AirDrop or OWL's virtual interface. Integration and hardware qualification remain. |
| Native Windows PCIe radio backend | Reuse the known chip/HAL work and implement a Windows device driver replacing Devourer's VFIO transport. | Substantial driver development, signing, hardware testing and recovery work. No ready Windows RTL8852CE injection backend was found. |
| Current Windows driver plus Npcap | Monitor capture can be probed where supported. | Capture does not supply the missing AWDL transmit path; the driver's monitor-mode request already failed. |
| WSL2 with built-in PCIe passthrough | Would require giving Linux the real PCIe device, not a virtual IP interface. | No supported WSL PCIe Wi-Fi assignment route was identified. USB/IP cannot attach a PCIe function. |

Devourer's source requires exclusive device ownership, Linux VFIO and an IOMMU group, a BAR2 register mapping, bus mastering, and pinned DMA rings below 4 GiB. The RTL8852CE uses its existing AX DMA implementation; its interrupt path currently polls. These are concrete platform dependencies to replace, not changes to a socket address or WSL network mode. [PcieTransport.h](https://github.com/OpenIPC/devourer/blob/f369aac553c9ce884901115ef9e66509f93c5683/src/PcieTransport.h), [PcieTransport.cpp](https://github.com/OpenIPC/devourer/blob/f369aac553c9ce884901115ef9e66509f93c5683/src/PcieTransport.cpp), [on-air qualification script](https://github.com/OpenIPC/devourer/blob/f369aac553c9ce884901115ef9e66509f93c5683/tests/pcie_8852ce_onair.sh).

A Windows port would need a device-owning driver with resource and DMA handling, then a narrow authenticated interface to the radio process. Microsoft documents KMDF DMA enablement and production kernel-driver signing requirements. [KMDF DMA](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/enabling-dma-transactions), [driver signing policy](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/kernel-mode-code-signing-policy--windows-vista-and-later-). This is an engineering option, not a driver ready to install here.

The device transport alone would not finish AirDrop. The implementation would also need to connect raw frame reception/transmission and channel control to AWDL, expose the resulting IPv6 link to Windrop, restore ordinary Wi-Fi ownership after stopping, and pass real iPhone transfers in both directions. Software loopback tests cannot establish those hardware behaviors. Secure Boot is enabled on this PC; no signing settings were changed during this investigation.

The Windows IPv6 interface is a separate problem from PCIe radio access. Wintun supplies a signed virtual layer-3 adapter and a userspace packet API that could serve that part of a future backend. It does not drive a physical Wi-Fi card, provide raw 802.11 transmission, or implement AWDL. This is an integration candidate, not an established RTL8852CE solution. [Wintun's published interface and signed distribution](https://www.wintun.net/).

## UvejsGj/WinDrop audit

A fresh upstream fetch found `main` still at `d34397dbf98b3bfaa3c96b1c9872d6d790dcf029`, exactly the MIT-licensed protocol revision Windrop already imported. Its Windows GUI constructs `InfraWifiTransport`, which explicitly reports `CanReachAppleDevices => false`. The bridge transports IP traffic to a Linux radio process; it does not make Windows control the PCIe card. Its reported direct iPhone transfers used the CLI on Linux with OWL. No newer native Windows radio implementation was found on the inspected branches. [Windows GUI transport](https://github.com/UvejsGj/WinDrop/blob/d34397dbf98b3bfaa3c96b1c9872d6d790dcf029/src/WinDrop.App/MainWindow.xaml.cs#L38), [transport capabilities](https://github.com/UvejsGj/WinDrop/blob/d34397dbf98b3bfaa3c96b1c9872d6d790dcf029/src/WinDrop.Protocol/Discovery/InfraWifiTransport.cs#L9), [reported AirDrop tests](https://github.com/UvejsGj/WinDrop/blob/d34397dbf98b3bfaa3c96b1c9872d6d790dcf029/README.md#where-it-stands-with-an-iphone).

The separate `phone-page` branch at `b3afab521c89959a7a33261e08f45c16b549d831` adds a QR-linked Safari page with HTTP uploads, downloads, URL tokens and consent. It needs ordinary network connectivity rather than AWDL, and the author reports transfers with a real iPhone while Windows runs. This is a credible existing-Wi-Fi alternative, but it is browser sharing and does not use the native AirDrop picker. The user has not authorized replacing the direct AirDrop requirement with it. [Pinned phone-page documentation](https://github.com/UvejsGj/WinDrop/blob/b3afab521c89959a7a33261e08f45c16b549d831/docs/phone-page.md).

That branch intentionally uses plain HTTP on trusted networks, and its author reports Safari converting selected HEIC photos to JPEG. A working browser transfer would therefore not establish AirDrop compatibility or byte-for-byte preservation of camera originals. Those limits must be assessed before adopting it. The branch has not been integrated into Windrop.

## Why ordinary Wi-Fi APIs do not establish AirDrop

Apple documents AirDrop discovery using BLE and peer-to-peer Wi-Fi. The researched OWL/OpenDrop stack uses AWDL, with raw monitor reception, frame injection and channel control. OWL explicitly requires direct radio access. An ordinary LAN socket or Windows Wi-Fi Direct group does not implement those operations. [Apple AirDrop security](https://support.apple.com/guide/security/airdrop-security-sec2261183f4/web), [OWL requirements](https://github.com/seemoo-lab/owl#requirements), [OpenDrop requirements](https://github.com/seemoo-lab/opendrop#requirements).

Microsoft exposes driver-level action-frame tasks in WDI/WiFiCx. Those documents describe the device/OS driver contract; they are not evidence that an ordinary app can request arbitrary AWDL action and data frames on the installed Realtek driver. [WDI action-frame task](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/oid-wdi-task-send-request-action-frame), [WiFiCx device capabilities](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wificx/ns-wificx-wifi_device_capabilities).

Npcap documents raw 802.11 **capture** where the hardware/driver supports it. Its project still tracks raw Wi-Fi injection as an open enhancement. The old Packet11 source is a research lead rather than current RTL8852CE support: its packet-construction routine rejects non-management frames, so it does not provide the complete AWDL data plane unchanged. [Npcap guide](https://npcap.com/guide/npcap-users-guide.html), [Npcap issue 85](https://github.com/nmap/npcap/issues/85), [Packet11 source](https://github.com/Felis-Sapiens/packet11/blob/master/packetio.c).

WSL mirrored networking exposes IP connectivity and multicast; it is not documented as raw PCIe radio ownership. Hyper-V Discrete Device Assignment documents Windows Server hosts and server-class PCIe/IOMMU requirements, with official device-class support for graphics and NVMe. It does not establish a supported consumer Windows/WSL Wi-Fi route. [WSL networking](https://learn.microsoft.com/en-us/windows/wsl/networking), [Discrete Device Assignment](https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/plan/plan-for-deploying-devices-using-discrete-device-assignment).

## New Android interoperability and Wi-Fi Aware

Google's AirDrop interoperability is real and works with Apple's Everyone mode, but Google's Windows instructions still describe Android-to-PC sharing. The independent assessment linked by Google tested `com.google.android.mosey`, OpenDrop interoperability and scoped IPv6 endpoints on `mosey0`/`awdl0`. This supports an AWDL-path inference; the public report does not disclose the firmware implementation. It provides no evidence that Windows Wi-Fi Direct can substitute. [Google security announcement](https://blog.google/security/android-quick-share-support-for-airdrop-security/), [NetSPI assessment](https://www.netspi.com/wp-content/uploads/2025/11/google-feature-review-report.pdf), [Quick Share for Windows](https://support.google.com/android/answer/13801258).

Apple's Wi-Fi Aware framework offers standardized paired P2P connections for apps and accessories on supported iPhones. The documentation does not say an arbitrary Wi-Fi Direct peer appears in native AirDrop. Using this framework would require an iPhone app/accessory pairing flow and a compatible peer implementation. [Apple Wi-Fi Aware](https://developer.apple.com/documentation/WiFiAware).

No current primary source inspected establishes native iPhone AirDrop discovery and initial transfer to an ordinary Windows LAN-only receiver without an AWDL link. That absence is not a universal impossibility proof. A LAN advertisement experiment would still need real iPhone evidence before presenting it as supported.
