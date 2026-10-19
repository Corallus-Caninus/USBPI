/* ------------------------------------------------------------------ */
/* keyboard fast path: evdev -> TCP -> client uinput                   */
/* ------------------------------------------------------------------ */

struct kbd_dev {
    struct kbd_dev *next;
    char busid[NAME_MAX_LEN];
    char evdev[PATH_MAX];
    char name[NAME_MAX_LEN * 2];
    int fd;
    bool writable;
};

static struct kbd_dev *kbd_devs;
static int kbd_listen_fd = -1;
static int kbd_client_fd = -1;
static uint32_t kbd_session;
static uint32_t kbd_seq;
static unsigned char kbd_state[KBD_KEY_BYTES];
static struct kbd_ringev kbd_ring[KBD_RING_EVENTS];
static unsigned kbd_ring_head;
static unsigned kbd_ring_count;
static uint32_t kbd_last_ack;
static int64_t kbd_last_send_ms;
static bool kbd_dirty;
static uint8_t kbd_led;
static unsigned char kbd_outbuf[KBD_CLIENT_OUTBUF];
static size_t kbd_out_head;
static size_t kbd_out_len;
static unsigned char kbd_inbuf[KBD_CLIENT_INBUF];
static size_t kbd_in_len;

static bool kbd_is_modifier(uint16_t code)
{
    switch (code) {
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT:
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL:
    case KEY_LEFTALT:
    case KEY_RIGHTALT:
    case KEY_LEFTMETA:
    case KEY_RIGHTMETA:
        return true;
    default:
        return false;
    }
}

static bool kbd_state_get(uint16_t code)
{
    if (code > KEY_MAX)
        return false;
    return (kbd_state[code / 8] & (1u << (code % 8))) != 0;
}

static void kbd_state_set(uint16_t code, uint8_t value)
{
    if (code > KEY_MAX)
        return;
    if (value)
        kbd_state[code / 8] |= (unsigned char)(1u << (code % 8));
    else
        kbd_state[code / 8] &= (unsigned char)~(1u << (code % 8));
}

static bool kbd_any_held(void)
{
    size_t i;

    for (i = 0; i < sizeof(kbd_state); i++)
        if (kbd_state[i])
            return true;
    return false;
}

static uint32_t kbd_ring_first(void)
{
    if (!kbd_ring_count)
        return kbd_seq + 1;
    return kbd_ring[kbd_ring_head].seq;
}

static void kbd_ring_push(uint32_t seq, uint16_t code, uint8_t value)
{
    unsigned tail;

    if (kbd_ring_count == KBD_RING_EVENTS) {
        kbd_ring_head = (kbd_ring_head + 1) % KBD_RING_EVENTS;
        kbd_ring_count--;
    }
    tail = (kbd_ring_head + kbd_ring_count) % KBD_RING_EVENTS;
    kbd_ring[tail].seq = seq;
    kbd_ring[tail].code = code;
    kbd_ring[tail].value = value;
    kbd_ring_count++;
}

static unsigned kbd_ring_window(struct kbd_ringev *out, unsigned max)
{
    unsigned n = kbd_ring_count < max ? kbd_ring_count : max;
    unsigned start = (kbd_ring_head + kbd_ring_count - n) % KBD_RING_EVENTS;
    unsigned i;

    for (i = 0; i < n; i++)
        out[i] = kbd_ring[(start + i) % KBD_RING_EVENTS];
    return n;
}

static unsigned kbd_ring_after(uint32_t after, struct kbd_ringev *out,
                               unsigned max)
{
    unsigned i, n = 0;

    for (i = 0; i < kbd_ring_count && n < max; i++) {
        struct kbd_ringev *e =
            &kbd_ring[(kbd_ring_head + i) % KBD_RING_EVENTS];

        if (e->seq > after)
            out[n++] = *e;
    }
    return n;
}

