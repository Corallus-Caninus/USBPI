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

/* Kernel uevents give instant hotplug notification and let the periodic
 * scan skip re-reading attributes for devices that are already active. */
static int open_uevent_socket(void)
{
    int fd;
    struct sockaddr_nl sa;

    fd = socket(AF_NETLINK,
                SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                NETLINK_KOBJECT_UEVENT);
    if (fd < 0)
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = UDEV_UEVENT_GROUP;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void drain_uevents(int fd)
{
    char buf[UEVENT_BUF_MAX];

    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        buf[n] = '\0';
        if (memmem(buf, (size_t)n, "/usb", 4) != NULL ||
            memmem(buf, (size_t)n, "usb", 3) != NULL)
            g_force_full_scan = true;
    }
}

int main(int argc, char **argv)
{
    const char *conf = DEFAULT_CONF;
    int opt, sigfd, uevfd = -1;
    sigset_t mask;
    int64_t next_scan, next_bind;
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
    if (!opt_dry_run) {
        uevfd = open_uevent_socket();
        if (uevfd < 0)
            log_warn("no uevent socket: %s (falling back to polling)",
                     strerror(errno));
    }

    next_scan = now_ms();
    next_bind = now_ms();

    while (!stop_requested) {
        struct signalfd_siginfo si;
        int timeout, i, idx, nfds;
        int64_t loop_now;

        if (now_ms() >= next_scan) {
            bool full = g_force_full_scan;

            scan();
            next_scan = now_ms() + (full ? g_cfg.rescan_ms : SLOW_RESCAN_MS);
        }
        if (!opt_dry_run && now_ms() >= next_bind) {
            bind_pass();
            next_bind = now_ms() + BIND_INTERVAL_MS;
        }
        loop_now = now_ms();
        timeout = (int)(next_scan - loop_now);
        if (!opt_dry_run && next_bind < next_scan &&
            (int)(next_bind - loop_now) < timeout)
            timeout = (int)(next_bind - loop_now);
        if (timeout < 0)
            timeout = 0;

        nfds = (sigfd >= 0) ? 1 : 0;
        if (uevfd >= 0)
            nfds++;
        for (struct device *d = devices; d; d = d->next) {
            if (!d->active)
                continue;
            for (i = 0; i < d->nifaces; i++) {
                struct iface *f = &d->ifaces[i];

                if (f->raw_fd >= 0)
                    nfds++;
                if (f->g_fd >= 0) {
                    if (f->g_retry_at <= loop_now) {
                        nfds++;
                    } else if ((int)(f->g_retry_at - loop_now) < timeout) {
                        /* wake up when the gadget backoff expires */
                        timeout = (int)(f->g_retry_at - loop_now);
                    }
                }
            }
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
        if (uevfd >= 0) {
            fds[idx].fd = uevfd;
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

                if (f->raw_fd >= 0) {
                    fds[idx].fd = f->raw_fd;
                    fds[idx].events = POLLIN;
                    fds[idx].revents = 0;
                    map[idx] = f;
                    idx++;
                }
                if (f->g_fd >= 0 && f->g_retry_at <= loop_now) {
                    fds[idx].fd = f->g_fd;
                    fds[idx].events = POLLOUT;
                    fds[idx].revents = 0;
                    map[idx] = f;
                    idx++;
                }
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
            if (!f) {
                if (fds[i].fd == uevfd) {
                    drain_uevents(uevfd);
                    if (g_force_full_scan)
                        next_scan = now_ms();
                } else if ((fds[i].revents & POLLIN) &&
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

                for (k = 0; k < MAIN_BATCH; k++) {
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
                        char why[BAD_REASON_MAX];

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

                for (k = 0; k < MAIN_BATCH && f->qcount > 0; k++)
                    if (queue_pop_write(f) < 0)
                        break;
            }
            if (f->bad) {
                next_scan = now_ms();
                continue;
            }
            if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                int64_t t = now_ms();

                if (fds[i].events & POLLOUT)
                    f->g_retry_at = t + GADGET_RETRY_MS;
                if (t < next_scan)
                    next_scan = t;
            }
        }
    }

    shutdown_cleanup();
    if (sigfd >= 0)
        close(sigfd);
    if (uevfd >= 0)
        close(uevfd);
    free(fds);
    free(map);
    log_info("stopped");
    return 0;
}
