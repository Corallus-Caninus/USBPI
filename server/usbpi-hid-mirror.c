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
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <linux/hidraw.h>

#define USB_DEVICES   "/sys/bus/usb/devices"
#define USBIP_HOST    "/sys/bus/usb/drivers/usbip-host"
#define GADGET_ROOT   "/sys/kernel/config/usb_gadget"
#define CONFIGFS_DIR  "/sys/kernel/config"
#define UDC_DIR       "/sys/class/udc"
#define CLAIM_DIR     "/run/usbpi-hid-mirror"
#define CLAIM_FILE    CLAIM_DIR "/claimed"
#define DEFAULT_CONF  "/etc/usbpi/hid-mirror.conf"

#define MAX_IFACES        4     /* hidg minors are a global kernel resource */
#define HIDG_MINORS       4
#define MAX_REPORT_LEN    512   /* larger devices fall back to usbip-host */
#define MAX_EXCLUDE       32
#define MAX_STRING        126
#define REPORT_DESC_MAX   4096
#define RESCAN_MIN_MS     200
#define DEFAULT_QUEUE     65536
#define RETRY_FAILED_SEC  60
#define PBUF              (PATH_MAX + 128)

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */

static bool opt_verbose;
static bool opt_dry_run;

static void logmsg(const char *level, const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "usbpi-hid-mirror: %s: ", level);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

#define log_info(...)  logmsg("info", __VA_ARGS__)
#define log_warn(...)  logmsg("warn", __VA_ARGS__)
#define log_err(...)   logmsg("err", __VA_ARGS__)
#define log_dbg(...)   do { if (opt_verbose) logmsg("dbg", __VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ */
/* small filesystem helpers                                            */
/* ------------------------------------------------------------------ */

