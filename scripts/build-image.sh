#!/usr/bin/env bash
# Build usbip-pi.img - Raspberry Pi OS Lite pre-configured as a zero-touch
# USB/IP server for the Raspberry Pi 3 Model B.
#
# No root, no loop mounts: the FAT boot partition inside the raw image is
# edited in place with mtools.  All server configuration is delivered as the
# `/usbpi` directory on the boot partition; a first-run script (triggered via
# the same `systemd.run=` mechanism Raspberry Pi Imager uses) installs it and
# a one-shot service installs the usbip package on the next boot.
#
# Normally invoked automatically by ../flash.sh (which enters the nix shell
# if a tool is missing).  To build by hand:  nix-shell --run ./scripts/build-image.sh
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

IMG_URL="${USBPI_IMAGE_URL:-https://downloads.raspberrypi.com/raspios_lite_armhf/images/raspios_lite_armhf-2026-10-06/2026-10-06-raspios-trixie-armhf-lite.img.xz}"
IMG="${USBPI_IMAGE:-usbip-pi.img}"
# FAT partition mount point inside the running Pi OS (Bookworm/Trixie)
BOOT_MOUNT="${USBPI_BOOT_MOUNT:-/boot/firmware}"
SERVER_DIR="server"
SSH_KEY="${USBPI_SSH_KEY:-$HOME/.ssh/id_ed25519.pub}"
# set to 0 to disable TCP-wrapper restriction (export to whole LAN)
LOCKDOWN="${USBPI_LOCKDOWN:-1}"

log()  { printf '>> %s\n' "$*"; }
die()  { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

for t in curl xz mtools sfdisk python3 openssl sha256sum; do
    command -v "$t" >/dev/null 2>&1 || die "missing tool '$t' - run via ./flash.sh (auto nix shell) or inside 'nix develop'"
done

# cross compiler for the armv7 (Raspberry Pi OS armhf) HID mirror daemon
ARM_CC="${USBPI_ARM_CC:-}"
if [ -z "$ARM_CC" ]; then
    for c in armv7l-unknown-linux-gnueabihf-gcc arm-linux-gnueabihf-gcc; do
        if command -v "$c" >/dev/null 2>&1; then
            ARM_CC="$(command -v "$c")"
            break
        fi
    done
fi
[ -n "$ARM_CC" ] || die "missing armv7 cross compiler (armv7l-unknown-linux-gnueabihf-gcc) - run via ./flash.sh (auto nix shell) or inside 'nix develop'"

mkdir -p build

# ---------------------------------------------------------------------------
# 1. download the base image (resumable)
# ---------------------------------------------------------------------------
src="build/base.img.xz"

if [ -s "$src" ] && xz -t "$src" 2>/dev/null; then
    log "using cached image $src"
else
    [ -f "$src" ] && log "resuming incomplete download" || log "downloading $IMG_URL"
    curl -fL --retry 5 --retry-delay 3 -C - -o "$src" "$IMG_URL"
    xz -t "$src" || die "downloaded image is corrupt: $src"
fi

# verify against the published sha256 if we can fetch it
if [ ! -f "$src.ok" ]; then
    if curl -fsSL -o build/base.sha256 "$IMG_URL.sha256" 2>/dev/null; then
        expected="$(awk '{print $1}' build/base.sha256)"
        actual="$(sha256sum "$src" | awk '{print $1}')"
        if [ -n "$expected" ] && [ "$expected" != "$actual" ]; then
            die "sha256 mismatch: expected $expected got $actual"
        fi
        log "sha256 verified"
    else
        log "WARNING: could not fetch sha256, skipping verification"
    fi
    touch "$src.ok"
fi

# ---------------------------------------------------------------------------
# 2. decompress
# ---------------------------------------------------------------------------
if [ ! -f "$IMG" ] || [ "$src" -nt "$IMG" ]; then
    log "decompressing to $IMG (about 4 GiB, one time)"
    xz -dc "$src" > "$IMG.part"
    mv "$IMG.part" "$IMG"
fi

# ---------------------------------------------------------------------------
# 2b. cross-compile the buffered HID mirror daemon (armv7, static)
# ---------------------------------------------------------------------------
ARM_STATIC_LIB="${USBPI_ARM_STATIC_LIB:-}"
arm_ldflags=(-static)
if [ -n "$ARM_STATIC_LIB" ]; then
    arm_ldflags+=(-L "$ARM_STATIC_LIB")
fi
log "building usbpi-hid-mirror (armv7, static) with $ARM_CC"
"$ARM_CC" -O2 -Wall -Wextra -Wno-format-truncation "${arm_ldflags[@]}" \
    -o build/usbpi-hid-mirror "$SERVER_DIR/usbpi-hid-mirror.c" \
    || die "cross-compiling usbpi-hid-mirror failed"
command -v file >/dev/null 2>&1 && file build/usbpi-hid-mirror || true

# ---------------------------------------------------------------------------
# 3. locate the FAT boot partition
# ---------------------------------------------------------------------------
mapfile -t offs < <(python3 - "$IMG" <<'PY'
import json, subprocess, sys
img = sys.argv[1]
tbl = json.loads(subprocess.check_output(["sfdisk", "--json", img]))["partitiontable"]
sect = tbl.get("sectorsize", 512)
parts = sorted(tbl["partitions"], key=lambda p: p["start"])
if len(parts) < 2:
    sys.exit("expected at least 2 partitions")
print(parts[0]["start"] * sect)
print(parts[1]["start"] * sect)
PY
)
fat_off="${offs[0]}"
fat_size="$(python3 - "$IMG" <<'PY'
import json, subprocess, sys
tbl = json.loads(subprocess.check_output(["sfdisk", "--json", sys.argv[1]]))["partitiontable"]
sect = tbl.get("sectorsize", 512)
parts = sorted(tbl["partitions"], key=lambda p: p["start"])
print(parts[0]["size"] * sect)
PY
)"
log "boot (FAT) partition at offset $fat_off, size $fat_size"

