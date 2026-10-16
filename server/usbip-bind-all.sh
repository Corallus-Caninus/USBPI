#!/bin/bash
# Export every local USB device over USB/IP ("usbip bind"), except:
#   * USB hubs / root hubs
#   * the Raspberry Pi 3B onboard Ethernet (smsc95xx, 0424:ec00) and its hub
#     (LAN9514/LAN9512, 0424:9514 / 0424:9512) - exporting these would kill
#     the network
#   * any mass-storage interface (class 08) - protects the USB flash drive the
#     Pi boots from
#
# The script is idempotent: devices already bound to usbip-host are skipped.
set -u

USBPIP=/usr/sbin/usbip
BOUND_DIR=/sys/bus/usb/drivers/usbip-host

modprobe usbip-host 2>/dev/null || true
[ -x "$USBPIP" ] || { echo "usbip not installed yet"; exit 0; }

bound=0
for dev in /sys/bus/usb/devices/*; do
    busid="$(basename "$dev")"
    # only real device names look like "1-1" / "1-1.3"; interfaces look like "1-1:1.0"
    case "$busid" in
        *-*) ;;
        *) continue ;;
    esac
    [ -r "$dev/idVendor" ] || continue
    vid="$(cat "$dev/idVendor" 2>/dev/null)"
    pid="$(cat "$dev/idProduct" 2>/dev/null)"

    # onboard Ethernet + onboard hub
    case "${vid}:${pid}" in
        0424:ec00|0424:9514|0424:9512) continue ;;
    esac

    # hubs / root hubs
    if [ "$(cat "$dev/bDeviceClass" 2>/dev/null)" = "09" ]; then
        continue
    fi

    # mass storage anywhere in the device -> skip (boot flash drive)
    skip=0
    for ic in "$dev"/*/bInterfaceClass; do
        [ -e "$ic" ] || continue
        if [ "$(cat "$ic" 2>/dev/null)" = "08" ]; then
            skip=1
            break
        fi
    done
    [ "$skip" = 1 ] && continue

    # Already bound to the stub?
    if [ -e "$BOUND_DIR/$busid" ]; then
        st="$(cat "$BOUND_DIR/$busid/usbip_status" 2>/dev/null || echo 1)"
        # usbip_status: 1 = available, 2 = in use, 3 = error.
        # NB: usbip_sockfd is write-only (--w-------) and cannot be read to
        # detect a client, so we test for an active usbipd TCP connection
        # instead - otherwise a device that is in use would be reset.
        if [ "$st" = "1" ]; then
            continue
        fi
        if [ "$st" = "2" ]; then
            command -v ss >/dev/null 2>&1 || continue          # can't tell -> leave it
            if ss -Htn state established '( sport = :3240 )' 2>/dev/null | grep -q .; then
                continue                                        # client connected -> leave it
            fi
        fi
        # stuck (error state, or "used" with no client connected): reset the stub
        echo "usbip: resetting stuck device $busid (status=$st)"
        "$USBPIP" unbind -b "$busid" >/dev/null 2>&1 || true
        sleep 1
        if "$USBPIP" bind -b "$busid" >/dev/null 2>&1; then
            echo "usbip: re-exported $busid ($vid:$pid)"
            bound=$((bound + 1))
        fi
        continue
    fi

    if "$USBPIP" bind -b "$busid" >/dev/null 2>&1; then
        echo "usbip: exported $busid ($vid:$pid)"
        bound=$((bound + 1))
    fi
done

echo "usbip-bind-all: $bound new device(s) exported at $(date -Is)"
