#!/usr/bin/env bash
# flash.sh - build usbip-pi.img (if needed) and write it to a block device.
#
#   ./flash.sh /dev/sdX     write to the given block device
#   ./flash.sh              pick a device from a menu
#
# The image is rebuilt automatically when it is missing or older than the
# server configuration.  The build needs a handful of userspace tools; if any
# are missing this script enters the bundled nix shell (shell.nix) to get them.
#
# Environment:
#   USBPI_IMAGE   image path/filename (default: usbip-pi.img)
#   FORCE=1       skip the confirmation prompt
#   (USBPI_* build options are passed straight through to scripts/build-image.sh)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$root"

img="${USBPI_IMAGE:-usbip-pi.img}"
case "$img" in /*) ;; *) img="$root/$img" ;; esac
dev="${1:-}"

log() { printf '>> %s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

case "$dev" in
    -h|--help)
        printf 'usage: %s [block-device]\n\n' "$0"
        printf 'With no argument, an interactive menu of attached drives is shown.\n'
        exit 0
        ;;
esac

# ---------------------------------------------------------------------------
# 1. pick a device
# ---------------------------------------------------------------------------
pick_device() {
    [ -t 0 ] || die "no device given and stdin is not a terminal"
    local -a disks=()
    while IFS= read -r line; do
        [ -n "$line" ] && disks+=("$line")
    done < <(lsblk -dpno NAME,SIZE,TRAN,RM,MODEL 2>/dev/null)

    [ "${#disks[@]}" -gt 0 ] || die "no block devices found"

    printf 'Available block devices:\n'
    local i
    for i in "${!disks[@]}"; do
        printf '  %2d) %s\n' "$((i + 1))" "${disks[$i]}"
    done

    local sel
    printf 'Select a device [1-%d]: ' "${#disks[@]}"
    read -r sel || die "no selection"
    case "$sel" in
        '' | *[!0-9]*) die "invalid selection: '$sel'" ;;
    esac
    [ "$sel" -ge 1 ] && [ "$sel" -le "${#disks[@]}" ] || die "selection out of range"
    dev="$(awk '{print $1}' <<<"${disks[$((sel - 1))]}")"
}

[ -n "$dev" ] || pick_device
[ -b "$dev" ] || die "$dev is not a block device"

# ---------------------------------------------------------------------------
# 2. build the image if it is missing or stale
# ---------------------------------------------------------------------------
needs_build() {
    [ -f "$img" ] || return 0
    [ "$root/scripts/build-image.sh" -nt "$img" ] && return 0
    [ -n "$(find "$root/server" -newer "$img" -print -quit 2>/dev/null)" ] && return 0
    return 1
}

have_build_tools() {
    local t
    for t in curl xz mtools sfdisk python3 openssl sha256sum; do
        command -v "$t" >/dev/null 2>&1 || return 1
    done
}

run_build() {
    if have_build_tools; then
        "$root/scripts/build-image.sh"
    elif command -v nix-shell >/dev/null 2>&1; then
        log "build tools missing - entering nix shell"
        nix-shell "$root/shell.nix" --run "exec \"$root/scripts/build-image.sh\""
    elif command -v nix >/dev/null 2>&1; then
        log "build tools missing - entering nix develop"
        nix develop "$root" -c "$root/scripts/build-image.sh"
    else
        die "build tools missing and nix is not installed"
    fi
}

if needs_build; then
    run_build
else
    log "using existing $img (rebuild with: rm -f \"$img\" && ./flash.sh $dev)"
fi

[ -f "$img" ] || die "$img was not produced"

# ---------------------------------------------------------------------------
# 3. confirm and write
# ---------------------------------------------------------------------------
echo
echo "About to OVERWRITE $dev with $img"
lsblk -o NAME,SIZE,TYPE,FSTYPE,MOUNTPOINT,MODEL "$dev" || true
echo

if [ "${FORCE:-0}" = "1" ]; then
    echo ">> FORCE=1, skipping confirmation"
elif [ -t 0 ]; then
    read -r -p "Type yes to continue: " answer || answer=""
    case "$(printf '%s' "$answer" | tr '[:upper:]' '[:lower:]')" in
        yes | y) ;;
        *)
            echo "aborted"
            exit 1
            ;;
    esac
else
    die "refusing to overwrite $dev without confirmation (set FORCE=1 to override)"
fi

sudo=""
[ -w "$dev" ] || sudo="sudo"

log "writing (this takes a few minutes)"
$sudo dd if="$img" of="$dev" bs=4M conv=fsync status=progress
$sudo sync

echo
echo ">> done. You can now remove the drive and boot the Pi."
echo "   Put the medium in the Pi, plug in the USB devices + Ethernet, and power on."
echo "   First boot reboots once; after that the Pi is reachable at usbip-pi.local."
