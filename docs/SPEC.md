# Windrop scope

Create a Windows desktop replacement for omdrop-owl's AirDrop workflow. The user requires direct Apple AirDrop, not browser sharing.

Windows runs the AirDrop sender and receiver. A Linux AWDL radio bridge runs on a dedicated USB Wi-Fi adapter in WSL2 or on a Linux host. The built-in Realtek 8852CE is not claimed as supported. No hardware is purchased or network/driver configuration changed automatically.

Requirements:
- Native desktop: discover peers, select/drop files, send, approve or decline incoming transfers, open the download folder, connect and disconnect the radio.
- CLI: browse, send, receive, and doctor with actionable readiness diagnostics.
- No silent fallback to an ordinary LAN when an AirDrop radio is missing.
- Radio access is authenticated with an injected random key. Only one controller owns the radio; outgoing connections are restricted to the AWDL link. Keys and private paths are excluded from evidence.
- File consent is enforced by the receiver on the same TLS connection. Interrupted uploads leave no partial files. Existing files are preserved.
- Keep protocol code reused under its existing MIT license, with explicit attribution and pinned upstream revisions.
- Provide source build and Windows publish scripts, radio setup instructions, automated public CLI/bridge tests, and a public GitHub PR.

Agreed test boundaries: public CLI and radio bridge, including readiness, discovery start/stop, consent, and byte-for-byte delivery. Real iPhone interoperability and throughput require hardware testing; passing a loopback test does not establish either.

The user subsequently required using the built-in RTL8852CE while Windows stays running. A native driver port is being developed toward that requirement. Its current increment is limited to a buildable KMDF identity query and public CLI diagnostics; it must report unavailable for AirDrop until a raw frame plane, AWDL and real hardware qualification exist. Building source does not authorize replacing the installed driver or changing Secure Boot/test-signing policy. See [WINDOWS-DRIVER.md](WINDOWS-DRIVER.md).
