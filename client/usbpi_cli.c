/* ------------------------------------------------------------------ */
/* attach / detach / watch                                             */
/* ------------------------------------------------------------------ */

static int refresh_ports(const char *usbip, struct attached_dev *devs, int max)
{
    char out[CLIENT_CMD_MAX];

    if (usbip_port_cmd(usbip, out, sizeof(out), NULL) < 0)
        return 0;
    return parse_ports(out, devs, max);
}

static bool port_list_has(const struct attached_dev *devs, int n,
                          const char *busid, const char *vidpid)
{
    int i;

    for (i = 0; i < n; i++) {
        if (busid && busid[0] && !strcmp(devs[i].busid, busid))
            return true;
        if (vidpid && vidpid[0] && !strcmp(devs[i].vidpid, vidpid))
            return true;
    }
    return false;
}

static int list_remote(const char *usbip, const char *ip, int port,
                       struct remote_dev *devs, int max)
{
    char out[CLIENT_CMD_MAX];
    int st = -1;

    if (usbip_list_cmd(usbip, ip, port, out, sizeof(out), &st) < 0 || st != 0)
        return 0;
    return parse_remote_listing(out, devs, max);
}

static void collect_attached_vidpids(const char *usbip, char *out, size_t outsz)
{
    char text[CLIENT_CMD_MAX];

    if (usbip_port_cmd(usbip, text, sizeof(text), NULL) < 0) {
        out[0] = '\0';
        return;
    }
    attached_vidpid_string(text, out, outsz);
}

/* Attach every device exported by one port.  Returns true if the port
 * exported at least one device (whether or not it was already attached). */
static bool attach_all_from(const char *usbip, const char *ip, int port,
                            const char *filter, const char *skip)
{
    struct remote_dev rem[32];
    struct attached_dev cur[32];
    char seen[CLIENT_CMD_MAX] = "";
    int nr, nc, i;
    bool exported = false;

    nr = list_remote(usbip, ip, port, rem, 32);
    if (nr <= 0)
        return false;
    nc = refresh_ports(usbip, cur, 32);

    for (i = 0; i < nr; i++) {
        const char *vp = rem[i].vidpid;

        exported = true;
        if (filter && filter[0] && strcmp(filter, vp))
            continue;
        if (port_list_has(cur, nc, rem[i].busid, vp))
            continue;
        if (vidpid_in_list(skip, vp) || vidpid_in_list(seen, vp))
            continue;
        usbpi_log("attaching %s from %s:%d", rem[i].busid, ip, port);
        if (usbip_attach_cmd(usbip, ip, port, rem[i].busid) == 0) {
            usbpi_log("attached %s from %s:%d", rem[i].busid, ip, port);
            {
                size_t used = strlen(seen);
                snprintf(seen + used, sizeof(seen) - used, "%s%s",
                         used ? " " : "", vp);
            }
        } else {
            usbpi_log("attach failed (%s from %s:%d); will retry",
                 rem[i].busid, ip, port);
        }
    }
    return exported;
}

static void detach_matching(const char *usbip, const char *filter)
{
    struct attached_dev cur[32];
    int n = refresh_ports(usbip, cur, 32);
    int i;

    for (i = 0; i < n; i++) {
        if (filter && filter[0] && strcmp(filter, cur[i].vidpid))
            continue;
        usbpi_log("detaching port %d", cur[i].port);
        usbip_detach_cmd(usbip, cur[i].port);
    }
}

static int cmd_attach(int argc, char **argv)
{
    const char *server = argc > 2 ? argv[2] :
                         (getenv("USBIP_SERVER") ? getenv("USBIP_SERVER") :
                          DEFAULT_SERVER);
    const char *filter = argc > 3 ? argv[3] :
                         (getenv("USBIP_VIDPID") ? getenv("USBIP_VIDPID") : "");
    const char *gadget_s = getenv("USBIP_GADGET");
    bool gadget = !gadget_s || strcmp(gadget_s, "0");
    int gport = getenv("USBIP_GADGET_PORT") ? atoi(getenv("USBIP_GADGET_PORT"))
                                            : GADGET_PORT;
    char ip[64], usbip[PATH_MAX];
    struct remote_dev rem[32];
    struct attached_dev cur[32];
    int n, i, chosen_port = USBIP_PORT;
    const char *chosen = NULL;

    if (!find_usbip(usbip, sizeof(usbip))) {
        usbpi_log("usbip not found");
        return 1;
    }
    if (resolve_server(server, ip, sizeof(ip)) < 0) {
        usbpi_log("cannot resolve %s", server);
        return 1;
    }
    n = refresh_ports(usbip, cur, 32);
    if (port_list_has(cur, n, NULL, filter[0] ? filter : NULL)) {
        usbpi_log("already attached%s%s", filter[0] ? " " : "", filter);
        return 0;
    }
    n = list_remote(usbip, ip, USBIP_PORT, rem, 32);
    for (i = 0; i < n; i++) {
        if (!filter[0] || !strcmp(filter, rem[i].vidpid)) {
            chosen = rem[i].busid;
            break;
        }
    }
    if (!chosen && gadget) {
        n = list_remote(usbip, ip, gport, rem, 32);
        for (i = 0; i < n; i++) {
            if (!filter[0] || !strcmp(filter, rem[i].vidpid)) {
                chosen = rem[i].busid;
                chosen_port = gport;
                break;
            }
        }
    }
    if (!chosen) {
        usbpi_log("%s exports no matching device", server);
        return 1;
    }
    usbpi_log("attaching %s from %s:%d", chosen, ip, chosen_port);
    if (usbip_attach_cmd(usbip, ip, chosen_port, chosen) != 0) {
        usbpi_log("attach failed");
        return 1;
    }
    return 0;
}

