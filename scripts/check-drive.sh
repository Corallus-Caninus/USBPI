#!/usr/bin/env bash
# Read-only diagnosis of a flashed USBPI drive.
#
#   ./scripts/check-drive.sh /dev/sdX
#
# Answers: was the image written intact, and did the Pi ever boot it?
# (If it boots, firstrun.sh removes the systemd.run hook from cmdline.txt and
# writes /usbpi/firstrun.log to the boot partition.)
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dev="${1:-}"
img="$here/usbip-pi.img"

[ -n "$dev" ] || { echo "usage: $0 <block-device>" >&2; exit 1; }
[ -b "$dev" ] || { echo "error: $dev is not a block device" >&2; exit 1; }

# FAT partition of our image starts at 8 MiB; 512 MiB long
FAT_OFF_MB=8
FAT_LEN_MB=512

export MTOOLS_SKIP_CHECK=1

src=""
tmpf=""

cleanup() { [ -n "$tmpf" ] && rm -f "$tmpf"; }
trap cleanup EXIT

if mdir -i "${dev}@@$((FAT_OFF_MB*1024*1024))" ::/ >/dev/null 2>&1; then
    src="${dev}@@$((FAT_OFF_MB*1024*1024))"
    echo ">> reading boot partition directly from $dev"
else
    tmpf="$(mktemp /tmp/usbpi-fat.XXXXXX.img)"
    echo ">> copying boot partition from $dev to $tmpf (read-only)"
    if dd if="$dev" of="$tmpf" bs=1M count=$FAT_LEN_MB skip=$FAT_OFF_MB status=progress 2>&1; then
        :
    else
        echo ">> direct dd failed, retrying with sudo"
        sudo dd if="$dev" of="$tmpf" bs=1M count=$FAT_LEN_MB skip=$FAT_OFF_MB status=progress
        sudo chown "$(id -u):$(id -g)" "$tmpf"
    fi
    src="$tmpf"
fi

echo
echo "=== cmdline.txt ==="
mtype -i "$src" ::/cmdline.txt 2>&1 || true

echo
echo "=== firstrun.log ==="
if mtype -i "$src" ::/usbpi/firstrun.log 2>/dev/null; then
    echo
    echo "VERDICT: the Pi BOOTED this drive and ran firstrun.sh (see log above)"
else
    echo "  (not present)"
    echo
    echo "VERDICT: NO firstrun.log -> the Pi has NOT booted this drive."
fi

echo
echo "=== /usbpi on the drive ==="
mdir -i "$src" ::/usbpi 2>&1 | tail -20

echo
if [ "${VERIFY:-0}" = "1" ] && [ -f "$img" ]; then
    echo "=== read-back integrity (reads ~2.6 GiB) ==="
    isize=$(stat -c %s "$img")
    echo ">> hashing first $isize bytes of $dev and comparing to usbip-pi.img"
    want="$(sha256sum "$img" | awk '{print $1}')"
    got="$(head -c "$isize" "$dev" | sha256sum | awk '{print $1}')"
    if [ "$want" = "$got" ]; then
        echo ">> OK: drive content matches usbip-pi.img"
    else
        echo ">> MISMATCH: want $want got $got"
    fi
else
    echo "(re-run with VERIFY=1 ./scripts/check-drive.sh $dev to checksum the whole image)"
fi
