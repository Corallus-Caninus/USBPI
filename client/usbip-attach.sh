#!/usr/bin/env bash
# Attach a device exported by the USB/IP server.
#
#   ./usbip-attach.sh [server] [vid:pid]
#
# server defaults to usbip-pi.local.  If vid:pid is omitted the first exported
# device is used.  Builds the matching usbip binary automatically if it is not
# already in PATH, and resolves mDNS (.local) names even without nss-mdns.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
server="${1:-${USBIP_SERVER:-usbip-pi.local}}"
vidpid="${2:-${USBIP_VIDPID:-}}"

# --- locate (or build) usbip -------------------------------------------------
if command -v usbip >/dev/null 2>&1; then
    USBIP="$(command -v usbip)"
elif [ -x "$here/../result/bin/usbip" ]; then
    USBIP="$here/../result/bin/usbip"
else
    echo ">> usbip not found; building it from the configured kernel (one time) ..."
    nix-build --out-link "$here/../result" "$here/../usbip.nix" >/dev/null \
        || { echo "error: could not build usbip" >&2; exit 1; }
    USBIP="$here/../result/bin/usbip"
fi
echo ">> using $USBIP"

# --- resolve the server (mDNS fallback when nss-mdns is not enabled) ---------
resolve_server() {
    local h="$1" ip
    if getent hosts "$h" >/dev/null 2>&1; then
        printf '%s\n' "$h"
        return 0
    fi
    if command -v avahi-resolve >/dev/null 2>&1; then
        ip="$(avahi-resolve -n "$h" -4 2>/dev/null | awk '{print $2}')"
        if [ -n "$ip" ]; then
            printf '%s\n' "$ip"
            return 0
        fi
    fi
    printf '%s\n' "$h"
}

target="$(resolve_server "$server")"
if [ "$target" != "$server" ]; then
    echo ">> $server resolves to $target"
elif ! getent hosts "$target" >/dev/null 2>&1; then
    echo "warning: could not resolve '$server'" >&2
    echo "         (is the Pi up? is avahi running here? otherwise pass its IP)" >&2
fi

echo ">> devices exported by $target:"
listing="$("$USBIP" list -r "$target")" || {
    echo "error: could not list devices on $server (network / firewall / usbipd?)" >&2
    exit 1
}
printf '%s\n' "$listing"

# --- pick the busid ----------------------------------------------------------
# NB: `usbip list -p` is only parsable with --local; for --remote the output is
# human-readable, with lines like: "1-1.3: Dell Computer Corp. : KB216 (413c:2113)"
if [ -n "$vidpid" ]; then
    busid="$(printf '%s\n' "$listing" \
        | sed -n "s/^[[:space:]]*\([0-9][0-9.-]*\):.*(${vidpid})[[:space:]]*$/\1/p" | head -n1)"
    [ -n "$busid" ] || { echo "error: no exported device matches $vidpid" >&2; exit 1; }
else
    busid="$(printf '%s\n' "$listing" \
        | sed -n 's/^[[:space:]]*\([0-9][0-9.-]*\):.*/\1/p' | head -n1)"
    [ -n "$busid" ] || { echo "error: $server exports no devices" >&2; exit 1; }
fi

# --- attach ------------------------------------------------------------------
echo ">> loading vhci-hcd and attaching $target busid $busid"
sudo modprobe vhci-hcd
sudo "$USBIP" attach -r "$target" -b "$busid"
echo ">> attached; current ports:"
sudo "$USBIP" port