static uint32_t kbd_new_session(void)
{
    uint32_t s = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

    if (fd >= 0) {
        if (read(fd, &s, sizeof(s)) != (ssize_t)sizeof(s))
            s = 0;
        close(fd);
    }
    if (!s)
        s = (uint32_t)time(NULL) ^ ((uint32_t)getpid() << 16);
    return s ? s : 1;
}

static void kbd_drop_client(const char *why)
{
    if (kbd_client_fd >= 0) {
        log_info("keyboard client disconnected (%s)", why);
        close(kbd_client_fd);
        kbd_client_fd = -1;
    }
    kbd_out_head = 0;
    kbd_out_len = 0;
    kbd_in_len = 0;
}

static void kbd_out_enqueue(const unsigned char *p, size_t n)
{
    size_t tail, first;

    if (kbd_client_fd < 0)
        return;
    if (kbd_out_len + n > sizeof(kbd_outbuf)) {
        kbd_drop_client("output buffer overflow");
        return;
    }
    tail = (kbd_out_head + kbd_out_len) % sizeof(kbd_outbuf);
    first = n;
    if (first > sizeof(kbd_outbuf) - tail)
        first = sizeof(kbd_outbuf) - tail;
    memcpy(kbd_outbuf + tail, p, first);
    if (n > first)
        memcpy(kbd_outbuf, p + first, n - first);
    kbd_out_len += n;
}

static void kbd_out_flush(void)
{
    while (kbd_client_fd >= 0 && kbd_out_len > 0) {
        size_t chunk = sizeof(kbd_outbuf) - kbd_out_head;
        ssize_t n;

        if (chunk > kbd_out_len)
            chunk = kbd_out_len;
        n = write(kbd_client_fd, kbd_outbuf + kbd_out_head, chunk);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            kbd_drop_client(strerror(errno));
            return;
        }
        if (n == 0)
            return;
        kbd_out_head = (kbd_out_head + (size_t)n) % sizeof(kbd_outbuf);
        kbd_out_len -= (size_t)n;
    }
}

static void kbd_send_state_packet(uint8_t type, uint32_t flags,
                                  const struct kbd_ringev *evs,
                                  unsigned count)
{
    uint8_t pkt[KBD_MAX_PKT];
    size_t n;

    n = kbd_pkt_state(pkt, sizeof(pkt), type, kbd_session, kbd_seq, flags,
                      kbd_state, KBD_KEY_BYTES, evs, count);
    if (n)
        kbd_out_enqueue(pkt, n);
    kbd_last_send_ms = now_ms();
}

static void kbd_send_snapshot(uint32_t flags)
{
    struct kbd_ringev evs[KBD_MAX_EVENTS_PKT];
    unsigned n = kbd_ring_window(evs, KBD_MAX_EVENTS_PKT);

    kbd_send_state_packet(KBD_PKT_SNAPSHOT, flags, evs, n);
}

static void kbd_send_events(void)
{
    struct kbd_ringev evs[KBD_MAX_EVENTS_PKT];
    unsigned n = kbd_ring_window(evs, KBD_MAX_EVENTS_PKT);

    kbd_send_state_packet(KBD_PKT_EVENTS, 0, evs, n);
}

/* Replay everything after the client's last applied sequence.  If the ring
 * no longer covers it, send current state and a GAP marker instead. */
static void kbd_send_replay(void)
{
    uint32_t after = kbd_last_ack;
    bool gap = false;

    if (kbd_ring_count) {
        uint32_t first = kbd_ring_first();

        if (kbd_last_ack + 1 < first || kbd_last_ack > kbd_seq)
            gap = true;
    } else if (kbd_last_ack != kbd_seq) {
        gap = true;
    }

    if (gap) {
        kbd_send_snapshot(KBD_FLAG_GAP);
        return;
    }

    for (;;) {
        struct kbd_ringev batch[KBD_MAX_EVENTS_PKT];
        unsigned n = kbd_ring_after(after, batch, KBD_MAX_EVENTS_PKT);

        if (!n)
            break;
        kbd_send_state_packet(KBD_PKT_EVENTS, 0, batch, n);
        after = batch[n - 1].seq;
        if (after >= kbd_seq)
            break;
    }
    kbd_send_snapshot(0);
}

