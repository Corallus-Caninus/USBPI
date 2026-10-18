#!/usr/bin/env bash
# Attach a device exported by the USB/IP server.
#
#   ./usbip-attach.sh [server] [vid:pid]
#
# server defaults to usbip-pi.local.  If vid:pid is omitted the first exported
# device is used.  Builds the matching usbip binary automatically if it is not
# already in PATH, and resolves mDNS (.local) names even without nss-mdns.
#
# Buffered HID devices (keyboards, mice) are exported by the Pi's HID mirror on
# a second daemon (device mode, default port 3241); this script looks there too
# unless USBIP_GADGET=0.
#
# Environment:
#   USBIP_PORT         main USB/IP port (default 3240)
#   USBIP_GADGET       1 = also look at buffered HID gadgets (default 1)
#   USBIP_GADGET_PORT  device-mode daemon port (default 3241)
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
server="${1:-${USBIP_SERVER:-usbip-pi.local}}"
vidpid="${2:-${USBIP_VIDPID:-}}"
port="${USBIP_PORT:-3240}"
gadget="${USBIP_GADGET:-1}"
gadget_port="${USBIP_GADGET_PORT:-3241}"

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

list_remote() { # port
    "$USBIP" --tcp-port "$1" list -r "$target" 2>/dev/null
}

# --- pick the busid ----------------------------------------------------------
# NB: `usbip list -p` is only parsable with --local; for --remote the output is
# human-readable, with lines like:
#   "1-1.3: Dell Computer Corp. : KB216 (413c:2113)"
#   "usbip-vudc.0: Dell Computer Corp. : KB216 (413c:2113)"
BUSID_RE='[0-9][0-9.-]*\|usbip-vudc\.[0-9]*'

pick_busid() { # listing
    local listing="$1"
    if [ -n "$vidpid" ]; then
        printf '%s\n' "$listing" \
            | sed -n "s/^[[:space:]]*\\(${BUSID_RE}\\):.*(${vidpid})[[:space:]]*$/\\1/p" | head -n1
    else
        printf '%s\n' "$listing" \
            | sed -n "s/^[[:space:]]*\\(${BUSID_RE}\\):.*/\\1/p" | head -n1
    fi
}

echo ">> devices exported by $target:"
listing="$(list_remote "$port")" || {
    echo "error: could not list devices on $server (network / firewall / usbipd?)" >&2
    exit 1
}
printf '%s\n' "$listing"

busid="$(pick_busid "$listing")"
if [ -z "$busid" ] && [ "$gadget" != "0" ]; then
    echo ">> no match on port $port; checking buffered HID gadgets on $gadget_port"
    listing="$(list_remote "$gadget_port")" || listing=""
    if [ -n "$listing" ]; then
        printf '%s\n' "$listing"
    fi
    busid="$(pick_busid "$listing")"
    if [ -n "$busid" ]; then
        port="$gadget_port"
    fi
fi

if [ -z "$busid" ]; then
    if [ -n "$vidpid" ]; then
        echo "error: no exported device matches $vidpid" >&2
    else
        echo "error: $server exports no devices" >&2
    fi
    exit 1
fi

# --- attach ------------------------------------------------------------------
args=()
if [ "$port" != "3240" ]; then
    args=(--tcp-port "$port")
fi
echo ">> loading vhci-hcd and attaching $target busid $busid"
sudo modprobe vhci-hcd
sudo "$USBIP" "${args[@]}" attach -r "$target" -b "$busid"
echo ">> attached; current ports:"
sudo "$USBIP" port
