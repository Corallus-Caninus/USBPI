/* ------------------------------------------------------------------ */
/* device model                                                        */
/* ------------------------------------------------------------------ */

static int64_t now_ms(void);

struct iface {
    char ifname[NAME_MAX_LEN];             /* e.g. "1-1.3:1.0" */
    int ifnum;
    int subclass;
    int protocol;
    bool is_keyboard;
    char hidraw[NAME_MAX_LEN];
    char hidg[72];
    unsigned char report_desc[REPORT_DESC_MAX];
    size_t report_desc_len;
    unsigned int report_len;     /* bytes per report, including report ID */
    int raw_fd;
    int g_fd;
    int64_t g_retry_at;          /* do not poll g_fd for write before this */
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
    bool kbd;                    /* handled by the keyboard fast path */
    bool kbd_no;                 /* not a real keyboard: export raw */
    bool bad;                    /* teardown and stop mirroring this device */
    bool failed;
    time_t retry_after;
    bool wait_logged;
    time_t last_probe;           /* last usbhid re-probe attempt */
    time_t wait_since;           /* when we started waiting for hidraw */
    char busid[NAME_MAX_LEN];
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
    char udc[NAME_MAX_LEN];
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