static void kbd_add_event(uint16_t code, uint8_t value)
{
    if (code > KEY_MAX)
        return;
    if (kbd_state_get(code) == (value != 0))
        return;
    kbd_seq++;
    kbd_state_set(code, value);
    kbd_ring_push(kbd_seq, code, value);
    kbd_dirty = true;
}

/* Release everything, non-modifiers first and modifiers last. */
static void kbd_release_all(void)
{
    uint16_t code;
    int pass;

    for (pass = 0; pass < 2; pass++) {
        for (code = 0; code <= KEY_MAX; code++) {
            if (!kbd_state_get(code))
                continue;
            if (kbd_is_modifier(code) != (pass == 1))
                continue;
            kbd_add_event(code, 0);
        }
    }
}

static void kbd_apply_led(void)
{
    static const uint16_t leds[] = { LED_NUML, LED_CAPSL, LED_SCROLLL };
    struct kbd_dev *k;

    for (k = kbd_devs; k; k = k->next) {
        struct input_event ev;
        size_t i;

        if (!k->writable)
            continue;
        for (i = 0; i < sizeof(leds) / sizeof(leds[0]); i++) {
            memset(&ev, 0, sizeof(ev));
            ev.type = EV_LED;
            ev.code = leds[i];
            ev.value = (kbd_led >> i) & 1;
            if (write(k->fd, &ev, sizeof(ev)) < 0) {
                /* LED feedback is best-effort */
            }
        }
        memset(&ev, 0, sizeof(ev));
        ev.type = EV_SYN;
        ev.code = SYN_REPORT;
        if (write(k->fd, &ev, sizeof(ev)) < 0) {
            /* LED feedback is best-effort */
        }
    }
}

static struct kbd_dev *kbd_find(const char *busid)
{
    struct kbd_dev *k;

    for (k = kbd_devs; k; k = k->next)
        if (!strcmp(k->busid, busid))
            return k;
    return NULL;
}

static bool kbd_path_in_use(const char *path)
{
    struct kbd_dev *k;

    for (k = kbd_devs; k; k = k->next)
        if (!strcmp(k->evdev, path))
            return true;
    return false;
}

static int kbd_find_evdev(const char *vid_s, const char *pid_s,
                          char *out, size_t outsz, char *name, size_t namesz,
                          int *fd_out, bool *writable)
{
    unsigned long vid = strtoul(vid_s, NULL, 16);
    unsigned long pid = strtoul(pid_s, NULL, 16);
    glob_t g;
    int found = -1;

    if (glob("/dev/input/event*", 0, NULL, &g) != 0)
        return -1;

    {
        size_t i;

        for (i = 0; i < g.gl_pathc; i++) {
            const char *path = g.gl_pathv[i];
            unsigned char evbits[(EV_MAX + 8) / 8];
            unsigned char keybits[KBD_KEY_BYTES];
            struct input_id id;
            bool wr = true;
            int fd;

            if (kbd_path_in_use(path))
                continue;
            fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                wr = false;
                fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            }
            if (fd < 0)
                continue;
            memset(&id, 0, sizeof(id));
            if (ioctl(fd, EVIOCGID, &id) < 0) {
                close(fd);
                continue;
            }
            memset(evbits, 0, sizeof(evbits));
            memset(keybits, 0, sizeof(keybits));
            if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0 ||
                ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0) {
                close(fd);
                continue;
            }
            if (!(evbits[EV_KEY / 8] & (1u << (EV_KEY % 8))) ||
                !(keybits[KEY_A / 8] & (1u << (KEY_A % 8))) ||
                !(keybits[KEY_ENTER / 8] & (1u << (KEY_ENTER % 8)))) {
                close(fd);
                continue;
            }
            if (id.vendor != (unsigned short)vid ||
                id.product != (unsigned short)pid) {
                close(fd);
                continue;
            }
            if (ioctl(fd, EVIOCGRAB, 1) < 0)
                log_dbg("%s: EVIOCGRAB failed: %s", path, strerror(errno));
            memset(name, 0, namesz);
            ioctl(fd, EVIOCGNAME(namesz - 1), name);
            snprintf(out, outsz, "%s", path);
            *fd_out = fd;
            *writable = wr;
            found = 0;
            break;
        }
    }
    globfree(&g);
    return found;
}

