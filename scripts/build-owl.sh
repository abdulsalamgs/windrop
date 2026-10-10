#!/usr/bin/env bash
# Source-only build: downloads the exact OWL revision used by omdrop-owl.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build="$root/build/owl-source"
commit=832d70f815c3d4a06a02117bf0fc5e868daa1ff0
command -v cmake >/dev/null || { echo 'Install cmake, build-essential, libpcap-dev, libev-dev, libnl-3-dev, libnl-genl-3-dev and libnl-route-3-dev.' >&2; exit 1; }
if [[ ! -d "$build" ]]; then
  git clone https://github.com/jedbillyb/owl.git "$build"
fi
[[ $(git -C "$build" rev-parse HEAD) == "$commit" ]] || {
  [[ -z $(git -C "$build" status --porcelain) ]] || { echo 'OWL source has local changes; refusing to replace them.' >&2; exit 1; }
  git -C "$build" checkout --detach "$commit"
}
git -C "$build" submodule update --init --recursive
cmake -S "$build" -B "$root/build/owl" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build "$root/build/owl" --target owl -j "$(nproc)"
echo "Built $root/build/owl/daemon/owl. OWL is GPL-3.0; its complete source remains in $build."
