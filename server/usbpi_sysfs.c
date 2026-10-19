/* ------------------------------------------------------------------ */
/* sysfs discovery                                                     */
/* ------------------------------------------------------------------ */

static bool onboard_vidpid(const char *vid, const char *pid)
{
    static const char *skip[] = { "0424:ec00", "0424:9514", "0424:9512" };
    char vidpid[VIDPID_LEN];
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

    /* HID boot protocol: 1 = keyboard, 2 = mouse. */
    f->is_keyboard = (f->subclass == 1 && f->protocol == 1);
    return 0;
}

struct scan_dev {
    char vid[VID_STR], pid[PID_STR], bcd[BCD_LEN];
    char manufacturer[MAX_STRING + 1];
    char product[MAX_STRING + 1];
    char serial[MAX_STRING + 1];
    int nifaces;
    char ifnames[MAX_IFACES][NAME_MAX_LEN];
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