static int read_text(const char *path, char *buf, size_t sz)
{
    int fd;
    ssize_t n;

    if (sz == 0)
        return -1;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    n = read(fd, buf, sz - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    return (int)n;
}

static int read_hex_attr(const char *path, unsigned long *out)
{
    char buf[64];
    char *end;
    unsigned long v;

    if (read_text(path, buf, sizeof(buf)) < 0)
        return -1;
    errno = 0;
    v = strtoul(buf, &end, 16);
    if (errno != 0 || end == buf)
        return -1;
    *out = v;
    return 0;
}

static int write_text(const char *path, const char *text)
{
    int fd;
    size_t len = strlen(text);
    ssize_t n;

    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    n = write(fd, text, len);
    close(fd);
    if (n < 0 || (size_t)n != len)
        return -1;
    return 0;
}

static int write_bin(const char *path, const void *data, size_t len)
{
    int fd;
    ssize_t n;

    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    n = write(fd, data, len);
    close(fd);
    if (n < 0 || (size_t)n != len)
        return -1;
    return 0;
}

static bool exists(const char *path)
{
    return access(path, F_OK) == 0;
}

static bool is_dir(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int remove_dir(const char *path)
{
    if (rmdir(path) < 0 && errno != ENOENT)
        return -1;
    return 0;
}

static int remove_file(const char *path)
{
    if (unlink(path) < 0 && errno != ENOENT)
        return -1;
    return 0;
}

/* formatted path in a rotating static buffer (never nest two calls) */
static char *gen_path(const char *fmt, ...)
{
    static char bufs[4][PATH_MAX];
    static unsigned idx;
    char *buf = bufs[idx++ % 4];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, PATH_MAX, fmt, ap);
    va_end(ap);
    return buf;
}

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */

struct config {
    bool enabled;
    unsigned queue_limit;
    int rescan_ms;
    char exclude[MAX_EXCLUDE][16];   /* "vvvv:pppp", lower case */
    int nexc;
};

static struct config g_cfg;

static void parse_exclude(struct config *cfg, char *val)
{
    char *tok;
    char *save = NULL;

    for (tok = strtok_r(val, " ,\t\r\n", &save); tok;
         tok = strtok_r(NULL, " ,\t\r\n", &save)) {
        char entry[16];
        size_t i, len = strlen(tok);

        if (cfg->nexc >= MAX_EXCLUDE) {
            log_warn("exclude list full, ignoring '%s'", tok);
            break;
        }
        if (len != 9 || tok[4] != ':') {
            log_warn("ignoring malformed exclude entry '%s'", tok);
            continue;
        }
        for (i = 0; i < len && i < sizeof(entry) - 1; i++)
            entry[i] = (char)tolower((unsigned char)tok[i]);
        entry[i] = '\0';
        memcpy(cfg->exclude[cfg->nexc], entry, i + 1);
        cfg->nexc++;
    }
}

static void load_config(const char *path, struct config *cfg)
{
    FILE *f;
    char line[512];

    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = true;
    cfg->queue_limit = DEFAULT_QUEUE;
    cfg->rescan_ms = 2000;

    f = fopen(path, "r");
    if (!f) {
        if (errno != ENOENT)
            log_warn("cannot read %s: %s (using defaults)", path,
                     strerror(errno));
        return;
    }

    while (fgets(line, sizeof(line), f)) {
        char *eq, *key, *val, *p;

        p = strchr(line, '#');
        if (p)
            *p = '\0';
        key = line;
        while (*key == ' ' || *key == '\t')
            key++;
        eq = strchr(key, '=');
        if (!eq)
            continue;
        *eq = '\0';
        val = eq + 1;
        while (*val == ' ' || *val == '\t')
            val++;
        for (p = key; *p; p++)
            *p = (char)tolower((unsigned char)*p);
        p = key + strlen(key);
        while (p > key && (p[-1] == ' ' || p[-1] == '\t'))
            *--p = '\0';

        if (!strcmp(key, "enabled")) {
            cfg->enabled = atoi(val) != 0;
        } else if (!strcmp(key, "queue_limit")) {
            long v = atol(val);

            if (v >= 16 && v <= 1000000)
                cfg->queue_limit = (unsigned)v;
            else
                log_warn("ignoring queue_limit=%s", val);
        } else if (!strcmp(key, "rescan_ms")) {
            long v = atol(val);

            if (v >= RESCAN_MIN_MS && v <= 60000)
                cfg->rescan_ms = (int)v;
            else
                log_warn("ignoring rescan_ms=%s", val);
        } else if (!strcmp(key, "exclude")) {
            parse_exclude(cfg, val);
        }
    }
    fclose(f);
}

static bool is_excluded(const struct config *cfg, const char *vid,
                        const char *pid)
{
    char vidpid[16];
    int i;

    snprintf(vidpid, sizeof(vidpid), "%.4s:%.4s", vid, pid);
    for (i = 0; i < cfg->nexc; i++)
        if (!strcasecmp(vidpid, cfg->exclude[i]))
            return true;
    return false;
}

/* ------------------------------------------------------------------ */
/* device model                                                        */
/* ------------------------------------------------------------------ */

struct iface {
    char ifname[64];             /* e.g. "1-1.3:1.0" */
    int ifnum;
    int subclass;
    int protocol;
    bool is_keyboard;
    char hidraw[64];
    char hidg[72];
    unsigned char report_desc[REPORT_DESC_MAX];
    size_t report_desc_len;
    unsigned int report_len;     /* bytes per report, including report ID */
    int raw_fd;
    int g_fd;
    bool bad;                    /* too-long report seen; device must go */
    /* bounded report queue (absolute-state reports only) */
    unsigned char *q;
    unsigned short *qlen;
    unsigned int qlimit;
    unsigned int qhead;
    unsigned int qcount;
    unsigned long long drops;
    time_t last_drop_log;
    unsigned char last[MAX_REPORT_LEN];
    unsigned int last_len;
    bool have_last;
    bool attached_hint;          /* client has pulled at least one report */
    bool detached_logged;
};

struct device {
    struct device *next;
    bool seen;
    bool claimed;
    bool active;
    bool bad;                    /* teardown and stop mirroring this device */
    bool failed;
    time_t retry_after;
    bool wait_logged;
    char busid[64];
    char dir[PATH_MAX];
    char vid[8];
    char pid[8];
    char bcd[16];
    char manufacturer[MAX_STRING + 1];
    char product[MAX_STRING + 1];
    char serial[MAX_STRING + 1];
    int nifaces;
    struct iface ifaces[MAX_IFACES];
    char gadget[PATH_MAX];
    char c1[PATH_MAX];
    char udc[64];
};

static struct device *devices;
static int active_ifaces;
static int active_gadgets;
static bool claim_dirty;

static struct device *device_find(const char *busid)
{
    struct device *d;

    for (d = devices; d; d = d->next)
        if (!strcmp(d->busid, busid))
            return d;
    return NULL;
}

static void device_free(struct device *d)
{
    struct iface *f;
    int i;

    for (i = 0; i < d->nifaces; i++) {
        f = &d->ifaces[i];
        if (f->raw_fd >= 0)
            close(f->raw_fd);
        if (f->g_fd >= 0)
            close(f->g_fd);
        free(f->q);
        free(f->qlen);
    }
    free(d);
}

/* a fatal per-interface error: stop mirroring the whole device */
static void mark_iface_bad(struct iface *f, const char *why)
{
    struct device *d;
    int j;

    log_err("%s: %s, re-exporting this device on usbip-host", f->ifname, why);
    f->bad = true;
    for (d = devices; d; d = d->next)
        for (j = 0; j < d->nifaces; j++)
            if (&d->ifaces[j] == f)
                d->bad = true;
}

/* ------------------------------------------------------------------ */
/* claimed busid file (consulted by usbip-bind-all.sh)                 */
/* ------------------------------------------------------------------ */

static void write_claim_file(void)
{
    char tmp[PBUF];
    struct device *d;
    FILE *f;

    if (opt_dry_run)
        return;
    snprintf(tmp, sizeof(tmp), "%s/.claimed.tmp", CLAIM_DIR);
    f = fopen(tmp, "w");
    if (!f) {
        log_warn("cannot write %s: %s", tmp, strerror(errno));
        return;
    }
    for (d = devices; d; d = d->next)
        if (d->claimed)
            fprintf(f, "%s\n", d->busid);
    fclose(f);
    if (rename(tmp, CLAIM_FILE) < 0)
        log_warn("cannot rename claim file: %s", strerror(errno));
    claim_dirty = false;
}

static void claim_device(struct device *d, const char *why)
{
    if (d->claimed)
        return;
    d->claimed = true;
    claim_dirty = true;
    write_claim_file();
    log_info("claiming %s (%s:%s): %s", d->busid, d->vid, d->pid, why);
}

static void unclaim_device(struct device *d)
{
    if (!d->claimed)
        return;
    d->claimed = false;
    claim_dirty = true;
    write_claim_file();
}

/* ------------------------------------------------------------------ */
/* HID report descriptor parsing - maximum input report size           */
/* ------------------------------------------------------------------ */

struct parser_state {
    unsigned report_size;
    unsigned report_count;
    unsigned report_id;
};

static unsigned max_input_report(const unsigned char *d, size_t len)
{
    unsigned bits[256] = { 0 };
    struct parser_state st = { 0 }, stack[8];
    int sp = 0;
    size_t pos = 0;
    bool has_ids = false;
    unsigned i, max_bytes = 0;

    while (pos < len) {
        unsigned char b = d[pos++];
        unsigned type, tag, size;
        unsigned long data = 0;

        if (b == 0xFE) {                 /* long item */
            if (pos + 2 > len)
                break;
            size = d[pos + 1];
            pos += 3 + size;
            continue;
        }
        type = (b >> 2) & 0x3;
        tag = (b >> 4) & 0xF;
        size = b & 0x3;
        if (size == 3)
            size = 4;
        if (pos + size > len)
            break;
        for (i = 0; i < size; i++)
            data |= (unsigned long)d[pos + i] << (8 * i);
        pos += size;

        switch (type) {
        case 0:                          /* main */
            if (tag == 0x8) {            /* Input */
                unsigned long total = (unsigned long)st.report_size *
                                      st.report_count;

                if (st.report_id < 256 && total < (1U << 16))
                    bits[st.report_id] += (unsigned)total;
            }
            break;
        case 1:                          /* global */
            switch (tag) {
            case 0x7:
                st.report_size = (unsigned)data;
                break;
            case 0x8:
                st.report_id = (unsigned)data & 0xFF;
                has_ids = true;
                break;
            case 0x9:
                st.report_count = (unsigned)data;
                break;
            case 0xA:                    /* Push */
                if (sp >= 0 && sp < (int)(sizeof(stack) / sizeof(stack[0])))
                    stack[sp] = st;
                sp++;
                break;
            case 0xB:                    /* Pop */
                if (sp > 0 && sp <= (int)(sizeof(stack) / sizeof(stack[0])))
                    st = stack[sp - 1];
                if (sp > 0)
                    sp--;
                break;
            default:
                break;
            }
            break;
        default:                         /* local / reserved */
            break;
        }
    }

    for (i = 0; i < 256; i++) {
        unsigned bytes;

        if (bits[i] == 0)
            continue;
        bytes = (bits[i] + 7) / 8;
        if (has_ids)
            bytes += 1;
        if (bytes > max_bytes)
            max_bytes = bytes;
    }
    return max_bytes;
}

/* ------------------------------------------------------------------ */
/* report queues                                                       */
/* ------------------------------------------------------------------ */

static void queue_drop_oldest(struct iface *f)
{
    f->qhead = (f->qhead + 1) % f->qlimit;
    f->qcount--;
    f->drops++;
    if (time(NULL) - f->last_drop_log >= 5) {
        log_warn("%s: report queue full (%u), dropped %llu old report(s)",
                 f->ifname, f->qlimit, f->drops);
        f->last_drop_log = time(NULL);
    }
}

static void queue_push(struct iface *f, const unsigned char *buf,
                       unsigned int len)
{
    unsigned int tail;

    /* Keyboard reports are absolute state: two identical consecutive
     * reports carry no new information, so collapse them (this also keeps
     * idle chatter from filling the queue while the client is away).
     * Never do this for relative devices (mice) - identical reports there
     * mean real motion. */
    if (f->is_keyboard && f->have_last && len == f->last_len &&
        !memcmp(buf, f->last, len))
        return;

    if (len > f->report_len)
        len = f->report_len;

    if (f->qcount == f->qlimit)
        queue_drop_oldest(f);
    tail = (f->qhead + f->qcount) % f->qlimit;
    memcpy(f->q + (size_t)tail * f->report_len, buf, len);
    f->qlen[tail] = (unsigned short)len;
    f->qcount++;

    memcpy(f->last, buf, len);
    f->last_len = len;
    f->have_last = true;
}

/* Returns 0 when a report was consumed, -1 otherwise. */
static int queue_pop_write(struct iface *f)
{
    unsigned char *rec = f->q + (size_t)f->qhead * f->report_len;
    unsigned int len = f->qlen[f->qhead];
    ssize_t n;

    n = write(f->g_fd, rec, len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return -1;
        if (errno == ESHUTDOWN) {
            /* no client attached: keep buffering */
            if (!f->detached_logged) {
                log_info("%s: client detached (%s), buffering locally",
                         f->ifname, strerror(errno));
                f->detached_logged = true;
            }
            f->attached_hint = false;
            return -1;
        }
        mark_iface_bad(f, strerror(errno));
        return -1;
    }
    if (f->detached_logged) {
        log_info("%s: client attached, draining buffered reports", f->ifname);
        f->detached_logged = false;
    }
    f->attached_hint = true;
    f->qhead = (f->qhead + 1) % f->qlimit;
    f->qcount--;
    return 0;
}

/* ------------------------------------------------------------------ */
/* sysfs discovery                                                     */
/* ------------------------------------------------------------------ */

static bool onboard_vidpid(const char *vid, const char *pid)
{
    static const char *skip[] = { "0424:ec00", "0424:9514", "0424:9512" };
    char vidpid[16];
    size_t i;

    snprintf(vidpid, sizeof(vidpid), "%.4s:%.4s", vid, pid);
    for (i = 0; i < sizeof(skip) / sizeof(skip[0]); i++)
        if (!strcasecmp(vidpid, skip[i]))
            return true;
    return false;
}

static bool device_is_virtual(const char *dir)
{
    char real[PATH_MAX];

    if (!realpath(dir, real))
        return false;
    return strstr(real, "vhci_hcd") != NULL ||
           strstr(real, "usbip-vudc") != NULL ||
           strstr(real, "dummy_hcd") != NULL;
}

/* find the hidraw node that belongs to an interface directory */
static int find_hidraw(const char *ifdir, char *out, size_t outsz)
{
    DIR *d, *h;
    struct dirent *e, *he;

    d = opendir(ifdir);
    if (!d)
        return -1;
    while ((e = readdir(d))) {
        char hpath[PBUF];

        if (e->d_name[0] == '.')
            continue;
        snprintf(hpath, sizeof(hpath), "%s/%s/hidraw", ifdir, e->d_name);
        if (!is_dir(hpath))
            continue;
        h = opendir(hpath);
        if (!h)
            continue;
        while ((he = readdir(h))) {
            if (strncmp(he->d_name, "hidraw", 6))
                continue;
            snprintf(out, outsz, "/dev/%s", he->d_name);
            closedir(h);
            closedir(d);
            return 0;
        }
        closedir(h);
    }
    closedir(d);
    return -1;
}

/* fill one interface from its sysfs directory */
static int fill_iface(struct iface *f, const char *ifname)
{
    char path[PBUF];
    unsigned long v;

    memset(f, 0, sizeof(*f));
    f->raw_fd = -1;
    f->g_fd = -1;
    strncpy(f->ifname, ifname, sizeof(f->ifname) - 1);

    snprintf(path, sizeof(path), USB_DEVICES "/%s/bInterfaceNumber", ifname);
    if (read_hex_attr(path, &v) < 0)
        return -1;
    f->ifnum = (int)v;

    snprintf(path, sizeof(path), USB_DEVICES "/%s/bInterfaceSubClass", ifname);
    if (read_hex_attr(path, &v) < 0)
        return -1;
    f->subclass = (int)v;

    snprintf(path, sizeof(path), USB_DEVICES "/%s/bInterfaceProtocol", ifname);
    if (read_hex_attr(path, &v) < 0)
        return -1;
    f->protocol = (int)v;

    f->is_keyboard = (f->subclass == 1 &&
                      (f->protocol == 1 || f->protocol == 2));
    return 0;
}

struct scan_dev {
    char vid[8], pid[8], bcd[16];
    char manufacturer[MAX_STRING + 1];
    char product[MAX_STRING + 1];
    char serial[MAX_STRING + 1];
    int nifaces;
    char ifnames[MAX_IFACES][64];
};

static bool iface_dirname(const char *busid, const char *entry)
{
    size_t n = strlen(busid);

    return !strncmp(entry, busid, n) && entry[n] == ':';
}

/* returns 0 when the device has mirrorable HID interfaces */
static int inspect_device(const char *busid, const char *dir,
                          struct scan_dev *sd)
{
    char path[PBUF];
    unsigned long cls;
    DIR *d;
    struct dirent *e;
    int n = 0;

    memset(sd, 0, sizeof(*sd));
    snprintf(path, sizeof(path), "%s/idVendor", dir);
    if (read_text(path, sd->vid, sizeof(sd->vid)) < 0)
        return -1;
    snprintf(path, sizeof(path), "%s/idProduct", dir);
    if (read_text(path, sd->pid, sizeof(sd->pid)) < 0)
        return -1;
    snprintf(path, sizeof(path), "%s/bcdDevice", dir);
    if (read_text(path, sd->bcd, sizeof(sd->bcd)) < 0)
        snprintf(sd->bcd, sizeof(sd->bcd), "0100");
    snprintf(path, sizeof(path), "%s/manufacturer", dir);
    read_text(path, sd->manufacturer, sizeof(sd->manufacturer));
    snprintf(path, sizeof(path), "%s/product", dir);
    read_text(path, sd->product, sizeof(sd->product));
    snprintf(path, sizeof(path), "%s/serial", dir);
    read_text(path, sd->serial, sizeof(sd->serial));

    if (onboard_vidpid(sd->vid, sd->pid))
        return -1;
    if (device_is_virtual(dir))
        return -1;

    snprintf(path, sizeof(path), "%s/bDeviceClass", dir);
    if (read_hex_attr(path, &cls) < 0)
        return -1;
    if (cls == 9)                        /* hub */
        return -1;

    d = opendir(dir);
    if (!d)
        return -1;
    while ((e = readdir(d))) {
        unsigned long icls;

        if (!iface_dirname(busid, e->d_name))
            continue;
        snprintf(path, sizeof(path), "%s/%s/bInterfaceClass", dir,
                 e->d_name);
        if (read_hex_attr(path, &icls) < 0)
            continue;
        if (icls == 8) {                 /* mass storage: protect boot drive */
            closedir(d);
            return -1;
        }
        if (icls != 3)
            continue;
        if (n >= MAX_IFACES) {
            log_warn("%s: more than %d HID interfaces, leaving on usbip-host",
                     busid, MAX_IFACES);
            closedir(d);
            return -1;
        }
        snprintf(sd->ifnames[n], sizeof(sd->ifnames[0]), "%s", e->d_name);
        n++;
    }
    closedir(d);
    sd->nifaces = n;
    return n > 0 ? 0 : -1;
}

/* find a free usbip-vudc instance not used by an active gadget */
static int pick_udc(char *out, size_t outsz)
{
    DIR *d;
    struct dirent *e;

    d = opendir(UDC_DIR);
    if (!d)
        return -1;
    while ((e = readdir(d))) {
        struct device *dev;
        bool used = false;

        if (strncmp(e->d_name, "usbip-vudc", 10))
            continue;
        for (dev = devices; dev; dev = dev->next)
            if (dev->active && !strcmp(dev->udc, e->d_name))
                used = true;
        if (!used) {
            snprintf(out, outsz, "%s", e->d_name);
            closedir(d);
            return 0;
        }
    }
    closedir(d);
    return -1;
}

static int vudc_count(void)
{
    DIR *d;
    struct dirent *e;
    int n = 0;

    d = opendir(UDC_DIR);
    if (!d)
        return 0;
    while ((e = readdir(d)))
        if (!strncmp(e->d_name, "usbip-vudc", 10))
            n++;
    closedir(d);
    return n;
}

/* ------------------------------------------------------------------ */
/* gadget setup / teardown                                             */
/* ------------------------------------------------------------------ */

static void hidg_path_for_minor(const char *function_dir, char *out,
                                size_t outsz)
{
    char buf[64], *colon;

    snprintf(out, outsz, "%s", "/dev/hidg?");
    snprintf(buf, sizeof(buf), "%s/dev", function_dir);
    if (read_text(buf, buf, sizeof(buf)) < 0)
        return;
    colon = strchr(buf, ':');
    if (!colon)
        return;
    snprintf(out, outsz, "/dev/hidg%lu", strtoul(colon + 1, NULL, 10));
}

static int open_hidg_wait(const char *path, int timeout_ms)
{
    int tries = timeout_ms / 50;
    int i;

    if (tries < 1)
        tries = 1;
    for (i = 0; i < tries; i++) {
        int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);

        if (fd >= 0)
            return fd;
        usleep(50 * 1000);
    }
    return -1;
}

