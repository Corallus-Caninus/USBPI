/* ------------------------------------------------------------------ */
/* client keyboard daemon: TCP -> uinput                               */
/* ------------------------------------------------------------------ */

static int keyd_fd = -1;
static int keyd_ufd = -1;
static bool keyd_uinput_readable;
static uint32_t keyd_session_seen;
static uint32_t keyd_last_applied;
static uint8_t keyd_led;
static bool keyd_released;
static unsigned char keyd_held[KBD_KEY_BYTES];
static int64_t keyd_last_pkt_ms;
static int64_t keyd_gaps[DEADMAN_GAPS];
static unsigned keyd_ngaps;
static unsigned keyd_gidx;
static int64_t keyd_last_ack_ms;

static bool keyd_is_modifier(uint16_t code)
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

static bool bits_get(const unsigned char *bits, uint16_t code)
{
    if (code > KEY_MAX)
        return false;
    return (bits[code / 8] & (1u << (code % 8))) != 0;
}

static void bits_set(unsigned char *bits, uint16_t code, bool value)
{
    if (code > KEY_MAX)
        return;
    if (value)
        bits[code / 8] |= (unsigned char)(1u << (code % 8));
    else
        bits[code / 8] &= (unsigned char)~(1u << (code % 8));
}

static bool keyd_any_held(void)
{
    size_t i;

    for (i = 0; i < sizeof(keyd_held); i++)
        if (keyd_held[i])
            return true;
    return false;
}

static void keyd_emit(uint16_t code, int value)
{
    struct input_event ev;

    if (keyd_ufd < 0)
        return;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_KEY;
    ev.code = code;
    ev.value = value;
    if (write(keyd_ufd, &ev, sizeof(ev)) < 0 && errno != EAGAIN)
        usbpi_log("uinput key write: %s", strerror(errno));
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    if (write(keyd_ufd, &ev, sizeof(ev)) < 0 && errno != EAGAIN)
        usbpi_log("uinput syn write: %s", strerror(errno));
}

static void keyd_release_all(void)
{
    uint16_t code;
    int pass;

    if (!keyd_any_held())
        return;
    for (pass = 0; pass < 2; pass++) {
        for (code = 0; code <= KEY_MAX; code++) {
            if (!bits_get(keyd_held, code))
                continue;
            if (keyd_is_modifier(code) != (pass == 1))
                continue;
            bits_set(keyd_held, code, false);
            keyd_emit(code, 0);
        }
    }
    keyd_released = true;
}

/* Apply the server's full state: release keys that are no longer down, then
 * press held keys with modifiers first (so Shift+A is never sent as 'a'). */
static void keyd_reconcile(const unsigned char *snap)
{
    bool was_held = keyd_any_held();
    uint16_t code;
    int pass;

    for (pass = 0; pass < 2; pass++) {
        for (code = 0; code <= KEY_MAX; code++) {
            if (!bits_get(keyd_held, code) || bits_get(snap, code))
                continue;
            if (keyd_is_modifier(code) != (pass == 1))
                continue;
            bits_set(keyd_held, code, false);
            keyd_emit(code, 0);
        }
    }
    for (pass = 0; pass < 2; pass++) {
        for (code = 0; code <= KEY_MAX; code++) {
            if (bits_get(keyd_held, code) || !bits_get(snap, code))
                continue;
            if (keyd_is_modifier(code) != (pass == 0))
                continue;
            bits_set(keyd_held, code, true);
            keyd_emit(code, 1);
        }
    }
    memcpy(keyd_held, snap, sizeof(keyd_held));
    if (!was_held && keyd_any_held()) {
        /* Start the dead-man from the active interval as soon as a key goes
         * down; idle snapshots must not inflate the timeout. */
        keyd_ngaps = 0;
        keyd_gidx = 0;
    }
    keyd_released = false;
}

static void keyd_note_gap(int64_t now)
{
    if (keyd_last_pkt_ms) {
        int64_t gap = now - keyd_last_pkt_ms;

        if (gap < 0)
            gap = 0;
        keyd_gaps[keyd_gidx] = gap;
        keyd_gidx = (keyd_gidx + 1) % DEADMAN_GAPS;
        if (keyd_ngaps < DEADMAN_GAPS)
            keyd_ngaps++;
    }
    keyd_last_pkt_ms = now;
}

static int64_t keyd_deadman_ms(void)
{
    int64_t maxgap = 0;
    int64_t t;
    unsigned i;

    for (i = 0; i < keyd_ngaps; i++)
        if (keyd_gaps[i] > maxgap)
            maxgap = keyd_gaps[i];
    t = maxgap * 2 + KBD_DEADMAN_MARGIN_MS;
    if (t < KBD_DEADMAN_MIN_MS)
        t = KBD_DEADMAN_MIN_MS;
    if (t > KBD_DEADMAN_MAX_MS)
        t = KBD_DEADMAN_MAX_MS;
    return t;
}

