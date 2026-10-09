#!/usr/bin/env bash
# Optional advanced build. Produces files only; never edits .wslconfig or stops WSL.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source="$root/build/wsl-kernel"
commit=c21a03b2943d147c280bdf32530d4fe6badfd6bd
[[ $(uname -m) == x86_64 ]] || { echo 'This build targets x64 WSL2 only.' >&2; exit 2; }
[[ ! -e "$source" ]] || { echo 'build/wsl-kernel already exists; keep its data and choose a fresh checkout for a rebuild.' >&2; exit 2; }
git clone --depth 1 --branch linux-msft-wsl-6.18.33.2 https://github.com/microsoft/WSL2-Linux-Kernel.git "$source"
[[ $(git -C "$source" rev-parse HEAD) == "$commit" ]] || { echo 'Kernel pin mismatch.' >&2; exit 1; }
cd "$source"
cp Microsoft/config-wsl .config
scripts/config --enable WLAN --enable WLAN_VENDOR_MEDIATEK --module CFG80211 --module MAC80211 --module MT76x2U
make olddefconfig
grep -q '^CONFIG_MT76x2U=m$' .config || { echo 'MT7612U was not enabled; inspect kernel dependencies.' >&2; exit 1; }
make -j "${WINDROP_BUILD_JOBS:-4}"
make INSTALL_MOD_PATH="$source/modules" modules_install
sudo Microsoft/scripts/gen_modules_vhdx.sh "$source/modules" "$(make -s kernelrelease)" "$source/modules.vhdx"
echo "Kernel: $source/arch/x86/boot/bzImage"
echo "Modules: $source/modules.vhdx"
echo 'Copy these to Windows and configure kernel/kernelModules manually. See docs/RADIO-SETUP.md.'