/* remove a gadget tree we created (configfs order matters) */
static void gadget_remove_tree(struct device *d, int nifaces)
{
    char path[PBUF];
    struct iface *f;
    int i;

    if (!d->gadget[0] || !is_dir(d->gadget))
        return;

    snprintf(path, sizeof(path), "%s/UDC", d->gadget);
    if (exists(path))
        write_text(path, "");
    for (i = 0; i < nifaces; i++) {
        f = &d->ifaces[i];
        if (!f->ifname[0])
            continue;                     /* never created */
        snprintf(path, sizeof(path), "%s/hid.usb%d", d->c1, f->ifnum);
        remove_file(path);
        snprintf(path, sizeof(path), "%s/functions/hid.usb%d", d->gadget,
                 f->ifnum);
        remove_dir(path);
    }
    remove_dir(gen_path("%s/configs/c.1", d->gadget));
    remove_dir(gen_path("%s/strings/0x409", d->gadget));
    remove_dir(gen_path("%s/strings", d->gadget));
    remove_dir(gen_path("%s/functions", d->gadget));
    remove_dir(d->gadget);
    d->gadget[0] = '\0';
}

static int setup_gadget(struct device *d)
{
    char path[PBUF], buf[PATH_MAX];
    struct iface *f;
    int i, created = 0;

    snprintf(d->gadget, sizeof(d->gadget), GADGET_ROOT "/usbpi-%s", d->busid);
    snprintf(d->c1, sizeof(d->c1), "%s/configs/c.1", d->gadget);

    if (is_dir(d->gadget)) {
        log_warn("%s: stale gadget directory, removing", d->busid);
        gadget_remove_tree(d, d->nifaces);
        snprintf(d->gadget, sizeof(d->gadget), GADGET_ROOT "/usbpi-%s",
                 d->busid);
        snprintf(d->c1, sizeof(d->c1), "%s/configs/c.1", d->gadget);
    }

    if (mkdir(d->gadget, 0775) < 0) {
        log_err("%s: cannot create gadget: %s", d->busid, strerror(errno));
        return -1;
    }

    snprintf(buf, sizeof(buf), "0x%.4s", d->vid);
    if (write_text(gen_path("%s/idVendor", d->gadget), buf) < 0)
        goto fail;
    snprintf(buf, sizeof(buf), "0x%.4s", d->pid);
    if (write_text(gen_path("%s/idProduct", d->gadget), buf) < 0)
        goto fail;
    snprintf(buf, sizeof(buf), "0x%.4s", d->bcd);
    if (write_text(gen_path("%s/bcdDevice", d->gadget), buf) < 0)
        goto fail;

    snprintf(path, sizeof(path), "%s/strings/0x409", d->gadget);
    if (mkdir(path, 0775) < 0 && errno != EEXIST)
        goto fail;
    if (d->manufacturer[0])
        write_text(gen_path("%s/strings/0x409/manufacturer", d->gadget),
                   d->manufacturer);
    if (d->product[0])
        write_text(gen_path("%s/strings/0x409/product", d->gadget),
                   d->product);
    if (d->serial[0])
        write_text(gen_path("%s/strings/0x409/serialnumber", d->gadget),
                   d->serial);

    if (mkdir(d->c1, 0775) < 0 && errno != EEXIST)
        goto fail;

    for (i = 0; i < d->nifaces; i++) {
        char fdir[PBUF], line[PATH_MAX];

        f = &d->ifaces[i];
        snprintf(fdir, sizeof(fdir), "%s/functions/hid.usb%d", d->gadget,
                 f->ifnum);
        if (mkdir(fdir, 0775) < 0 && errno != EEXIST) {
            log_err("%s: cannot create hid function: %s", d->busid,
                    strerror(errno));
            goto fail;
        }
        created = i + 1;
        snprintf(line, sizeof(line), "%d", f->subclass);
        if (write_text(gen_path("%s/subclass", fdir), line) < 0)
            goto fail;
        snprintf(line, sizeof(line), "%d", f->protocol);
        if (write_text(gen_path("%s/protocol", fdir), line) < 0)
            goto fail;
        if (write_text(gen_path("%s/no_out_endpoint", fdir), "1") < 0)
            goto fail;
        snprintf(line, sizeof(line), "%u", f->report_len);
        if (write_text(gen_path("%s/report_length", fdir), line) < 0)
            goto fail;
        if (write_bin(gen_path("%s/report_desc", fdir), f->report_desc,
                      f->report_desc_len) < 0)
            goto fail;
        /* configfs resolves the symlink target against the process cwd,
         * so link with the absolute function path */
        if (symlink(fdir, gen_path("%s/hid.usb%d", d->c1, f->ifnum)) < 0 &&
            errno != EEXIST)
            goto fail;
        hidg_path_for_minor(fdir, f->hidg, sizeof(f->hidg));
    }

    if (pick_udc(d->udc, sizeof(d->udc)) < 0) {
        log_warn("%s: no free usbip-vudc instance", d->busid);
        goto fail;
    }
    if (write_text(gen_path("%s/UDC", d->gadget), d->udc) < 0) {
        log_err("%s: cannot bind to %s: %s", d->busid, d->udc,
                strerror(errno));
        goto fail;
    }

    for (i = 0; i < d->nifaces; i++) {
        f = &d->ifaces[i];
        f->g_fd = open_hidg_wait(f->hidg, 2000);
        if (f->g_fd < 0) {
            log_err("%s: %s did not appear: %s", d->busid, f->hidg,
                    strerror(errno));
            goto fail;
        }
    }
    for (i = 0; i < d->nifaces; i++) {
        f = &d->ifaces[i];
        f->qlimit = g_cfg.queue_limit;
        f->q = calloc(f->qlimit, f->report_len);
        f->qlen = calloc(f->qlimit, sizeof(*f->qlen));
        if (!f->q || !f->qlen) {
            log_err("%s: out of memory for report queue", d->busid);
            goto fail;
        }
    }

    d->active = true;
    active_gadgets++;
    active_ifaces += d->nifaces;
    log_info("%s: mirroring %d HID interface(s) via %s (%s:%s)", d->busid,
             d->nifaces, d->udc, d->vid, d->pid);
    for (i = 0; i < d->nifaces; i++) {
        f = &d->ifaces[i];
        log_info("%s: %s -> %s (%s, report %u bytes)", d->busid, f->hidraw,
                 f->hidg, f->is_keyboard ? "keyboard" : "other HID",
                 f->report_len);
    }
    return 0;

fail:
    log_err("%s: gadget setup failed, leaving on usbip-host", d->busid);
    for (i = 0; i < d->nifaces; i++) {
        f = &d->ifaces[i];
        if (f->g_fd >= 0) {
            close(f->g_fd);
            f->g_fd = -1;
        }
        free(f->q);
        f->q = NULL;
        free(f->qlen);
        f->qlen = NULL;
        f->qcount = 0;
        f->qhead = 0;
    }
    gadget_remove_tree(d, created);
    d->failed = true;
    d->retry_after = time(NULL) + RETRY_FAILED_SEC;
    unclaim_device(d);
    return -1;
}

