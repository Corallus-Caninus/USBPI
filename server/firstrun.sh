#!/bin/bash
# USBPI first-run script.
#
# Executed once on the very first boot via the kernel parameter
#   systemd.run=/boot/firmware/usbpi/firstrun.sh
# (the same mechanism Raspberry Pi Imager uses).
#
# It runs as root, installs the USB/IP server configuration into the root
# filesystem, enables the one-shot provisioning service and then lets
# systemd reboot (systemd.run_success_action=reboot).  On the second boot the
# provisioning service installs the `usbip` package and starts the server.
set -u

# ---------------------------------------------------------------------------
# locate the boot (FAT) partition - Bookworm/Trixie mount it on /boot/firmware
# ---------------------------------------------------------------------------
if [ -d /boot/firmware ]; then
    BOOT=/boot/firmware
else
    BOOT=/boot
fi
PAYLOAD="$BOOT/usbpi"
LOG="$PAYLOAD/firstrun.log"
mkdir -p "$PAYLOAD"
exec >"$LOG" 2>&1

echo "=== usbpi firstrun start: $(date -Is) (boot=$BOOT) ==="

die() { echo "FATAL: $*"; exit 1; }
[ -d "$PAYLOAD" ] || die "payload directory $PAYLOAD missing"

USERNAME=usbip

# ---------------------------------------------------------------------------
# 1. dedicated admin account (break-glass SSH only; never needed in normal use)
# ---------------------------------------------------------------------------
if ! id "$USERNAME" >/dev/null 2>&1; then
    echo "creating user $USERNAME"
    useradd -m -s /bin/bash \
        -G sudo,adm,dialout,cdrom,audio,video,plugdev,games,users,input,netdev,gpio,i2c,spi \
        "$USERNAME" 2>/dev/null \
    || useradd -m -s /bin/bash "$USERNAME" 2>/dev/null \
    || echo "WARN: useradd failed"
fi

if [ -f "$PAYLOAD/password" ] && id "$USERNAME" >/dev/null 2>&1; then
    echo "$USERNAME:$(cat "$PAYLOAD/password")" | chpasswd -e 2>/dev/null \
        || echo "WARN: chpasswd failed"
fi

HOME_DIR="$(getent passwd "$USERNAME" | cut -d: -f6)"
if [ -n "${HOME_DIR:-}" ] && [ -s "$PAYLOAD/authorized_keys" ]; then
    install -d -m 700 -o "$USERNAME" -g "$USERNAME" "$HOME_DIR/.ssh"
    install -m 600 -o "$USERNAME" -g "$USERNAME" \
        "$PAYLOAD/authorized_keys" "$HOME_DIR/.ssh/authorized_keys"
    echo "installed authorized_keys"
fi

# make sure sshd is there for emergencies; no login is required for operation
systemctl enable ssh 2>/dev/null || true

# ---------------------------------------------------------------------------
# 2. install the USB/IP server configuration into the root filesystem
# ---------------------------------------------------------------------------
install -d /etc/modules-load.d /usr/local/sbin

install -m 644 "$PAYLOAD/modules-load.conf"    /etc/modules-load.d/usbip.conf
install -m 644 "$PAYLOAD/usbipd.service"       /etc/systemd/system/usbipd.service
install -m 644 "$PAYLOAD/usbip-autobind.service" /etc/systemd/system/usbip-autobind.service
install -m 644 "$PAYLOAD/usbip-autobind.timer" /etc/systemd/system/usbip-autobind.timer
install -m 644 "$PAYLOAD/usbip-provision.service" /etc/systemd/system/usbip-provision.service
install -m 755 "$PAYLOAD/usbip-provision.sh"   /usr/local/sbin/usbip-provision.sh
install -m 755 "$PAYLOAD/usbip-bind-all.sh"    /usr/local/sbin/usbip-bind-all.sh

# usbipd is built with TCP wrappers on Debian, so these are enforced.
if ! grep -q '^usbipd:' /etc/hosts.allow 2>/dev/null; then
    cat "$PAYLOAD/hosts.allow" >> /etc/hosts.allow
fi
if ! grep -q '^usbipd:' /etc/hosts.deny 2>/dev/null; then
    cat "$PAYLOAD/hosts.deny" >> /etc/hosts.deny
fi

# ---------------------------------------------------------------------------
# 3. hostname -> usbip-pi.local (avahi mDNS)
# ---------------------------------------------------------------------------
echo "usbip-pi" > /etc/hostname
if grep -q '^127\.0\.1\.1' /etc/hosts; then
    sed -i 's/^127\.0\.1\.1.*/127.0.1.1\tusbip-pi/' /etc/hosts
else
    printf '127.0.1.1\tusbip-pi\n' >> /etc/hosts
fi
hostname usbip-pi 2>/dev/null || true

# ---------------------------------------------------------------------------
# 4. disable the Raspberry Pi first-boot dialogs so the appliance is truly
#    headless.  userconfig.service only recognises users created via
#    userconf.txt on the boot partition; since we create the user directly it
#    would otherwise pop up a keyboard/user dialog on the console on every boot
#    (and block multi-user.target while waiting for input).
# ---------------------------------------------------------------------------
systemctl disable userconfig.service 2>/dev/null || true
systemctl mask userconfig.service 2>/dev/null || true

if [ -d /etc/cloud ]; then
    touch /etc/cloud/cloud-init.disabled
    echo "cloud-init disabled"
fi

# ---------------------------------------------------------------------------
# 5. enable the one-shot provisioning service for the next boot
# ---------------------------------------------------------------------------
systemctl enable usbip-provision.service

# ---------------------------------------------------------------------------
# 6. remove the first-run kernel parameters so this runs exactly once
# ---------------------------------------------------------------------------
CMDLINE="$BOOT/cmdline.txt"
if [ -f "$CMDLINE" ]; then
    cp -a "$CMDLINE" "$PAYLOAD/cmdline.txt.orig"
    sed -i -E \
        -e 's/ ?systemd\.run=[^ ]*//g' \
        -e 's/ ?systemd\.run_success_action=[^ ]*//g' \
        -e 's/ ?systemd\.run_failure_action=[^ ]*//g' \
        -e 's/ ?systemd\.unit=kernel-command-line\.target//g' \
        "$CMDLINE"
    # tidy up doubled spaces and make sure the file ends with a newline
    sed -i -e 's/  */ /g' -e 's/^ //' "$CMDLINE"
    printf '\n' >> "$CMDLINE"
fi

sync
echo "=== usbpi firstrun done: $(date -Is) - rebooting ==="
# systemd.run_success_action=reboot performs the reboot.
exit 0