static void keyd_send_pkt(uint8_t type, uint32_t flags)
{
    uint8_t pkt[KBD_MAX_PKT];
    size_t n;

    if (keyd_fd < 0)
        return;
    n = kbd_pkt_client(pkt, sizeof(pkt), type, keyd_session_seen,
                       keyd_last_applied, flags, keyd_led);
    if (!n)
        return;
    if (write(keyd_fd, pkt, n) < 0 && errno != EAGAIN &&
        errno != EWOULDBLOCK && errno != EINTR)
        usbpi_log("keyboard socket write: %s", strerror(errno));
}

static int keyd_uinput_open(void)
{
    struct uinput_setup usetup;
    int fd, code;

    fd = open(UINPUT_DEV, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
        keyd_uinput_readable = true;
    } else {
        fd = open(UINPUT_DEV, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        keyd_uinput_readable = false;
    }
    if (fd < 0) {
        usbpi_log("cannot open %s: %s", UINPUT_DEV, strerror(errno));
        return -1;
    }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_REP);
    ioctl(fd, UI_SET_EVBIT, EV_LED);
    for (code = 1; code <= KEY_MAX; code++)
        ioctl(fd, UI_SET_KEYBIT, code);
    ioctl(fd, UI_SET_LEDBIT, LED_NUML);
    ioctl(fd, UI_SET_LEDBIT, LED_CAPSL);
    ioctl(fd, UI_SET_LEDBIT, LED_SCROLLL);

    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = UINPUT_VENDOR;
    usetup.id.product = UINPUT_PRODUCT;
    usetup.id.version = USBPI_UINPUT_VERSION;
    snprintf(usetup.name, sizeof(usetup.name), "%s", UINPUT_NAME);
    if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0 ||
        ioctl(fd, UI_DEV_CREATE) < 0) {
        usbpi_log("cannot create uinput keyboard: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int keyd_connect(const char *ip)
{
    struct sockaddr_in sa;
    struct pollfd pfd;
    int fd, err = 0;
    socklen_t elen = sizeof(err);
    int one = 1;
    int buf = KBD_SNDBUF;

    fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(KBD_PORT);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 &&
        errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    pfd.fd = fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    if (poll(&pfd, 1, PORT_PROBE_TIMEOUT_MS) <= 0 ||
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
        close(fd);
        return -1;
    }
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
    return fd;
}

static void keyd_handle_led_read(void)
{
    for (;;) {
        struct input_event ev;
        ssize_t n = read(keyd_ufd, &ev, sizeof(ev));

        if (n != (ssize_t)sizeof(ev))
            return;
        if (ev.type == EV_LED) {
            unsigned bit;

            switch (ev.code) {
            case LED_NUML: bit = 0; break;
            case LED_CAPSL: bit = 1; break;
            case LED_SCROLLL: bit = 2; break;
            default: continue;
            }
            if (ev.value)
                keyd_led |= (uint8_t)(1u << bit);
            else
                keyd_led &= (uint8_t)~(1u << bit);
            keyd_send_pkt(KBD_PKT_ACK, 0);
            keyd_last_ack_ms = cli_now_ms();
        }
    }
}

/* Parse complete packets from the input buffer. */
static void keyd_parse(unsigned char *in, size_t *inlen)
{
    size_t off = 0;

    while (*inlen - off >= KBD_HDR_LEN) {
        uint16_t plen = kbd_get_u16(in + off + 6);
        size_t total = KBD_HDR_LEN + (size_t)plen;
        struct kbd_wire_hdr h;
        struct kbd_ringev evs[KBD_MAX_EVENTS_PKT];
        unsigned char snap[KBD_KEY_BYTES];
        uint16_t count = 0;
        unsigned i;
        int64_t now;

        if (*inlen - off < total)
            break;
        if (kbd_pkt_parse(in + off, total, &h, &count, snap,
                          sizeof(snap), evs, KBD_MAX_EVENTS_PKT) < 0) {
            usbpi_log("malformed keyboard packet");
            keyd_fd = -1;
            return;
        }
        if (h.session != keyd_session_seen) {
            if (keyd_session_seen != 0) {
                usbpi_log("server session changed, resyncing");
                keyd_release_all();
            }
            keyd_session_seen = h.session;
            keyd_last_applied = 0;
        }
        for (i = 0; i < count; i++) {
            if (evs[i].seq <= keyd_last_applied)
                continue;
            if (evs[i].code <= KEY_MAX &&
                bits_get(keyd_held, evs[i].code) != (evs[i].value != 0)) {
                bits_set(keyd_held, evs[i].code, evs[i].value != 0);
                keyd_emit(evs[i].code, evs[i].value ? 1 : 0);
            }
            keyd_last_applied = evs[i].seq;
        }
        keyd_reconcile(snap);
        if (h.type == KBD_PKT_SNAPSHOT && h.seq > keyd_last_applied)
            keyd_last_applied = h.seq;
        now = cli_now_ms();
        keyd_note_gap(now);
        keyd_send_pkt(KBD_PKT_ACK, 0);
        keyd_last_ack_ms = now;
        off += total;
    }
    if (off) {
        memmove(in, in + off, *inlen - off);
        *inlen -= off;
    }
}

static void keyd_tune(void)
{
    struct sched_param sp;

    mlockall(MCL_CURRENT | MCL_FUTURE);
    setpriority(PRIO_PROCESS, 0, -10);
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = SCHED_PRIO_CLIENT;
    sched_setscheduler(0, SCHED_FIFO, &sp);
}

static int keyd_run(const char *server)
{
    unsigned char inbuf[CLIENT_READ_MAX];
    size_t inlen = 0;
    int backoff = KBD_RECONNECT_MIN_MS;

    keyd_tune();
    keyd_ufd = keyd_uinput_open();
    if (keyd_ufd < 0)
        return 1;

    signal(SIGINT, cli_signal);
    signal(SIGTERM, cli_signal);

    while (!cli_stop) {
        char ip[IP_STR];
        struct pollfd pfd[2];
        int nfds;

        if (resolve_server(server, ip, sizeof(ip)) < 0) {
            usbpi_log("cannot resolve %s", server);
            poll(NULL, 0, backoff);
            backoff = backoff * 2 > KBD_RECONNECT_MAX_MS ?
                      KBD_RECONNECT_MAX_MS : backoff * 2;
            continue;
        }
        keyd_fd = keyd_connect(ip);
        if (keyd_fd < 0) {
            poll(NULL, 0, backoff);
            backoff = backoff * 2 > KBD_RECONNECT_MAX_MS ?
                      KBD_RECONNECT_MAX_MS : backoff * 2;
            continue;
        }
        backoff = KBD_RECONNECT_MIN_MS;
        inlen = 0;
        keyd_last_pkt_ms = 0;
        keyd_last_ack_ms = cli_now_ms();
        usbpi_log("keyboard channel connected to %s:%d", ip, KBD_PORT);
        keyd_send_pkt(KBD_PKT_HELLO, keyd_released ? KBD_FLAG_RELEASED : 0);

        while (!cli_stop && keyd_fd >= 0) {
            int64_t now;

            pfd[0].fd = keyd_fd;
            pfd[0].events = POLLIN;
            pfd[0].revents = 0;
            nfds = 1;
            if (keyd_uinput_readable) {
                pfd[1].fd = keyd_ufd;
                pfd[1].events = POLLIN;
                pfd[1].revents = 0;
                nfds = 2;
            }
            if (poll(pfd, (nfds_t)nfds, KBD_SNAPSHOT_MS) < 0) {
                if (errno == EINTR)
                    continue;
                usbpi_log("keyboard poll: %s", strerror(errno));
                break;
            }
            if (nfds == 2 && (pfd[1].revents & POLLIN))
                keyd_handle_led_read();
            if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                usbpi_log("keyboard connection lost");
                break;
            }
            if (pfd[0].revents & POLLIN) {
                for (;;) {
                    ssize_t n;

                    if (inlen == sizeof(inbuf)) {
                        usbpi_log("keyboard input overflow");
                        keyd_fd = -1;
                        break;
                    }
                    n = read(keyd_fd, inbuf + inlen, sizeof(inbuf) - inlen);
                    if (n > 0) {
                        inlen += (size_t)n;
                        continue;
                    }
                    if (n == 0) {
                        usbpi_log("keyboard server closed the connection");
                        keyd_fd = -1;
                        break;
                    }
                    if (errno == EINTR)
                        continue;
                    if (errno != EAGAIN && errno != EWOULDBLOCK)
                        keyd_fd = -1;
                    break;
                }
                if (keyd_fd >= 0)
                    keyd_parse(inbuf, &inlen);
            }

            now = cli_now_ms();
            if (keyd_any_held() && keyd_last_pkt_ms &&
                now - keyd_last_pkt_ms > keyd_deadman_ms()) {
                usbpi_log("keyboard heartbeat missed, releasing held keys");
                keyd_release_all();
            }
            if (keyd_fd >= 0 && now - keyd_last_ack_ms >= KBD_ACK_MS) {
                keyd_send_pkt(KBD_PKT_ACK, 0);
                keyd_last_ack_ms = now;
            }
        }

        if (keyd_fd >= 0) {
            close(keyd_fd);
            keyd_fd = -1;
        }
        keyd_release_all();
        if (cli_stop)
            break;
        poll(NULL, 0, backoff);
        backoff = backoff * 2 > KBD_RECONNECT_MAX_MS ?
                  KBD_RECONNECT_MAX_MS : backoff * 2;
    }

    keyd_release_all();
    if (keyd_ufd >= 0) {
        ioctl(keyd_ufd, UI_DEV_DESTROY);
        close(keyd_ufd);
        keyd_ufd = -1;
    }
    return 0;
}

static int cmd_keyd(int argc, char **argv)
{
    const char *server = argc > 2 ? argv[2] :
                         (getenv("USBIP_SERVER") ? getenv("USBIP_SERVER") :
                          DEFAULT_SERVER);

    return keyd_run(server);
}
