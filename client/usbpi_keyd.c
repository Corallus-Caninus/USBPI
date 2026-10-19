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
static int64_t keyd_last_ack_ms;
static int64_t keyd_deadman_value_ms = KBD_DEADMAN_MIN_MS;
static unsigned int keyd_repeat_delay_ms = KBD_REPEAT_DELAY_MS;

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
    keyd_released = false;
}

static int64_t keyd_deadman_ms(void)
{
    return keyd_deadman_value_ms;
}

/* Locate the evdev node the kernel created for our uinput keyboard. */
static int keyd_find_own_event(char *out, size_t outsz)
{
    glob_t g;
    int found = -1;
    size_t i;

    if (glob("/dev/input/event*", 0, NULL, &g) != 0)
        return -1;
    for (i = 0; i < g.gl_pathc; i++) {
        char name[UINPUT_MAX_NAME_SIZE];
        struct input_id id;
        int fd = open(g.gl_pathv[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);

        if (fd < 0)
            continue;
        memset(name, 0, sizeof(name));
        memset(&id, 0, sizeof(id));
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 &&
            ioctl(fd, EVIOCGID, &id) >= 0 &&
            !strcmp(name, UINPUT_NAME) &&
            id.vendor == UINPUT_VENDOR && id.product == UINPUT_PRODUCT) {
            snprintf(out, outsz, "%s", g.gl_pathv[i]);
            found = 0;
            close(fd);
            break;
        }
        close(fd);
    }
    globfree(&g);
    return found;
}

/* The dead-man must fire before the kernel emits its first autorepeat.  Read
 * the virtual keyboard's repeat delay; if it is shorter than a sane minimum,
 * raise only this device's delay.  The dead-man is then the shorter of three
 * missed snapshots and the repeat delay minus a safety margin. */
static void keyd_configure_repeat(void)
{
    char path[PATH_MAX];
    unsigned int rep[2] = { KBD_REPEAT_DELAY_MS, KBD_REPEAT_PERIOD_MS };
    int tries, fd;
    int wait_loops = KBD_EVENT_WAIT_MS / 10;

    for (tries = 0; tries < wait_loops; tries++) {
        if (keyd_find_own_event(path, sizeof(path)) == 0)
            break;
        usleep(10 * 1000);
    }
    if (tries == wait_loops) {
        usbpi_log("uinput event node not found, assuming %d ms repeat delay",
                  KBD_REPEAT_DELAY_MS);
    } else {
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            unsigned int cur[2] = { 0, 0 };

            if (ioctl(fd, EVIOCGREP, cur) == 0 && cur[0] > 0) {
                if (cur[0] >= KBD_REPEAT_DELAY_MS) {
                    rep[0] = cur[0];
                    if (cur[1] > 0)
                        rep[1] = cur[1];
                } else if (ioctl(fd, EVIOCSREP, rep) < 0) {
                    /* Could not raise it: respect the shorter actual delay so
                     * the dead-man still fires before autorepeat. */
                    usbpi_log("cannot raise repeat delay: %s",
                              strerror(errno));
                    rep[0] = cur[0];
                    if (cur[1] > 0)
                        rep[1] = cur[1];
                }
            } else if (ioctl(fd, EVIOCSREP, rep) < 0) {
                usbpi_log("cannot set repeat delay: %s", strerror(errno));
            }
            close(fd);
        }
    }

    keyd_repeat_delay_ms = rep[0];
    {
        int64_t base = (int64_t)KBD_SNAPSHOT_MS * KBD_DEADMAN_MISSES;
        int64_t bound = (int64_t)keyd_repeat_delay_ms -
                        KBD_DEADMAN_MARGIN_MS;

        if (bound < KBD_DEADMAN_MIN_MS)
            bound = (int64_t)keyd_repeat_delay_ms / 2;
        if (bound < 20)
            bound = 20;
        keyd_deadman_value_ms = base < bound ? base : bound;
    }
    usbpi_log("keyboard repeat delay %u ms, dead-man %lld ms",
              keyd_repeat_delay_ms, (long long)keyd_deadman_value_ms);
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
        keyd_last_pkt_ms = now;
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
    keyd_configure_repeat();

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
            int64_t now = cli_now_ms();
            int timeout = KBD_SNAPSHOT_MS;

            if (keyd_any_held() && keyd_last_pkt_ms) {
                int64_t remain = keyd_deadman_ms() -
                                 (now - keyd_last_pkt_ms);

                if (remain < timeout)
                    timeout = remain > 0 ? (int)remain : 0;
            }

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
            if (poll(pfd, (nfds_t)nfds, timeout) < 0) {
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
                now - keyd_last_pkt_ms >= keyd_deadman_ms()) {
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