static bool device_has_keyboard(const struct device *d)
{
    int i;

    for (i = 0; i < d->nifaces; i++)
        if (d->ifaces[i].is_keyboard)
            return true;
    return false;
}

static int kbd_open_device(struct device *d)
{
    struct kbd_dev *k;
    char path[PATH_MAX], name[NAME_MAX_LEN * 2];
    int fd = -1;
    bool writable = false;

    if (kbd_find(d->busid))
        return 0;
    if (kbd_find_evdev(d->vid, d->pid, path, sizeof(path), name,
                       sizeof(name), &fd, &writable) < 0)
        return -1;

    k = calloc(1, sizeof(*k));
    if (!k) {
        close(fd);
        return -1;
    }
    snprintf(k->busid, sizeof(k->busid), "%s", d->busid);
    snprintf(k->evdev, sizeof(k->evdev), "%s", path);
    snprintf(k->name, sizeof(k->name), "%s", name);
    k->fd = fd;
    k->writable = writable;
    k->next = kbd_devs;
    kbd_devs = k;
    log_info("%s: keyboard fast path on %s (%s)", d->busid, path,
             name[0] ? name : "unnamed");
    return 0;
}

static void kbd_close_device(struct device *d)
{
    struct kbd_dev **pp = &kbd_devs;

    while (*pp) {
        struct kbd_dev *k = *pp;

        if (!strcmp(k->busid, d->busid)) {
            *pp = k->next;
            close(k->fd);
            free(k);
            kbd_release_all();
            log_info("%s: keyboard fast path stopped", d->busid);
            return;
        }
        pp = &k->next;
    }
}

static void kbd_read_device(struct kbd_dev *k)
{
    for (;;) {
        struct input_event ev;
        ssize_t n = read(k->fd, &ev, sizeof(ev));

        if (n == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_KEY && (ev.value == 0 || ev.value == 1))
                kbd_add_event(ev.code, (uint8_t)ev.value);
            continue;
        }
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            log_warn("%s: evdev read: %s", k->evdev, strerror(errno));
        }
        break;
    }
}

static bool kbd_dispatch_fd(int fd)
{
    struct kbd_dev *k;

    for (k = kbd_devs; k; k = k->next) {
        if (k->fd == fd) {
            kbd_read_device(k);
            return true;
        }
    }
    return false;
}

static void kbd_parse_in(void)
{
    size_t off = 0;

    while (kbd_in_len - off >= KBD_HDR_LEN) {
        struct kbd_wire_hdr h;
        uint8_t led = 0;
        uint16_t plen = kbd_get_u16(kbd_inbuf + off + 6);
        size_t total = KBD_HDR_LEN + (size_t)plen;

        if (kbd_in_len - off < total)
            break;
        if (kbd_pkt_client_parse(kbd_inbuf + off, total, &h, &led) < 0) {
            kbd_drop_client("malformed packet");
            return;
        }
        if (h.type == KBD_PKT_HELLO) {
            kbd_led = led;
            if (h.session != kbd_session) {
                kbd_last_ack = 0;
                kbd_send_snapshot(KBD_FLAG_GAP);
            } else {
                kbd_last_ack = h.seq;
                kbd_send_replay();
            }
        } else {
            if (h.seq > kbd_last_ack)
                kbd_last_ack = h.seq;
            if (led != kbd_led) {
                kbd_led = led;
                kbd_apply_led();
            }
        }
        off += total;
    }
    if (off) {
        memmove(kbd_inbuf, kbd_inbuf + off, kbd_in_len - off);
        kbd_in_len -= off;
    }
}

