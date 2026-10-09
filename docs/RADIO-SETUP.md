# Radio setup

Windrop's Windows process implements AirDrop's TLS, Discover/Ask/Upload exchange, and file archives. OWL supplies the Wi-Fi link, and `windrop-radio` relays discovery and TCP bytes. The bridge uses no file keys and does not terminate AirDrop TLS. Everyone mode's peer certificates and display names are not authenticated; a malicious bridge or nearby attacker can impersonate peers. Consent remains required.

## Hardware choice

The development PC has a Realtek 8852CE PCIe adapter, Windows driver `6001.16.172.0`, and Ethernet. No compatible dedicated USB radio was connected. The built-in card is **not claimed as supported**, and PCIe Wi-Fi cannot be attached to WSL through USB/IP.

The first hardware candidate is **ALFA AWUS036ACM**, MediaTek MT7612U, USB ID `0e8d:7612`. ALFA documents [Linux support with the in-kernel driver](https://docs.alfa.com.tw/Support/Linux/MT7612U/); this identifies a plausible driver-backed candidate, **not a successful Windrop/AirDrop qualification**. Confirm the exact USB ID, active monitor mode, injection, and permitted channels before relying on it. An adapter with a similar product name or a different revision may use another chipset.

[SwooshDrop's hardware beta](https://github.com/AlbinoPriest/swooshdrop) reports successful receiving with TP-Link TL-WN725N, USB ID `0bda:8179`, through a dedicated WSL runtime and an additional driver. That result belongs to SwooshDrop; its installer/driver bundle is not part of Windrop. It does not validate this bridge or our send path.

Keep internet on Ethernet or a separate Wi-Fi card. The AirDrop radio is reserved while active. OWL's [requirements](https://github.com/seemoo-lab/owl#requirements) call for monitor mode and frame injection. The pinned fork used here incorporates newer channel scheduling work; hardware still determines reliability and speed.

## Linux radio host

Use a Linux host with the USB adapter and a kernel that supplies its driver and firmware. Ubuntu/Debian build prerequisites:

```bash
sudo apt update
sudo apt install build-essential cmake git pkg-config iw libpcap-dev libev-dev libnl-3-dev libnl-genl-3-dev libnl-route-3-dev
```

From a Linux checkout of Windrop, with .NET 8 SDK installed:

```bash
bash scripts/build-owl.sh
dotnet publish src/Windrop.Radio -c Release -r linux-x64 --self-contained true -o publish/radio
iw dev
iw phy phy0 info  # choose the USB adapter's actual phy; check monitor capabilities and channel permissions
```

Generate a 32-byte random key on one machine. In Windows PowerShell:

```powershell
$radioBytes = New-Object byte[] 32
[Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($radioBytes)
$env:WINDROP_RADIO_KEY = [BitConverter]::ToString($radioBytes).Replace('-', '')
```

Copy that key securely into the radio's `WINDROP_RADIO_KEY` environment variable. Do not place it in command-line arguments, logs, source files, or evidence. Prefer WSL local forwarding. If using a separate host, use a trusted private/VPN connection: bridge control messages, including the access key, are **not encrypted by the relay**. Never expose its ports to the internet.

```bash
read -rsp 'Radio key: ' WINDROP_RADIO_KEY; echo; export WINDROP_RADIO_KEY
# Use the actual dedicated, disconnected adapter name and an allowed AWDL channel.
sudo --preserve-env=WINDROP_RADIO_KEY bash scripts/start-radio.sh wlan0 44
```

The launcher starts OWL, waits for an IPv6 link-local address on `awdl0`, and starts the relay on loopback. Ctrl+C stops only its own relay and OWL processes, in that order. It refuses a connected adapter or an existing `awdl0`; it never reloads a driver or changes an existing radio session. Channel 6 or 149 may be used if permitted and appropriate for the peer. OWL enforces the adapter's regulatory restrictions.

For a separate Linux host, explicitly set `WINDROP_RADIO_LISTEN` to that host's private address and preserve it with sudo. Set `WINDROP_RADIO_HOST` on Windows to the same address. Restrict TCP 7788/7789 to the Windows host. Allow the radio host to connect back to the Windows process's dynamic inbound TCP port; create an application-scoped Windows firewall rule for Windrop if prompted. Never disable the firewall globally.

## WSL2 on the same Windows PC

Use a normal WSL2 Linux distribution, not Docker Desktop's managed distribution. Follow Microsoft's [USB passthrough instructions](https://learn.microsoft.com/en-us/windows/wsl/connect-usb) to install usbipd-win, identify the dedicated adapter's bus ID, bind it in an elevated shell, and attach it:

```powershell
usbipd list
usbipd bind --busid <dedicated-adapter-busid>
usbipd attach --wsl --busid <dedicated-adapter-busid>
```

An adapter attached to WSL is unavailable to Windows until detached. Verify inside WSL with `lsusb`, `modprobe mt76x2u`, and `iw dev`. **USB passthrough alone does not install a Wi-Fi driver.** If the running WSL kernel lacks the module, stop here or use a Linux host with the driver. Do not force-load a module built for another kernel.

An optional x64 WSL kernel build helper is provided for MT7612U. It follows [Microsoft's kernel build procedure](https://github.com/microsoft/WSL2-Linux-Kernel/blob/linux-msft-wsl-6.18.33.2/README.md), pins commit `c21a03b2943d147c280bdf32530d4fe6badfd6bd`, and adds the wireless/MT7612U configuration. It has been syntax checked; **the kernel build and USB-radio path have not been executed in this project**. Building can take substantial disk space and time:

```bash
sudo apt install build-essential flex bison dwarves libssl-dev libelf-dev cpio qemu-utils
bash scripts/build-wsl-kernel.sh
```

Copy the reported `bzImage` and `modules.vhdx` into a chosen Windows directory. Preserve your existing `.wslconfig` and merge these fields under its `[wsl2]` section using absolute Windows paths with escaped backslashes:

```ini
[wsl2]
kernel=C:\\path\\to\\bzImage
kernelModules=C:\\path\\to\\modules.vhdx
```

Custom kernel settings apply to every WSL2 distribution, including Docker. Schedule any `wsl --shutdown` yourself after saving work; the helper never does it. Remove only those custom settings to return to Microsoft's default kernel. Restart WSL, reattach the USB adapter, install its firmware if needed, and recheck `modprobe mt76x2u` and `iw dev`.

For networking, Windows normally reaches WSL services through `localhost`; the relay's reverse connection must also reach Windows. Microsoft's [WSL networking guide](https://learn.microsoft.com/en-us/windows/wsl/networking) describes NAT, mirrored networking, and firewall behavior. With NAT, bind the bridge to the WSL VM's private address and configure that address in Windows so the bridge sees the Windows gateway address for callbacks. Mirrored mode supports localhost communication on Windows 11 22H2 and later. Do not change existing WSL networking settings automatically.

## First real-device qualification

1. Start OWL and confirm an AWDL peer is logged while the iPhone's AirDrop sheet is open.
2. Start `windrop-radio`, then connect Windrop with the same key. Do not restart OWL underneath a connected app.
3. Set the iPhone to Everyone for 10 Minutes with Wi-Fi and Bluetooth on.
4. Send one small file to Windows; approve it and compare the original and received SHA-256 hashes.
5. Send a file back from Windrop and confirm its content on the Apple device.
6. Decline a transfer, interrupt one, try several files, check collision preservation, then test larger files and throughput.
7. Disconnect Windrop before stopping the radio so mDNS goodbyes can withdraw its service. Detach USB when finished.

Record the exact adapter USB ID, driver/kernel, channel, iOS version, success/failure counts, and hashes of nonprivate fixtures. Software loopback tests do not replace this qualification.
