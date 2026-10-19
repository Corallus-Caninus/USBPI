/* ------------------------------------------------------------------ */
/* discovery pass                                                      */
/* ------------------------------------------------------------------ */

static bool device_has_keyboard(const struct device *d);
static int kbd_open_device(struct device *d);
static void kbd_close_device(struct device *d);

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
                char ep[PBUF], buf[SYSFS_SMALL_BUF];
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

/*
 * Sysfs unbind does not re-run driver matching, so a device we took from
 * usbip-host is left without an owner until something probes it again.
 * Writing each interface name to the bus's drivers_probe attribute makes the
 * usbhid driver bind it, which is what creates the hidraw nodes we need.
 */
static void probe_interfaces(struct device *d)
{
    int i;

    for (i = 0; i < d->nifaces; i++)
        write_text("/sys/bus/usb/drivers_probe", d->ifaces[i].ifname);
}

static void handle_device(struct device *d)
{
    char path[PBUF], st[TCP_STATE_STR];
    bool bound, in_use = false;
    int vc;

    if (d->active || d->kbd)
        return;
    if (d->failed && time(NULL) < d->retry_after)
        return;

    /* With the keyboard fast path enabled, only keyboards are claimed here.
     * Mice, gamepads and other HID devices stay on usbip-host and are
     * exported raw by bind_pass(). */
    if (g_cfg.keyboard && (!device_has_keyboard(d) || d->kbd_no))
        return;

    snprintf(path, sizeof(path), USBIP_HOST "/%s", d->busid);
    bound = exists(path);
    if (bound) {
        int n;

        snprintf(path, sizeof(path), USBIP_HOST "/%s/usbip_status",
                 d->busid);
        /* status 2 means a usbip client is using the device: never yank it
         * out from under them.  If the status cannot be read at all, stay
         * conservative and leave the device alone as well. */
        n = read_text(path, st, sizeof(st));
        if (n < 0 || atoi(st) == 2)
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
        probe_interfaces(d);
        d->last_probe = time(NULL);
        d->wait_since = time(NULL);
        d->wait_logged = false;
        return;                        /* hidraw appears on the next pass */
    }

    /* Keyboards bypass USB/IP entirely: evdev -> TCP -> uinput.  Keeping the
     * device on usbhid gives us the kernel's own HID parsing and autorepeat. */
    if (g_cfg.keyboard && !d->kbd_no && device_has_keyboard(d)) {
        time_t now;

        if (opt_dry_run) {
            if (!d->wait_logged) {
                log_info("dry-run: would use the keyboard fast path for %s "
                         "(%s:%s)", d->busid, d->vid, d->pid);
                d->wait_logged = true;
            }
            return;
        }
        if (!d->claimed)
            claim_device(d, "keyboard fast path");
        if (kbd_open_device(d) == 0) {
            d->kbd = true;
            d->wait_since = 0;
            d->wait_logged = false;
            return;
        }
        now = time(NULL);
        if (now - d->last_probe >= PROBE_INTERVAL_SEC) {
            probe_interfaces(d);
            d->last_probe = now;
        }
        if (!d->wait_logged) {
            log_info("%s: waiting for the evdev keyboard node", d->busid);
            d->wait_logged = true;
        }
        if (!d->wait_since)
            d->wait_since = now;
        if (now - d->wait_since > KBD_WAIT_TIMEOUT_SEC) {
            log_warn("%s: no evdev keyboard after %ds, exporting raw",
                     d->busid, KBD_WAIT_TIMEOUT_SEC);
            unclaim_device(d);
            d->kbd_no = true;
            d->wait_since = 0;
            d->wait_logged = false;
        }
        return;
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
        time_t now = time(NULL);

        if (now - d->last_probe >= PROBE_INTERVAL_SEC) {
            probe_interfaces(d);
            d->last_probe = now;
        }
        if (!d->wait_logged) {
            log_info("%s: waiting for all HID interfaces to be ready",
                     d->busid);
            d->wait_logged = true;
        }
        if (d->claimed) {
            /* we already unbound usbip-host; if usbhid never comes up the
             * device would be stranded, so re-export it on this timeout */
            if (!d->wait_since)
                d->wait_since = now;
            if (now - d->wait_since > WAIT_TIMEOUT_SEC) {
                log_warn("%s: HID interfaces did not appear after %ds, "
                         "re-exporting on usbip-host", d->busid,
                         WAIT_TIMEOUT_SEC);
                unclaim_device(d);
                d->failed = true;
                d->retry_after = now + RETRY_FAILED_SEC;
                d->wait_since = 0;
                d->wait_logged = false;
            }
        }
        return;
    }
    d->wait_since = 0;

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

    if (d->kbd) {
        kbd_close_device(d);
        d->kbd = false;
    }
    teardown_gadget(d);
    unclaim_device(d);
    if (was_active && d->bad) {
        d->failed = true;
        d->retry_after = time(NULL) + STUCK_RETRY_SEC;
    }
}

static bool g_force_full_scan = true;

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

        /* Active devices are expensive to re-inspect (dozens of sysfs reads
         * and a realpath per device).  A uevent forces a full pass, so the
         * periodic pass only has to notice disappearances. */
        dev = device_find(e->d_name);
        if (!g_force_full_scan && dev && (dev->active || dev->kbd)) {
            dev->seen = true;
            continue;
        }
        if (inspect_device(e->d_name, devdir, &sd) < 0)
            continue;
        if (is_excluded(&g_cfg, sd.vid, sd.pid)) {
            log_dbg("%s: excluded by configuration", e->d_name);
            continue;
        }

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
    g_force_full_scan = false;
}

