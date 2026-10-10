#!/usr/bin/env bash
# Owns only the two processes this invocation starts. Never reloads a driver.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
interface=${1:-}
channel=${2:-44}
[[ $EUID == 0 ]] || { echo 'Run with sudo --preserve-env=WINDROP_RADIO_KEY.' >&2; exit 2; }
[[ $interface =~ ^[a-zA-Z0-9_-]+$ ]] || { echo 'Usage: start-radio.sh <dedicated-wifi-interface> [6|44|149]' >&2; exit 2; }
[[ $channel == 6 || $channel == 44 || $channel == 149 ]] || { echo 'Choose AWDL channel 6, 44 or 149.' >&2; exit 2; }
radio_key=${WINDROP_RADIO_KEY:-}
[[ ${#radio_key} -ge 32 ]] || { echo 'Set WINDROP_RADIO_KEY to the key used by the Windows app.' >&2; exit 2; }
[[ -x "$root/build/owl/daemon/owl" && -x "$root/publish/radio/windrop-radio" ]] || { echo 'Build OWL and publish the radio first. See docs/RADIO-SETUP.md.' >&2; exit 4; }
iw dev "$interface" info >/dev/null
if iw dev "$interface" link | grep -q 'Connected to'; then
  echo 'This adapter is connected to Wi-Fi. Use a dedicated disconnected USB adapter; your internet connection must use another adapter.' >&2
  exit 3
fi
if ip link show awdl0 >/dev/null 2>&1; then
  echo 'awdl0 already exists. Stop its owner first; this script will not replace it.' >&2
  exit 5
fi
owl_pid= radio_pid=
cleanup() {
  trap - EXIT INT TERM
  [[ -z $radio_pid ]] || { kill -TERM "$radio_pid" 2>/dev/null || true; wait "$radio_pid" 2>/dev/null || true; }
  [[ -z $owl_pid ]] || { kill -TERM "$owl_pid" 2>/dev/null || true; wait "$owl_pid" 2>/dev/null || true; }
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
"$root/build/owl/daemon/owl" -i "$interface" -c "$channel" -S intersect -v &
owl_pid=$!
ready=false
for ((attempt=0; attempt<100; attempt++)); do
  kill -0 "$owl_pid" 2>/dev/null || { echo 'OWL exited. Check monitor/injection support and permitted channels.' >&2; exit 4; }
  if ip -6 addr show dev awdl0 scope link 2>/dev/null | grep -q 'inet6 fe80:.*'; then ready=true; break; fi
  sleep 0.1
done
[[ $ready == true ]] || { echo 'OWL did not create a usable awdl0.' >&2; exit 4; }
"$root/publish/radio/windrop-radio" --interface awdl0 --listen "${WINDROP_RADIO_LISTEN:-127.0.0.1}" &
radio_pid=$!
wait "$radio_pid"
