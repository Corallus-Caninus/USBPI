// SPDX-License-Identifier: WTFPL
/*
 * usbpi-hid-mirror - buffer HID devices on the Pi and export them over USB/IP.
 *
 * The stock USB/IP path (usbip-host) only polls a USB device while the remote
 * client has a URB in flight, and a keyboard only remembers its current state,
 * so any key transition during a network hiccup is lost.  This daemon keeps HID
 * devices on the Pi's own usbhid (so they are polled locally, whatever the
 * network does), copies their reports into a bounded in-memory queue and
 * replays them, in order, into a configfs HID gadget bound to usbip-vudc.<N>.
 * usbipd in device mode (port 3241) exports that gadget; the client attaches it
 * like any other USB/IP device and sees the original VID/PID and descriptors.
 *
 * Reports are one write() per report to /dev/hidgN.  While the client is
 * attached those writes complete at the client's pull rate; while it is absent
 * writes return -ESHUTDOWN and the queue simply keeps growing (capped at
 * queue_limit reports, dropping the oldest with a warning).
 */
#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <linux/netlink.h>
#include <time.h>
#include <unistd.h>

#include <linux/hidraw.h>

#include "configuration.c"

#include "usbpi_log.c"
#include "usbpi_config.c"
#include "usbpi_device.c"
#include "usbpi_hid.c"
#include "usbpi_sysfs.c"
#include "usbpi_gadget.c"
#include "usbpi_bind.c"
#include "usbpi_scan.c"
#include "usbpi_kbd.c"
#include "usbpi_main.c"
