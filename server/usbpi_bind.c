/* ------------------------------------------------------------------ */
/* generic usbip-host export (was usbip-bind-all.sh)                   */
/* ------------------------------------------------------------------ */

static bool busid_claimed(const char *busid)
{
    struct device *d = device_find(busid);

    return d && d->claimed;
}

static bool device_has_interface_class(const char *busid, const char *dir,
                                       unsigned cls)
{
    DIR *d;
    struct dirent *e;
    char path[PBUF];
    unsigned long v;

    d = opendir(dir);
    if (!d)
        return false;
    while ((e = readdir(d))) {
        if (!iface_dirname(busid, e->d_name))
            continue;
        snprintf(path, sizeof(path), "%s/%s/bInterfaceClass", dir, e->d_name);
        if (read_hex_attr(path, &v) == 0 && v == cls) {
            closedir(d);
            return true;
        }
    }
    closedir(d);
    return false;
}

/* Any established TCP connection to the usbipd export port?  This replaces
 * the old `ss` invocation; reading /proc/net/tcp needs no process spawn. */
static bool tcp_port_established(unsigned port)
{
    FILE *f = fopen(PROC_TCP, "r");
    char line[SYSFS_VALUE_MAX];

    if (!f)
        return false;
    while (fgets(line, sizeof(line), f)) {
        char local[64], rem[64], st[8];
        char *colon;
        unsigned p;

        if (sscanf(line, " %*d: %63s %63s %7s", local, rem, st) != 3)
            continue;
        if (strcmp(st, "01") != 0)       /* 01 == ESTABLISHED */
            continue;
        colon = strrchr(local, ':');
        if (!colon)
            continue;
        p = (unsigned)strtoul(colon + 1, NULL, 16);
        if (p == port) {
            fclose(f);
            return true;
        }
    }
    fclose(f);
    return false;
}

static int usbip_bind(const char *busid)
{
    return write_text(USBIP_HOST_BIND, busid);
}

static int usbip_unbind(const char *busid)
{
    return write_text(USBIP_HOST_UNBIND, busid);
}

/* Export every eligible device that is not claimed by the HID/keyboard
 * machinery.  Mirrors the old bind-all script's filters exactly:
 *   - onboard Ethernet and its hub
 *   - hubs / root hubs (bDeviceClass 9)
 *   - any mass-storage interface (protects the boot drive)
 *   - virtual controllers and devices claimed by this daemon
 */
static void bind_pass(void)
{
    DIR *dir;
    struct dirent *e;

    dir = opendir(USB_DEVICES);
    if (!dir)
        return;

    while ((e = readdir(dir))) {
        char devdir[PBUF], path[PBUF], vid[8], pid[8], st[16];
        unsigned long cls;
        struct device *d;

        if (strchr(e->d_name, '-') == NULL || strchr(e->d_name, ':') != NULL)
            continue;
        snprintf(devdir, sizeof(devdir), USB_DEVICES "/%s", e->d_name);
        if (!is_dir(devdir))
            continue;

        snprintf(path, sizeof(path), "%s/idVendor", devdir);
        if (read_text(path, vid, sizeof(vid)) < 0)
            continue;
        snprintf(path, sizeof(path), "%s/idProduct", devdir);
        if (read_text(path, pid, sizeof(pid)) < 0)
            continue;

        if (onboard_vidpid(vid, pid))
            continue;
        if (device_is_virtual(devdir))
            continue;

        snprintf(path, sizeof(path), "%s/bDeviceClass", devdir);
        if (read_hex_attr(path, &cls) < 0)
            continue;
        if (cls == 9)                    /* hub */
            continue;
        if (device_has_interface_class(e->d_name, devdir, 8))
            continue;                    /* mass storage: boot drive */

        d = device_find(e->d_name);
        if (d && d->claimed)
            continue;                    /* mirror / keyboard fast path */
        if (busid_claimed(e->d_name))
            continue;

        snprintf(path, sizeof(path), USBIP_HOST "/%s", e->d_name);
        if (exists(path)) {
            int n;

            snprintf(path, sizeof(path), USBIP_HOST "/%s/usbip_status",
                     e->d_name);
            n = read_text(path, st, sizeof(st));
            if (n < 0)
                continue;                /* cannot tell: leave it alone */
            if (atoi(st) == 2) {
                /* Marked in use: only reset it when no client is connected. */
                if (tcp_port_established(USBIP_PORT))
                    continue;
            } else if (atoi(st) != 3) {
                continue;                /* available (1) or unknown */
            }
            log_warn("%s: resetting stuck usbip-host export (status=%s)",
                     e->d_name, st);
            usbip_unbind(e->d_name);
            usleep(1000 * 1000);
        }

        if (usbip_bind(e->d_name) == 0)
            log_info("exported %s (%s:%s)", e->d_name, vid, pid);
    }
    closedir(dir);
}
