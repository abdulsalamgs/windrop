# Local Windows driver testing

The desktop can run, but direct iPhone AirDrop requires the physical radio
driver to load and pass hardware tests. The current PC has Secure Boot enabled
and the original Realtek driver installed. Opening the app alone cannot enable
this radio backend.

A paid EV certificate is not required to prepare a local development test.
`prepare-test-driver.ps1` creates a self-signed code-signing certificate in the
current user's personal certificate store with a non-exportable private key.
It copies the unsigned INF/SYS, signs the SYS, regenerates its catalog, signs
the catalog and exports only the public certificate. It never trusts that
certificate, installs the driver, changes boot settings or reboots.

```powershell
./scripts/build-driver.ps1
./scripts/prepare-test-driver.ps1 -OutputDirectory ./publish/my-test-driver
./scripts/verify-test-driver.ps1 -PackageDirectory ./publish/my-test-driver
./scripts/backup-radio-driver.ps1 -OutputDirectory ./publish/my-radio-recovery
```

Existing output directories are refused. The unsigned build is preserved.
Verification checks file hashes, the public certificate, and both embedded
signatures. It permits only WinVerifyTrust success or the expected untrusted-root
error for that exact signer. It rejects a broken signature even if the manifest
hash is rewritten. These checks establish package consistency, not trusted
provenance or permission to load a kernel driver.

Keep recovery exports offline and outside Git: `recovery.json` identifies the
actual device and hashes the exported original driver. `RECOVERY.txt` describes
selecting its original INF through Device Manager. Recovery after a real
experimental-driver installation has not yet been qualified.

## Separate loading decision

Windows blocks development test-signed drivers by default. On this Secure Boot
PC, the development route requires disabling Secure Boot in firmware, enabling
test-signing mode and restarting Windows. Driver-package certificate trust and
binding the Wi-Fi device are additional system changes. None are performed by
the preparation scripts. A normally signed release uses Microsoft's submission
process instead. [Microsoft test-signing requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/the-testsigning-boot-configuration-option),
[Microsoft submission requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-reqs).

Before those changes, verify access to Windows recovery and any BitLocker
recovery key, retain the original-driver export, and use Ethernet for recovery.
Binding Windrop replaces ordinary Wi-Fi on this card. The experimental DMA/reset
failure paths can deliberately crash Windows; signing does not establish that
the driver works or that an iPhone transfer will succeed. See
[driver qualification](WINDOWS-DRIVER.md#qualification-gate).

The next hardware gate is loaded diagnostics and function-reset support, then
firmware initialization, real frame TX/RX and AWDL timing. Only afterward test
iPhone discovery, consent and byte-exact delivery. Installation and boot-security
changes require a separate explicit decision after reviewing this package.