export MTOOLS_SKIP_CHECK=1
FAT=(-i "${IMG}@@${fat_off}")

# sanity check that we found the right partition
mtype "${FAT[@]}" ::/cmdline.txt >/dev/null 2>&1 \
    || die "no cmdline.txt on the FAT partition at offset $fat_off - wrong partition?"

# ---------------------------------------------------------------------------
# 4. install the /usbpi payload on the boot partition
# ---------------------------------------------------------------------------
log "writing /usbpi payload to the boot partition"
mmd "${FAT[@]}" ::/usbpi 2>/dev/null || true

for f in \
    firstrun.sh \
    usbip-provision.sh \
    usbip-bind-all.sh \
    usbipd.service \
    usbipd-device.service \
    usbpi-hid-mirror.service \
    usbip-autobind.service \
    usbip-autobind.timer \
    usbip-provision.service \
    hid-mirror.conf \
    modprobe-usbpi.conf \
    modules-load.conf \
    hosts.allow \
    hosts.deny
do
    [ -f "$SERVER_DIR/$f" ] || die "missing $SERVER_DIR/$f"
    mcopy "${FAT[@]}" -o "$SERVER_DIR/$f" "::/usbpi/$f"
done

# the mirror daemon is built, not stored in server/
[ -f build/usbpi-hid-mirror ] || die "build/usbpi-hid-mirror missing"
mcopy "${FAT[@]}" -o build/usbpi-hid-mirror "::/usbpi/usbpi-hid-mirror"

# hosts.allow/deny depend on LOCKDOWN
if [ "$LOCKDOWN" != 1 ]; then
    : > build/empty
    mcopy "${FAT[@]}" -o build/empty "::/usbpi/hosts.allow"
    mcopy "${FAT[@]}" -o build/empty "::/usbpi/hosts.deny"
fi

# break-glass account: password hash + this machine's SSH key.
# Reuse the password from a previous build when CREDENTIALS.txt exists, so
# credentials stay stable across rebuilds (keeps them valid for an already
# flashed Pi).
if [ -f CREDENTIALS.txt ] && grep -q 'password: ' CREDENTIALS.txt; then
    pw="$(awk -F': *' '/password: /{print $2}' CREDENTIALS.txt)"
    log "reusing existing break-glass password from CREDENTIALS.txt"
else
    pw="$(openssl rand -base64 24 | tr -dc 'A-Za-z0-9' | cut -c1-16)"
    log "generating break-glass credentials"
fi
hash="$(openssl passwd -6 "$pw")"
printf '%s\n' "$hash" > build/password
mcopy "${FAT[@]}" -o build/password "::/usbpi/password"
umask 077
printf 'Raspberry Pi USB/IP appliance break-glass login\n  host:     usbip-pi.local  (or its DHCP address)\n  user:     usbip\n  password: %s\n' "$pw" > CREDENTIALS.txt

if [ -s "$SSH_KEY" ]; then
    log "authorizing SSH key $SSH_KEY"
    mcopy "${FAT[@]}" -o "$SSH_KEY" "::/usbpi/authorized_keys"
fi

# empty 'ssh' file enables sshd on first boot (classic RPi OS mechanism)
: > build/ssh
mcopy "${FAT[@]}" -o build/ssh "::/ssh"

# ---------------------------------------------------------------------------
# 5. hook firstrun.sh into the kernel command line
# ---------------------------------------------------------------------------
log "installing first-run hook into cmdline.txt"
mtype "${FAT[@]}" ::/cmdline.txt > build/cmdline.txt
if ! grep -q 'usbpi/firstrun.sh' build/cmdline.txt; then
    # collapse to a single line, strip trailing whitespace
    tr '\n' ' ' < build/cmdline.txt | sed -e 's/  */ /g' -e 's/ *$//' > build/cmdline.new
    printf ' systemd.run=%s/usbpi/firstrun.sh systemd.run_success_action=reboot systemd.unit=kernel-command-line.target\n' \
        "$BOOT_MOUNT" >> build/cmdline.new
    mcopy "${FAT[@]}" -o build/cmdline.new "::/cmdline.txt"
else
    log "cmdline.txt already contains the first-run hook"
fi

# ---------------------------------------------------------------------------
# 5b. green ACT LED heartbeat (headless "kernel is running" indicator)
# ---------------------------------------------------------------------------
log "enabling green ACT LED heartbeat in config.txt"
mtype "${FAT[@]}" ::/config.txt > build/config.txt
if grep -q 'act_led_trigger=heartbeat' build/config.txt; then
    log "config.txt already enables the LED heartbeat"
else
    [ -z "$(tail -c1 build/config.txt)" ] || printf '\n' >> build/config.txt
    printf 'dtparam=act_led_trigger=heartbeat\n' >> build/config.txt
    mcopy "${FAT[@]}" -o build/config.txt "::/config.txt"
fi

# ---------------------------------------------------------------------------
# 6. finish
# ---------------------------------------------------------------------------
sync
sha256sum "$IMG" > "$IMG.sha256"
log "built $IMG"
log "sha256: $(cat "$IMG.sha256")"
log "credentials written to CREDENTIALS.txt"
log "flash it with: ./flash.sh <block-device>"
