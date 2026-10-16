#!/bin/bash
# USBPI one-shot provisioning: install the usbip userspace package and start
# the USB/IP server.  Runs on the second boot (and retries on later boots until
# it succeeds, thanks to the ConditionPathExists marker in the unit).
set -u

LOG=/var/log/usbip-provision.log
exec >>"$LOG" 2>&1
echo "=== usbip provisioning start: $(date -Is) ==="

export DEBIAN_FRONTEND=noninteractive

# --- wait for the network / package lists -----------------------------------
updated=0
for i in $(seq 1 60); do
    if apt-get update -o Acquire::Retries=5; then
        updated=1
        break
    fi
    echo "apt-get update failed (attempt $i), retrying in 10s"
    sleep 10
done
if [ "$updated" != 1 ]; then
    echo "FATAL: could not reach the package repositories"
    exit 1
fi

# --- install usbip ----------------------------------------------------------
installed=0
for i in $(seq 1 10); do
    if apt-get install -y --no-install-recommends usbip; then
        installed=1
        break
    fi
    echo "apt-get install usbip failed (attempt $i), retrying in 10s"
    sleep 10
done
if [ "$installed" != 1 ] || ! command -v usbip >/dev/null 2>&1; then
    echo "FATAL: usbip package could not be installed"
    exit 1
fi
echo "usbip installed: $(dpkg-query -W usbip 2>/dev/null || echo unknown)"

# --- bring up the server ----------------------------------------------------
systemctl daemon-reload
systemctl enable --now usbipd.service
systemctl enable --now usbip-autobind.timer
/usr/local/sbin/usbip-bind-all.sh || true

# --- mark done --------------------------------------------------------------
touch /var/lib/.usbip-provisioned
systemctl disable usbip-provision.service
echo "=== usbip provisioning done: $(date -Is) ==="
echo "exported devices:"
/usr/sbin/usbip list -l 2>/dev/null | grep -E '^ *- busid|usbip-host' || true
