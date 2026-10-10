# Third-party notices

The protocol implementation, CLI support code, and original protocol tests in `src/WinDrop.Protocol`, `src/WinDrop.Cli`, and `tests/WinDrop.Protocol.Tests` are adapted from [UvejsGj/WinDrop](https://github.com/UvejsGj/WinDrop), copyright (c) 2026 Uvejs Gjelaj, under the MIT license retained in [LICENSE](LICENSE).

Imported revision: `d34397dbf98b3bfaa3c96b1c9872d6d790dcf029`. Windrop adds its own native desktop interface, authenticated bounded radio relay, readiness checks, integration tests, and build/setup tooling. The existing protocol and its tests are reused rather than represented as original work.

[omdrop-owl](https://github.com/t4t5/omdrop-owl), inspected at `1a952d21f798bd045e505a5dc583f940b025177a`, inspired the radio workflow. Its helper scripts and patches are not copied or bundled.

`scripts/build-owl.sh` fetches [jedbillyb/owl](https://github.com/jedbillyb/owl) at `832d70f815c3d4a06a02117bf0fc5e868daa1ff0`, the revision pinned by omdrop-owl. OWL is GPL-3.0-or-later. The native `windrop-awdl` worker links its protocol core with the included Windows patches under that license. The ordinary desktop package contains no OWL binary; the separate native development package includes the worker, full license, pinned source archive, radiotap submodule source and exact port patches. The Linux build retains its source in `build/owl-source`.

The native hardware worker links [OpenIPC/devourer](https://github.com/OpenIPC/devourer) at `f369aac553c9ce884901115ef9e66509f93c5683`, GPL-2.0-only. Its chip firmware/tables are retained with their upstream notices. `drivers/Windrop.Radio/radio.c` adapts its AX DMA implementation. The kernel driver and `native/hardware` are GPL-2.0-only; `native/awdl` is GPL-3.0-or-later. These components are not covered by the root MIT license. The hardware and AWDL workers run separately over generic raw-frame IPC, rather than linking GPL2-only code into the GPL3 worker. Full licenses are in `native/licenses` and `drivers/Windrop.Radio/LICENSE`.

OWL's radiotap parser is pinned at `94984dd829a605a2d7af55241145c159607f3b30`, under its ISC license. libusb 1.0.29 (`15a7ebb4d426c5ce196684347d2b7cafad862626`) is a dynamically linked dependency of Devourer, under LGPL-2.1-or-later; its full license and corresponding source are included in the native package. The Windows PCIe backend does not use libusb to control the card. libusb may be replaced or rebuilt independently.

Wintun 0.14.1 is used only through its public API. The API header's MIT option is selected (copyright WireGuard LLC, 2018–2021); the unmodified, WireGuard-signed DLL is redistributed alongside software using that API under its supplied Prebuilt Binaries License. That license is copied into the package. No GPL2 Wintun example loader code was copied. CMake 3.31.8 is downloaded as a build tool, not bundled with the runtime. Source and licenses accompany the native development bundle; redistributors must retain these and the applicable upstream notices.

The optional WSL kernel build uses Microsoft's GPL-2.0 Linux kernel tree; no kernel or firmware is bundled. .NET is MIT-licensed with its own third-party notices. Self-contained publish output retains the runtime notices. xUnit, Microsoft.NET.Test.Sdk, and coverlet are test dependencies only.