static void teardown_gadget(struct device *d)
{
    struct iface *f;
    int i;

    if (d->active) {
        for (i = 0; i < d->nifaces; i++) {
            f = &d->ifaces[i];
            if (f->g_fd >= 0) {
                close(f->g_fd);
                f->g_fd = -1;
            }
            if (f->raw_fd >= 0) {
                close(f->raw_fd);
                f->raw_fd = -1;
            }
            free(f->q);
            free(f->qlen);
            f->q = NULL;
            f->qlen = NULL;
            f->qcount = 0;
            f->qhead = 0;
        }
        active_gadgets--;
        active_ifaces -= d->nifaces;
        d->active = false;
        log_info("%s: mirror stopped", d->busid);
    }
    gadget_remove_tree(d, d->nifaces);
}

/* remove leftovers from a previous run */
static void cleanup_stale_gadgets(void)
{
    DIR *d;
    struct dirent *e;

    d = opendir(GADGET_ROOT);
    if (!d)
        return;
    while ((e = readdir(d))) {
        char path[PBUF];
        int i;

        if (strncmp(e->d_name, "usbpi-", 6))
            continue;
        snprintf(path, sizeof(path), GADGET_ROOT "/%s/UDC", e->d_name);
        if (exists(path))
            write_text(path, "");
        log_info("removing stale gadget %s", e->d_name);
        for (i = 0; i < MAX_IFACES; i++) {
            snprintf(path, sizeof(path),
                     GADGET_ROOT "/%s/configs/c.1/hid.usb%d", e->d_name, i);
            remove_file(path);
        }
        for (i = 0; i < MAX_IFACES; i++) {
            snprintf(path, sizeof(path), GADGET_ROOT "/%s/functions/hid.usb%d",
                     e->d_name, i);
            remove_dir(path);
        }
        snprintf(path, sizeof(path), GADGET_ROOT "/%s/functions", e->d_name);
        remove_dir(path);
        snprintf(path, sizeof(path), GADGET_ROOT "/%s/strings/0x409",
                 e->d_name);
        remove_dir(path);
        snprintf(path, sizeof(path), GADGET_ROOT "/%s/strings", e->d_name);
        remove_dir(path);
        snprintf(path, sizeof(path), GADGET_ROOT "/%s/configs/c.1",
                 e->d_name);
        remove_dir(path);
        snprintf(path, sizeof(path), GADGET_ROOT "/%s/configs", e->d_name);
        remove_dir(path);
        snprintf(path, sizeof(path), GADGET_ROOT "/%s", e->d_name);
        remove_dir(path);
    }
    closedir(d);
}

