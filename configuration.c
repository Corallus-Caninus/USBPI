/*
 * USBPI compile-time configuration.
 *
 * This file contains nothing but #define directives.  It is included by the
 * server daemon, the client binary and the shared wire helpers, so every
 * tunable interval, buffer size, path and protocol constant lives here.
 *
 * The image ships with defaults chosen for a Raspberry Pi 3B on a wired LAN.
 * Nothing here normally needs changing; rebuild with ./build.sh after edits.
 */

/* ------------------------------------------------------------------ */
/* sysfs / configfs paths                                              */
/* ------------------------------------------------------------------ */
#define USB_DEVICES             "/sys/bus/usb/devices"
#define USBIP_HOST              "/sys/bus/usb/drivers/usbip-host"
#define USBIP_HOST_BIND         USBIP_HOST "/bind"
#define USBIP_HOST_UNBIND       USBIP_HOST "/unbind"
#define DRIVERS_PROBE           "/sys/bus/usb/drivers_probe"
#define GADGET_ROOT             "/sys/kernel/config/usb_gadget"
#define CONFIGFS_DIR            "/sys/kernel/config"
#define UDC_DIR                 "/sys/class/udc"
#define CLAIM_DIR               "/run/usbpi-hid-mirror"
#define CLAIM_FILE              CLAIM_DIR "/claimed"
#define DEFAULT_CONF            "/etc/usbpi/hid-mirror.conf"
#define KBD_CONF                "/etc/usbpi/kbd.conf"
#define PROC_TCP                "/proc/net/tcp"
#define CPU_GOVERNOR_DIR        "/sys/devices/system/cpu"
#define USB_AUTOSUSPEND         "/sys/module/usbcore/parameters/autosuspend"

/* ------------------------------------------------------------------ */
/* protocol ports / addresses                                          */
/* ------------------------------------------------------------------ */
#define USBIP_PORT              3240
#define GADGET_PORT             3241
#define KBD_PORT                3250
#define KBD_BIND_ADDR           "0.0.0.0"

/* ------------------------------------------------------------------ */
/* server limits                                                       */
/* ------------------------------------------------------------------ */
#define MAX_IFACES              4
#define HIDG_MINORS             4
#define MAX_REPORT_LEN          512
#define MAX_EXCLUDE             32
#define MAX_STRING              126
#define REPORT_DESC_MAX         4096
#define PBUF                    (PATH_MAX + 128)
#define CLAIM_LINE_MAX          128
#define SYSFS_VALUE_MAX         256
#define CONFIG_LINE_MAX         512
#define NAME_MAX_LEN            64
#define SYSFS_SMALL_BUF         64
#define MAX_REPORT_IDS          256
#define MAIN_BATCH              256
#define BAD_REASON_MAX          64

/* ------------------------------------------------------------------ */
/* server timing (milliseconds unless noted)                           */
/* ------------------------------------------------------------------ */
#define RESCAN_MIN_MS           200
#define DEFAULT_RESCAN_MS       2000
#define SLOW_RESCAN_MS          10000
#define GADGET_RETRY_MS         100
#define HIDG_OPEN_TIMEOUT_MS    2000
#define UDEV_UEVENT_GROUP       1
#define RETRY_FAILED_SEC        60
#define STUCK_RETRY_SEC         3600
#define PROBE_INTERVAL_SEC      5
#define WAIT_TIMEOUT_SEC        20
#define CLAIM_RECHECK_MS        1000
#define BIND_INTERVAL_MS        5000
#define UEVENT_BUF_MAX          4096

/* ------------------------------------------------------------------ */
/* report queue (legacy HID mirror)                                    */
/* ------------------------------------------------------------------ */
#define DEFAULT_QUEUE           4096
#define QUEUE_DROP_LOG_SEC      5

/* ------------------------------------------------------------------ */
/* keyboard fast path: ring / replay                                   */
/* ------------------------------------------------------------------ */
#define KBD_MAGIC               0x55534250u
#define KBD_VERSION             1
#define KBD_RING_EVENTS         4096
#define KBD_MAX_EVENTS_PKT      16
#define KBD_MAX_PKT             512
#define KBD_KEY_BYTES           ((KEY_MAX + 8) / 8)
#define KBD_MAX_CLIENT_BUF      (KBD_MAX_PKT * 4)

/* ------------------------------------------------------------------ */
/* keyboard fast path: timing (milliseconds)                           */
/* ------------------------------------------------------------------ */
#define KBD_SNAPSHOT_MS         50
#define KBD_IDLE_SNAPSHOT_MS    200
#define KBD_ACTIVE_MS           500
#define KBD_DEADMAN_MISSES      3
#define KBD_DEADMAN_MIN_MS      150
#define KBD_DEADMAN_MAX_MS      500
#define KBD_DEADMAN_MARGIN_MS   20
#define KBD_RECONNECT_MIN_MS    100
#define KBD_RECONNECT_MAX_MS    2000
#define KBD_ACK_MS              50
#define KBD_EVENT_FLOOD         64

/* ------------------------------------------------------------------ */
/* keyboard fast path: socket tuning                                   */
/* ------------------------------------------------------------------ */
#define KBD_SNDBUF              (32 * 1024)
#define KBD_RCVBUF              (32 * 1024)
#define KBD_TCP_USER_TIMEOUT_MS 2000
#define KBD_KEEPALIVE_IDLE      1
#define KBD_KEEPALIVE_INTVL     1
#define KBD_KEEPALIVE_CNT       3

/* ------------------------------------------------------------------ */
/* client defaults                                                     */
/* ------------------------------------------------------------------ */
#define DEFAULT_SERVER          "usbip-pi.lan"
#define DEFAULT_CANDIDATES      "usbip-pi.local"
#define DEFAULT_INTERVAL        5
#define DOWN_THRESHOLD          2
#define PORT_PROBE_TIMEOUT_MS   2000
#define LAST_IP_FILE            "/var/lib/usbip-sync.last-ip"
#define UINPUT_DEV              "/dev/uinput"
#define UINPUT_NAME             "USBPI remote keyboard"
#define UINPUT_VENDOR           0x1d6b
#define UINPUT_PRODUCT          0x0104
#define UINPUT_VERSION          0x0100
#define CLIENT_CMD_MAX          4096
#define CLIENT_READ_MAX         8192

/* ------------------------------------------------------------------ */
/* automatic scheduling / latency tuning                               */
/* ------------------------------------------------------------------ */
#define SCHED_PRIO_SERVER       50
#define SCHED_PRIO_CLIENT       40
#define TUNE_GOVERNOR           "performance"
#define TUNE_IRQ_CORE           0
#define TUNE_DAEMON_CORE_SLOT   3

/* ------------------------------------------------------------------ */
/* logging / diagnostics                                               */
/* ------------------------------------------------------------------ */
#define LATENCY_LOG_SEC         60
#define LOG_LINE_MAX            1024
