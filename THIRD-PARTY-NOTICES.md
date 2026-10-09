# Third-party notices

The protocol implementation, CLI support code, and original protocol tests in `src/WinDrop.Protocol`, `src/WinDrop.Cli`, and `tests/WinDrop.Protocol.Tests` are adapted from [UvejsGj/WinDrop](https://github.com/UvejsGj/WinDrop), copyright (c) 2026 Uvejs Gjelaj, under the MIT license retained in [LICENSE](LICENSE).

Imported revision: `d34397dbf98b3bfaa3c96b1c9872d6d790dcf029`. Windrop adds its own native desktop interface, authenticated bounded radio relay, readiness checks, integration tests, and build/setup tooling. The existing protocol and its tests are reused rather than represented as original work.

[omdrop-owl](https://github.com/t4t5/omdrop-owl), inspected at `1a952d21f798bd045e505a5dc583f940b025177a`, inspired the radio workflow. Its helper scripts and patches are not copied or bundled.

`scripts/build-owl.sh` fetches [jedbillyb/owl](https://github.com/jedbillyb/owl) at `832d70f815c3d4a06a02117bf0fc5e868daa1ff0`, the revision pinned by omdrop-owl. OWL is GPL-3.0 and remains a separate process with its full source and license in `build/owl-source`. OWL binaries are not included in the Windows package. Redistributors of a radio bundle must provide OWL's corresponding source, including its submodules and notices, and comply with firmware/driver licenses separately.

The optional WSL kernel build uses Microsoft's GPL-2.0 Linux kernel tree; no kernel or firmware is bundled. .NET is MIT-licensed with its own third-party notices. Self-contained publish output retains the runtime notices. xUnit, Microsoft.NET.Test.Sdk, and coverlet are test dependencies only.
