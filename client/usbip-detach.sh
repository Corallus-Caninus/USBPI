#!/usr/bin/env bash
# Detach imported USB/IP devices.
#
#   ./usbip-detach.sh [vid:pid]
#
# With a vendor:product filter, only matching ports are detached; otherwise all
# imported ports are detached.  (Recent kernels do not report the remote host in
# `usbip port`, so filtering by device id is the reliable option.)
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
vidpid="${1:-${USBIP_VIDPID:-}}"

if command -v usbip >/dev/null 2>&1; then
    USBIP="$(command -v usbip)"
elif [ -x "$here/../result/bin/usbip" ]; then
    USBIP="$here/../result/bin/usbip"
else
    nix-build --out-link "$here/../result" "$here/../usbip.nix" >/dev/null \
        || { echo "error: could not build usbip" >&2; exit 1; }
    USBIP="$here/../result/bin/usbip"
fi

ports="$(sudo "$USBIP" port | awk -v vp="$vidpid" '
    /^Port [0-9]+:/ { if (p != "" && hit) print p+0; p=$2; sub(":", "", p); hit=0 }
    vp == "" { hit=1 }
    vp != "" && index($0, "(" vp ")") { hit=1 }
    END { if (p != "" && hit) print p+0 }
')"

if [ -z "$ports" ]; then
    echo "nothing attached${vidpid:+ matching $vidpid}"
    exit 0
fi

for p in $ports; do
    echo ">> detaching port $p"
    sudo "$USBIP" detach -p "$p"
done