static int cmd_detach(int argc, char **argv)
{
    const char *filter = argc > 2 ? argv[2] :
                         (getenv("USBIP_VIDPID") ? getenv("USBIP_VIDPID") : "");
    char usbip[PATH_MAX];

    if (!find_usbip(usbip, sizeof(usbip))) {
        usbpi_log("usbip not found");
        return 1;
    }
    detach_matching(usbip, filter[0] ? filter : NULL);
    return 0;
}

static int cmd_watch(int argc, char **argv)
{
    const char *server = getenv("USBIP_SERVER") ? getenv("USBIP_SERVER")
                                                : DEFAULT_SERVER;
    const char *filter_env = getenv("USBIP_VIDPID");
    if (!filter_env)
        filter_env = "";
    const char *gadget_s = getenv("USBIP_GADGET");
    bool gadget = !gadget_s || strcmp(gadget_s, "0");
    int gport = getenv("USBIP_GADGET_PORT") ? atoi(getenv("USBIP_GADGET_PORT"))
                                            : GADGET_PORT;
    int interval = getenv("USBIP_INTERVAL") ? atoi(getenv("USBIP_INTERVAL"))
                                            : DEFAULT_INTERVAL;
    bool once = getenv("USBIP_ONCE") != NULL;
    const char *all_env = getenv("USBIP_ALL");
    bool all;
    int down = 0;
    char last_state[32] = "";
    char usbip[PATH_MAX];
    int i;

    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--once"))
            once = true;
        else
            server = argv[i];
    }
    if (!find_usbip(usbip, sizeof(usbip))) {
        usbpi_log("usbip not found");
        return 1;
    }
    if (all_env)
        all = atoi(all_env) != 0;
    else
        all = !(filter_env && filter_env[0]);

    if (all)
        usbpi_log("watching %s (all devices, every %ds)", server, interval);
    else
        usbpi_log("watching %s (vid:pid=%s, every %ds)", server, filter_env,
             interval);

    signal(SIGINT, cli_signal);
    signal(SIGTERM, cli_signal);

    while (!cli_stop) {
        char ip[64];
        bool ok;

        ok = resolve_server(server, ip, sizeof(ip)) == 0 &&
             port_open(ip, USBIP_PORT, PORT_PROBE_TIMEOUT_MS);
        if (!ok && read_last_ip(ip, sizeof(ip)))
            ok = port_open(ip, USBIP_PORT, PORT_PROBE_TIMEOUT_MS);

        if (!ok) {
            down++;
            if (down == DOWN_THRESHOLD) {
                usbpi_log("server unreachable - clearing stale attachments");
                detach_matching(usbip, all ? NULL : filter_env);
                snprintf(last_state, sizeof(last_state), "offline");
            }
        } else {
            bool any = false;

            remember_ip(ip);
            if (down >= DOWN_THRESHOLD) {
                usbpi_log("server is back");
                last_state[0] = '\0';
            }
            down = 0;

            if (all) {
                char skip[CLIENT_CMD_MAX] = "";

                if (gadget) {
                    if (attach_all_from(usbip, ip, gport, NULL, NULL))
                        any = true;
                    collect_attached_vidpids(usbip, skip, sizeof(skip));
                }
                if (attach_all_from(usbip, ip, USBIP_PORT, NULL, skip))
                    any = true;
                if (!any) {
                    if (strcmp(last_state, "idle"))
                        usbpi_log("no devices exported by %s yet", ip);
                    snprintf(last_state, sizeof(last_state), "idle");
                } else {
                    snprintf(last_state, sizeof(last_state), "attached");
                }
            } else {
                struct attached_dev cur[32];
                int n = refresh_ports(usbip, cur, 32);

                if (filter_env && filter_env[0]
                        ? port_list_has(cur, n, NULL, filter_env)
                        : n > 0) {
                    if (strcmp(last_state, "attached"))
                        usbpi_log("attached via %s", ip);
                    snprintf(last_state, sizeof(last_state), "attached");
                } else {
                    struct remote_dev rem[32];
                    const char *chosen = NULL;
                    int port = USBIP_PORT;

                    n = list_remote(usbip, ip, USBIP_PORT, rem, 32);
                    for (i = 0; i < n; i++) {
                        if (!filter_env[0] ||
                            !strcmp(filter_env, rem[i].vidpid)) {
                            chosen = rem[i].busid;
                            break;
                        }
                    }
                    if (!chosen && gadget) {
                        n = list_remote(usbip, ip, gport, rem, 32);
                        for (i = 0; i < n; i++) {
                            if (!filter_env[0] ||
                                !strcmp(filter_env, rem[i].vidpid)) {
                                chosen = rem[i].busid;
                                port = gport;
                                break;
                            }
                        }
                    }
                    if (chosen) {
                        usbpi_log("attaching %s from %s:%d", chosen, ip, port);
                        if (usbip_attach_cmd(usbip, ip, port, chosen) == 0)
                            snprintf(last_state, sizeof(last_state), "attached");
                    } else {
                        if (strcmp(last_state, "no-device"))
                            usbpi_log("%s reachable, requested device not exported",
                                 ip);
                        snprintf(last_state, sizeof(last_state), "no-device");
                    }
                }
            }
        }

        if (once)
            break;
        sleep((unsigned)interval);
    }
    return 0;
}
