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
    unsigned bits[MAX_REPORT_IDS] = { 0 };
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

                if (st.report_id < MAX_REPORT_IDS && total < (1U << 16))
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

    for (i = 0; i < MAX_REPORT_IDS; i++) {
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
        if (errno == ESHUTDOWN || errno == ENODEV) {
            /* No client attached: keep buffering.  Back off before touching
             * the gadget again; a detached hidg node reports POLLOUT
             * continuously and an unthrottled retry would spin at 100% CPU. */
            if (!f->detached_logged) {
                log_info("%s: client detached (%s), buffering locally",
                         f->ifname, strerror(errno));
                f->detached_logged = true;
            }
            f->attached_hint = false;
            f->g_retry_at = now_ms() + GADGET_RETRY_MS;
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

