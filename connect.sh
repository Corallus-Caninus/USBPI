#!/usr/bin/env bash
# connect.sh - forward every USB device exported by the Pi to this machine.
#
#   ./connect.sh            # keep everything attached (Ctrl-C to stop)
#   ./connect.sh --once     # attach everything once, then exit
#
# The C client does the work: `usbpi keyd` receives keyboards over the fast
# path (uinput) and `usbpi watch` attaches non-keyboard devices over USB/IP.
# This wrapper only builds the binary, self-elevates and supervises both.
#
# Environment overrides:
#   USBIP_SERVER      preferred server name/IP  (default: usbip-pi.lan)
#   USBIP_CANDIDATES  extra fallbacks to try    (default: usbip-pi.local)
#   USBIP_INTERVAL    seconds between checks    (default: 5)
#   USBIP_VIDPID      attach only this device   (default: all)
#   USBIP_GADGET      0 disables the buffered gadget port
#   USBIP_KEYBOARD    0 disables the keyboard fast-path daemon
#   USBIP_ONCE        1 = one pass, then exit
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
server="${USBIP_SERVER:-usbip-pi.lan}"
once="${USBIP_ONCE:-}"

for a in "$@"; do
    [ "$a" = "--once" ] && once=1
done

build_client() {
    if command -v cc >/dev/null 2>&1; then
        "$root/build.sh" client
    elif command -v nix >/dev/null 2>&1; then
        nix develop "$root" -c "$root/build.sh" client
    else
        echo "error: no C compiler found (install gcc/cc or use nix develop)" >&2
        exit 1
    fi
}

if [ ! -x "$root/build/usbpi" ] \
    || [ "$root/build.sh" -nt "$root/build/usbpi" ] \
    || [ -n "$(find "$root/client" "$root/shared" "$root/configuration.c" \
              -newer "$root/build/usbpi" -print -quit 2>/dev/null)" ]; then
    build_client
fi

if [ "$(id -u)" != 0 ]; then
    envs=()
    for v in USBIP_SERVER USBIP_CANDIDATES USBIP_INTERVAL USBIP_VIDPID \
             USBIP_GADGET USBIP_GADGET_PORT USBIP_KEYBOARD USBIP_ONCE; do
        if [ -n "${!v:-}" ]; then
            envs+=("$v=${!v}")
        fi
    done
    exec sudo -E env "${envs[@]}" "$0" "$@"
fi

kpid=""
cleanup() {
    if [ -n "$kpid" ]; then
        kill "$kpid" 2>/dev/null || true
        wait "$kpid" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

if [ -z "$once" ] && [ "${USBIP_KEYBOARD:-1}" != "0" ]; then
    modprobe uinput 2>/dev/null || true
    "$root/build/usbpi" keyd "$server" &
    kpid=$!
fi

"$root/build/usbpi" watch "$@"
