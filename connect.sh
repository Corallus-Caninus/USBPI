#!/usr/bin/env bash
# connect.sh - forward every USB device exported by the Pi to this machine.
#
#   ./connect.sh            # keep everything attached (Ctrl-C to stop)
#   ./connect.sh --once     # attach everything once, then exit
#
# It re-runs itself with sudo, finds the Pi by DNS name (usbip-pi.lan), mDNS
# (usbip-pi.local) or IP, loads vhci-hcd, and attaches every exported device,
# re-attaching automatically whenever the Pi or a device comes back.
#
# The matching `usbip` userspace tool is built from your running kernel the
# first time if it is not already available.
#
# Environment overrides:
#   USBIP_SERVER      preferred server name/IP  (default: usbip-pi.lan)
#   USBIP_CANDIDATES  extra fallbacks to try    (default: usbip-pi.local)
#   USBIP_INTERVAL    seconds between checks    (default: 5)
#   USBIP_VIDPID      attach only this device, e.g. 13ba:0018 (default: all)
#
# For boot-time persistence use the NixOS module (client/usbip-client.nix).
set -u

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

server="${USBIP_SERVER:-usbip-pi.lan}"
candidates="${USBIP_CANDIDATES:-usbip-pi.local}"
interval="${USBIP_INTERVAL:-5}"
vidpid="${USBIP_VIDPID:-}"

once=""
for a in "$@"; do
    [ "$a" = "--once" ] && once=1
done

# build the usbip tool if it is neither on PATH nor already built here
ensure_usbip() {
    command -v usbip >/dev/null 2>&1 && return 0
    [ -x "$root/result/bin/usbip" ] && return 0
    if command -v nix-build >/dev/null 2>&1; then
        echo ">> building usbip from the running kernel (one time) ..."
        nix-build --out-link "$root/result" "$root/usbip.nix" >/dev/null || true
    elif command -v nix >/dev/null 2>&1; then
        echo ">> building usbip from the running kernel (one time) ..."
        nix build --out-link "$root/result" --file "$root/usbip.nix" >/dev/null || true
    fi
}

# environment the watcher should see
envs=(
    "USBIP_SERVER=$server"
    "USBIP_CANDIDATES=$candidates"
    "USBIP_INTERVAL=$interval"
)
if [ -n "$vidpid" ]; then
    envs+=("USBIP_VIDPID=$vidpid")
else
    envs+=(USBIP_ALL=1)
fi
[ -n "$once" ] && envs+=(USBIP_ONCE=1)

if [ "$(id -u)" != 0 ]; then
    ensure_usbip
    exec sudo "${envs[@]}" "$root/client/usbip-sync.sh"
fi
exec env "${envs[@]}" "$root/client/usbip-sync.sh"