static void kbd_client_read(void)
{
    for (;;) {
        ssize_t n;

        if (kbd_in_len == sizeof(kbd_inbuf)) {
            kbd_drop_client("input buffer overflow");
            return;
        }
        n = read(kbd_client_fd, kbd_inbuf + kbd_in_len,
                 sizeof(kbd_inbuf) - kbd_in_len);
        if (n > 0) {
            kbd_in_len += (size_t)n;
            continue;
        }
        if (n == 0) {
            kbd_drop_client("peer closed");
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        kbd_drop_client(strerror(errno));
        return;
    }
    kbd_parse_in();
}

static void kbd_client_event(short revents)
{
    if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
        kbd_drop_client("socket error");
        return;
    }
    if (revents & POLLIN)
        kbd_client_read();
    if (kbd_client_fd >= 0 && (revents & POLLOUT))
        kbd_out_flush();
}

static void kbd_accept(void)
{
    for (;;) {
        int fd = accept4(kbd_listen_fd, NULL, NULL,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        int one = 1;
        int buf = KBD_SNDBUF;

        if (fd < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        if (kbd_client_fd >= 0)
            kbd_drop_client("replaced by a new connection");
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
        buf = KBD_RCVBUF;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
#ifdef TCP_USER_TIMEOUT
        {
            unsigned int uto = KBD_TCP_USER_TIMEOUT_MS;
            setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &uto, sizeof(uto));
        }
#endif
        kbd_client_fd = fd;
        kbd_out_head = 0;
        kbd_out_len = 0;
        kbd_in_len = 0;
        kbd_last_ack = 0;
        kbd_last_send_ms = now_ms();
        log_info("keyboard client connected");
        kbd_send_snapshot(0);
        kbd_out_flush();
    }
}

static int kbd_init(void)
{
    int fd, one = 1;
    struct sockaddr_in sa;

    if (!g_cfg.keyboard)
        return 0;
    kbd_session = kbd_new_session();
    fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        log_err("keyboard fast path: socket: %s", strerror(errno));
        g_cfg.keyboard = false;
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(KBD_PORT);
    if (inet_pton(AF_INET, KBD_BIND_ADDR, &sa.sin_addr) != 1 ||
        bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(fd, 4) < 0) {
        log_err("keyboard fast path: bind %s:%d: %s", KBD_BIND_ADDR,
                KBD_PORT, strerror(errno));
        close(fd);
        g_cfg.keyboard = false;
        return -1;
    }
    kbd_listen_fd = fd;
    log_info("keyboard fast path listening on %s:%d (session %08x)",
             KBD_BIND_ADDR, KBD_PORT, kbd_session);
    return 0;
}

static void kbd_service(void)
{
    int64_t now;

    if (kbd_client_fd < 0)
        return;
    now = now_ms();
    if (kbd_dirty) {
        kbd_dirty = false;
        kbd_send_events();
    } else if (now - kbd_last_send_ms >=
               (kbd_any_held() ? KBD_SNAPSHOT_MS : KBD_IDLE_SNAPSHOT_MS)) {
        kbd_send_snapshot(0);
    }
    kbd_out_flush();
}

static void kbd_shutdown(void)
{
    struct kbd_dev *k, *next;

    if (kbd_client_fd >= 0) {
        close(kbd_client_fd);
        kbd_client_fd = -1;
    }
    if (kbd_listen_fd >= 0) {
        close(kbd_listen_fd);
        kbd_listen_fd = -1;
    }
    for (k = kbd_devs; k; k = next) {
        next = k->next;
        close(k->fd);
        free(k);
    }
    kbd_devs = NULL;
}