/* ------------------------------------------------------------------ */
/* discovery pass                                                      */
/* ------------------------------------------------------------------ */

static int hidraw_ready(struct device *d)
{
    struct iface *f;
    int i;

    for (i = 0; i < d->nifaces; i++) {
        f = &d->ifaces[i];
        if (f->raw_fd >= 0)
            continue;
        if (!f->hidraw[0]) {
            char ifdir[PBUF];

            snprintf(ifdir, sizeof(ifdir), USB_DEVICES "/%s", f->ifname);
            if (find_hidraw(ifdir, f->hidraw, sizeof(f->hidraw)) < 0) {
                log_dbg("%s: no hidraw node for %s yet", d->busid,
                        f->ifname);
                return -1;
            }
        }
        f->raw_fd = open(f->hidraw, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (f->raw_fd < 0) {
            log_dbg("%s: cannot open %s: %s", d->busid, f->hidraw,
                    strerror(errno));
            f->hidraw[0] = '\0';
            return -1;
        }
    }
    return 0;
}

static int load_report_info(struct device *d)
{
    struct iface *f;
    int i;

    for (i = 0; i < d->nifaces; i++) {
        f = &d->ifaces[i];
        struct hidraw_report_descriptor rdesc;
        unsigned maxdesc, maxep = 0, len;
        char ifdir[PBUF];
        DIR *dir;
        struct dirent *e;

        if (f->report_len)                 /* already parsed */
            continue;
        memset(&rdesc, 0, sizeof(rdesc));
        if (ioctl(f->raw_fd, HIDIOCGRDESCSIZE, &rdesc.size) < 0) {
            log_warn("%s: %s: cannot read report descriptor size: %s",
                     d->busid, f->hidraw, strerror(errno));
            return -1;
        }
        if (rdesc.size > sizeof(rdesc.value)) {
            log_warn("%s: %s: descriptor too large (%u)", d->busid,
                     f->hidraw, rdesc.size);
            return -1;
        }
        if (ioctl(f->raw_fd, HIDIOCGRDESC, &rdesc) < 0) {
            log_warn("%s: %s: cannot read report descriptor: %s", d->busid,
                     f->hidraw, strerror(errno));
            return -1;
        }
        memcpy(f->report_desc, rdesc.value, rdesc.size);
        f->report_desc_len = rdesc.size;
        maxdesc = max_input_report(rdesc.value, rdesc.size);

        /* endpoint max packet as an upper bound */
        snprintf(ifdir, sizeof(ifdir), USB_DEVICES "/%s", f->ifname);
        dir = opendir(ifdir);
        if (dir) {
            while ((e = readdir(dir))) {
                char ep[PBUF], buf[64];
                unsigned long v;

                if (strncmp(e->d_name, "ep_", 3))
                    continue;
                snprintf(ep, sizeof(ep), "%s/%s/type", ifdir, e->d_name);
                if (read_text(ep, buf, sizeof(buf)) < 0 ||
                    strcmp(buf, "Interrupt"))
                    continue;
                snprintf(ep, sizeof(ep), "%s/%s/direction", ifdir,
                         e->d_name);
                if (read_text(ep, buf, sizeof(buf)) < 0 || strcmp(buf, "in"))
                    continue;
                snprintf(ep, sizeof(ep), "%s/%s/wMaxPacketSize", ifdir,
                         e->d_name);
                if (read_hex_attr(ep, &v) == 0 && v > maxep)
                    maxep = (unsigned)v;
            }
            closedir(dir);
        }

        len = maxdesc > maxep ? maxdesc : maxep;
        if (len == 0)
            len = 8;
        if (len > MAX_REPORT_LEN) {
            log_warn("%s: %s: report size %u > %d, leaving on usbip-host",
                     d->busid, f->hidraw, len, MAX_REPORT_LEN);
            return -1;
        }
        f->report_len = len;
    }
    return 0;
}

static void handle_device(struct device *d)
{
    char path[PBUF], st[16];
    bool bound, in_use = false;
    int vc;

    if (d->active)
        return;
    if (d->failed && time(NULL) < d->retry_after)
        return;

    snprintf(path, sizeof(path), USBIP_HOST "/%s", d->busid);
    bound = exists(path);
    if (bound) {
        snprintf(path, sizeof(path), USBIP_HOST "/%s/usbip_status",
                 d->busid);
        if (read_text(path, st, sizeof(st)) == 0 && atoi(st) == 2)
            in_use = true;
    }

    if (in_use) {
        if (!d->wait_logged) {
            log_info("%s: in use by a usbip client, waiting for release",
                     d->busid);
            d->wait_logged = true;
        }
        return;
    }

    if (bound) {
        /* ensure usbhid owns the device so we can read reports locally */
        if (opt_dry_run) {
            if (!d->wait_logged) {
                log_info("dry-run: would claim %s (%s:%s) and unbind "
                         "usbip-host", d->busid, d->vid, d->pid);
                d->wait_logged = true;
            }
            return;
        }
        claim_device(d, "unbinding from usbip-host");
        snprintf(path, sizeof(path), USBIP_HOST "/unbind");
        if (write_text(path, d->busid) < 0) {
            log_err("%s: unbind failed: %s", d->busid, strerror(errno));
            unclaim_device(d);
            d->failed = true;
            d->retry_after = time(NULL) + RETRY_FAILED_SEC;
            return;
        }
        log_info("%s: unbound from usbip-host, usbhid takes over", d->busid);
        d->wait_logged = false;
        return;                        /* hidraw appears on the next pass */
    }

    /* budget check before claiming anything new */
    if (!d->claimed && !opt_dry_run) {
        vc = vudc_count();
        if (vc == 0) {
            if (!d->wait_logged) {
                log_warn("%s: usbip-vudc is not available; is the module "
                         "loaded?", d->busid);
                d->wait_logged = true;
            }
            return;
        }
        if (active_ifaces + d->nifaces > HIDG_MINORS) {
            log_warn("%s: HID gadget budget exhausted (%d interface(s) in "
                     "use), leaving on usbip-host", d->busid, active_ifaces);
            d->failed = true;
            d->retry_after = time(NULL) + RETRY_FAILED_SEC;
            return;
        }
        if (active_gadgets >= vc) {
            log_warn("%s: no free usbip-vudc instance, leaving on usbip-host",
                     d->busid);
            d->failed = true;
            d->retry_after = time(NULL) + RETRY_FAILED_SEC;
            return;
        }
    }

    if (hidraw_ready(d) < 0 || load_report_info(d) < 0) {
        if (!d->wait_logged) {
            log_info("%s: waiting for all HID interfaces to be ready",
                     d->busid);
            d->wait_logged = true;
        }
        return;
    }

    if (opt_dry_run) {
        if (!d->wait_logged) {
            log_info("dry-run: %s ready: %d interface(s), would create "
                     "gadget", d->busid, d->nifaces);
            d->wait_logged = true;
        }
        return;
    }
    d->wait_logged = false;

    if (!d->claimed)
        claim_device(d, "mirroring HID interfaces");
    setup_gadget(d);
}

/* tear down a device, keeping an explicitly failed one from being retried
 * until it is physically replugged */
static void drop_device(struct device *d)
{
    bool was_active = d->active;

    teardown_gadget(d);
    unclaim_device(d);
    if (was_active && d->bad) {
        d->failed = true;
        d->retry_after = time(NULL) + 3600;
    }
}

static void scan(void)
{
    struct device *d, **pp;
    DIR *dir;
    struct dirent *e;

    for (d = devices; d; d = d->next)
        d->seen = false;

    dir = opendir(USB_DEVICES);
    if (!dir) {
        log_err("cannot open %s: %s", USB_DEVICES, strerror(errno));
        return;
    }
    while ((e = readdir(dir))) {
        struct scan_dev sd;
        char devdir[PBUF], ifdir[PBUF];
        struct device *dev;
        int i;

        if (strchr(e->d_name, '-') == NULL || strchr(e->d_name, ':'))
            continue;
        snprintf(devdir, sizeof(devdir), USB_DEVICES "/%s", e->d_name);
        if (!is_dir(devdir))
            continue;
        if (inspect_device(e->d_name, devdir, &sd) < 0)
            continue;
        if (is_excluded(&g_cfg, sd.vid, sd.pid)) {
            log_dbg("%s: excluded by configuration", e->d_name);
            continue;
        }

        dev = device_find(e->d_name);
        if (!dev) {
            dev = calloc(1, sizeof(*dev));
            if (!dev)
                continue;
            dev->next = devices;
            devices = dev;
            snprintf(dev->busid, sizeof(dev->busid), "%s", e->d_name);
            for (i = 0; i < MAX_IFACES; i++) {
                dev->ifaces[i].raw_fd = -1;
                dev->ifaces[i].g_fd = -1;
            }
        }
        dev->seen = true;
        snprintf(dev->dir, sizeof(dev->dir), "%s", devdir);
        snprintf(dev->vid, sizeof(dev->vid), "%s", sd.vid);
        snprintf(dev->pid, sizeof(dev->pid), "%s", sd.pid);
        snprintf(dev->bcd, sizeof(dev->bcd), "%s", sd.bcd);
        snprintf(dev->manufacturer, sizeof(dev->manufacturer), "%s",
                 sd.manufacturer);
        snprintf(dev->product, sizeof(dev->product), "%s", sd.product);
        snprintf(dev->serial, sizeof(dev->serial), "%s", sd.serial);

        if (dev->active) {
            bool same = dev->nifaces == sd.nifaces;
            int j;

            for (j = 0; same && j < sd.nifaces; j++)
                if (strcmp(dev->ifaces[j].ifname, sd.ifnames[j]))
                    same = false;
            if (!same) {
                log_info("%s: interface layout changed, re-creating mirror",
                         dev->busid);
                drop_device(dev);
            }
        }

        dev->nifaces = sd.nifaces;
        for (i = 0; i < sd.nifaces; i++) {
            struct iface tmp;

            snprintf(ifdir, sizeof(ifdir), USB_DEVICES "/%s", sd.ifnames[i]);
            if (fill_iface(&tmp, sd.ifnames[i]) < 0)
                continue;
            if (dev->active) {
                /* preserve live state across rescans */
                struct iface *cur = &dev->ifaces[i];

                tmp.raw_fd = cur->raw_fd;
                tmp.g_fd = cur->g_fd;
                tmp.q = cur->q;
                tmp.qlen = cur->qlen;
                tmp.qlimit = cur->qlimit;
                tmp.qhead = cur->qhead;
                tmp.qcount = cur->qcount;
                tmp.drops = cur->drops;
                tmp.last_drop_log = cur->last_drop_log;
                memcpy(tmp.last, cur->last, sizeof(tmp.last));
                tmp.last_len = cur->last_len;
                tmp.have_last = cur->have_last;
                tmp.attached_hint = cur->attached_hint;
                tmp.detached_logged = cur->detached_logged;
                tmp.report_len = cur->report_len;
                memcpy(tmp.report_desc, cur->report_desc,
                       sizeof(tmp.report_desc));
                tmp.report_desc_len = cur->report_desc_len;
                snprintf(tmp.hidraw, sizeof(tmp.hidraw), "%s", cur->hidraw);
                snprintf(tmp.hidg, sizeof(tmp.hidg), "%s", cur->hidg);
            }
            dev->ifaces[i] = tmp;
        }
        if (dev->bad) {
            drop_device(dev);
            continue;
        }
        handle_device(dev);
    }
    closedir(dir);

    /* remove devices that disappeared */
    pp = &devices;
    while ((d = *pp)) {
        if (!d->seen) {
            drop_device(d);
            *pp = d->next;
            device_free(d);
            claim_dirty = true;
            continue;
        }
        pp = &d->next;
    }

    if (claim_dirty)
        write_claim_file();
}

/* ------------------------------------------------------------------ */
/* main loop                                                           */
/* ------------------------------------------------------------------ */

static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static volatile sig_atomic_t stop_requested;

static void mount_configfs_if_needed(void)
{
    if (is_dir(CONFIGFS_DIR "/usb_gadget"))
        return;
    if (mkdir(CONFIGFS_DIR, 0755) < 0 && errno != EEXIST)
        log_warn("cannot create %s: %s", CONFIGFS_DIR, strerror(errno));
    if (mount("none", CONFIGFS_DIR, "configfs", 0, NULL) < 0)
        log_warn("cannot mount configfs: %s", strerror(errno));
}

static void shutdown_cleanup(void)
{
    struct device *d, *next;

    for (d = devices; d; d = next) {
        next = d->next;
        drop_device(d);
        device_free(d);
    }
    devices = NULL;
    write_claim_file();
}

static void usage(FILE *out)
{
    fprintf(out,
        "usage: usbpi-hid-mirror [options]\n"
        "\n"
        "  -c FILE   configuration file (default " DEFAULT_CONF ")\n"
        "  -n        dry run: scan and log, change nothing\n"
        "  -v        verbose\n"
        "  -h        this help\n");
}

int main(int argc, char **argv)
{
    const char *conf = DEFAULT_CONF;
    int opt, sigfd;
    sigset_t mask;
    int64_t next_scan;
    struct pollfd *fds = NULL;
    struct iface **map = NULL;
    int cap = 0;

    while ((opt = getopt(argc, argv, "c:nvh")) != -1) {
        switch (opt) {
        case 'c':
            conf = optarg;
            break;
        case 'n':
            opt_dry_run = true;
            break;
        case 'v':
            opt_verbose = true;
            break;
        case 'h':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return 1;
        }
    }

    load_config(conf, &g_cfg);
    if (!g_cfg.enabled) {
        log_info("disabled by configuration, exiting");
        return 0;
    }

    if (!opt_dry_run) {
        mkdir(CLAIM_DIR, 0755);
        mount_configfs_if_needed();
        cleanup_stale_gadgets();
    }

    log_info("starting (%s)%s", conf, opt_dry_run ? " [dry-run]" : "");

    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    sigfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);

    next_scan = now_ms();

    while (!stop_requested) {
        struct signalfd_siginfo si;
        int timeout, i, idx, nfds;

        if (now_ms() >= next_scan) {
            scan();
            next_scan = now_ms() + g_cfg.rescan_ms;
        }
        timeout = (int)(next_scan - now_ms());
        if (timeout < 0)
            timeout = 0;

        nfds = (sigfd >= 0) ? 1 : 0;
        for (struct device *d = devices; d; d = d->next) {
            if (!d->active)
                continue;
            for (i = 0; i < d->nifaces; i++)
                if (d->ifaces[i].raw_fd >= 0 && d->ifaces[i].g_fd >= 0)
                    nfds += 2;
        }
        if (nfds > cap) {
            cap = nfds + 16;
            fds = realloc(fds, (size_t)cap * sizeof(*fds));
            map = realloc(map, (size_t)cap * sizeof(*map));
            if (!fds || !map) {
                log_err("out of memory");
                break;
            }
        }

        idx = 0;
        if (sigfd >= 0) {
            fds[idx].fd = sigfd;
            fds[idx].events = POLLIN;
            fds[idx].revents = 0;
            map[idx] = NULL;
            idx++;
        }
        for (struct device *d = devices; d; d = d->next) {
            if (!d->active)
                continue;
            for (i = 0; i < d->nifaces; i++) {
                struct iface *f = &d->ifaces[i];

                if (f->raw_fd < 0 || f->g_fd < 0)
                    continue;
                fds[idx].fd = f->raw_fd;
                fds[idx].events = POLLIN;
                fds[idx].revents = 0;
                map[idx] = f;
                idx++;
                fds[idx].fd = f->g_fd;
                fds[idx].events = POLLOUT;
                fds[idx].revents = 0;
                map[idx] = f;
                idx++;
            }
        }

        if (poll(fds, (nfds_t)idx, timeout) < 0) {
            if (errno == EINTR)
                continue;
            log_err("poll: %s", strerror(errno));
            break;
        }

        for (i = 0; i < idx; i++) {
            struct iface *f = map[i];

            if (fds[i].revents == 0)
                continue;
            if (!f) {                          /* signalfd */
                if ((fds[i].revents & POLLIN) &&
                    read(sigfd, &si, sizeof(si)) > 0) {
                    log_info("signal %d, shutting down", si.ssi_signo);
                    stop_requested = 1;
                }
                continue;
            }
            if (f->bad)
                continue;
            if (fds[i].revents & POLLIN) {
                unsigned char buf[REPORT_DESC_MAX];
                int k;

                for (k = 0; k < 256; k++) {
                    ssize_t n = read(f->raw_fd, buf, sizeof(buf));

                    if (n < 0) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK &&
                            errno != EINTR)
                            mark_iface_bad(f, strerror(errno));
                        break;
                    }
                    if (n == 0)
                        break;
                    if ((unsigned)n > f->report_len) {
                        char why[64];

                        snprintf(why, sizeof(why),
                                 "report of %zd bytes exceeds %u", n,
                                 f->report_len);
                        mark_iface_bad(f, why);
                        break;
                    }
                    queue_push(f, buf, (unsigned)n);
                }
            }
            if (fds[i].revents & POLLOUT) {
                int k;

                for (k = 0; k < 256 && f->qcount > 0; k++)
                    if (queue_pop_write(f) < 0)
                        break;
            }
            if (f->bad) {
                next_scan = now_ms();
                continue;
            }
            if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                int64_t t = now_ms();

                if (t < next_scan)
                    next_scan = t;
            }
        }
    }

    shutdown_cleanup();
    if (sigfd >= 0)
        close(sigfd);
    free(fds);
    free(map);
    log_info("stopped");
    return 0;
}
