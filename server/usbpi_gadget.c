/* ------------------------------------------------------------------ */
/* gadget setup / teardown                                             */
/* ------------------------------------------------------------------ */

/*
 * Resolve the /dev/hidgN node backing a configfs HID function.  The function's
 * `dev` attribute holds "major:minor" and is populated as soon as the function
 * is created, before the gadget is bound.  Build that attribute path in a PBUF
 * buffer: a 64-byte buffer truncates it for long busids and silently yields
 * "/dev/hidg?", which only fails after the UDC has already been bound.
 */
static int hidg_path_for_minor(const char *function_dir, char *out,
                               size_t outsz, const char *busid)
{
    char path[PBUF], buf[SYSFS_SMALL_BUF], *colon;

    snprintf(out, outsz, "%s", "/dev/hidg?");
    snprintf(path, sizeof(path), "%s/dev", function_dir);
    if (read_text(path, buf, sizeof(buf)) < 0) {
        log_err("%s: cannot read %s: %s", busid, path, strerror(errno));
        return -1;
    }
    colon = strchr(buf, ':');
    if (!colon) {
        log_err("%s: %s is not \"major:minor\" (\"%s\")", busid, path, buf);
        return -1;
    }
    snprintf(out, outsz, "/dev/hidg%lu", strtoul(colon + 1, NULL, 10));
    return 0;
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
        if (hidg_path_for_minor(fdir, f->hidg, sizeof(f->hidg),
                                d->busid) < 0)
            goto fail;
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
        f->g_fd = open_hidg_wait(f->hidg, HIDG_OPEN_TIMEOUT_MS);
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

