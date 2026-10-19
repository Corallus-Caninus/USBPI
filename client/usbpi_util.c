/* ------------------------------------------------------------------ */
/* client utilities: logging, process capture, resolution, parsing     */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t cli_stop;

static int64_t cli_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void usbpi_log(const char *fmt, ...)
{
    struct tm tm;
    time_t t = time(NULL);
    va_list ap;

    localtime_r(&t, &tm);
    fprintf(stderr, "%04d-%02d-%02d %02d:%02d:%02d usbpi: ",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void cli_signal(int sig)
{
    (void)sig;
    cli_stop = 1;
}

static bool is_ipv4(const char *s)
{
    struct in_addr a;

    return inet_pton(AF_INET, s, &a) == 1;
}

static bool path_executable(const char *p)
{
    return p && access(p, X_OK) == 0;
}

static bool find_in_path(const char *name, char *out, size_t outsz)
{
    const char *path = getenv("PATH");
    char *copy, *dir, *save;

    if (!path)
        return false;
    copy = strdup(path);
    if (!copy)
        return false;
    for (dir = strtok_r(copy, ":", &save); dir;
         dir = strtok_r(NULL, ":", &save)) {
        snprintf(out, outsz, "%s/%s", dir, name);
        if (path_executable(out)) {
            free(copy);
            return true;
        }
    }
    free(copy);
    return false;
}

/* Read up to outsz-1 bytes of a child's stdout, discarding the rest. */
static int run_capture(char *const argv[], char *out, size_t outsz, int *status)
{
    int pfd[2];
    pid_t pid;
    size_t used = 0;
    int st = 0;

    if (pipe(pfd) < 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    while (used + 1 < outsz) {
        ssize_t n = read(pfd[0], out + used, outsz - 1 - used);

        if (n <= 0)
            break;
        used += (size_t)n;
    }
    if (used + 1 >= outsz) {
        char tmp[TMP_BUF];

        while (read(pfd[0], tmp, sizeof(tmp)) > 0)
            ;
    }
    out[used] = '\0';
    close(pfd[0]);
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    if (status)
        *status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return 0;
}

static int run_wait(char *const argv[])
{
    pid_t pid = fork();
    int st;

    if (pid < 0)
        return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void repo_root(char *out, size_t outsz)
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    char *slash;

    if (n < 0) {
        snprintf(out, outsz, ".");
        return;
    }
    exe[n] = '\0';
    slash = strrchr(exe, '/');
    if (slash)
        *slash = '\0';                   /* build/ */
    slash = strrchr(exe, '/');
    if (slash)
        *slash = '\0';                   /* repo root */
    snprintf(out, outsz, "%s", exe);
}

static bool find_usbip(char *out, size_t outsz)
{
    const char *env = getenv("USBIP_BIN");
    char root[PATH_MAX], cand[PATH_MAX];

    if (path_executable(env)) {
        snprintf(out, outsz, "%s", env);
        return true;
    }
    if (find_in_path("usbip", out, outsz))
        return true;

    repo_root(root, sizeof(root));
    snprintf(cand, sizeof(cand), "%s/result/bin/usbip", root);
    if (path_executable(cand)) {
        snprintf(out, outsz, "%s", cand);
        return true;
    }

    if (find_in_path("nix-build", cand, sizeof(cand))) {
        char outlink[PATH_MAX], expr[PATH_MAX];
        char *argv[6];

        snprintf(outlink, sizeof(outlink), "%s/result", root);
        snprintf(expr, sizeof(expr), "%s/usbip.nix", root);
        argv[0] = cand;
        argv[1] = "--out-link";
        argv[2] = outlink;
        argv[3] = expr;
        argv[4] = NULL;
        usbpi_log("building usbip from the configured kernel (one time)");
        if (run_wait(argv) == 0) {
            snprintf(cand, sizeof(cand), "%s/result/bin/usbip", root);
            if (path_executable(cand)) {
                snprintf(out, outsz, "%s", cand);
                return true;
            }
        }
    }
    return false;
}

static int resolve_server(const char *server, char *out, size_t outsz)
{
    struct addrinfo hints, *res = NULL;

    if (is_ipv4(server)) {
        snprintf(out, outsz, "%s", server);
        return 0;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(server, NULL, &hints, &res) == 0) {
        struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;

        inet_ntop(AF_INET, &sin->sin_addr, out, (socklen_t)outsz);
        freeaddrinfo(res);
        return 0;
    }
    if (strstr(server, ".local")) {
        char buf[4096], *p, *save;
        char *argv[5];
        int st = -1;

        argv[0] = "avahi-resolve";
        argv[1] = "-n";
        argv[2] = (char *)server;
        argv[3] = "-4";
        argv[4] = NULL;
        if (run_capture(argv, buf, sizeof(buf), &st) == 0 && st == 0) {
            for (p = strtok_r(buf, " \t\r\n", &save); p;
                 p = strtok_r(NULL, " \t\r\n", &save)) {
                if (is_ipv4(p)) {
                    snprintf(out, outsz, "%s", p);
                    return 0;
                }
            }
        }
    }
    return -1;
}

static bool port_open(const char *ip, int port, int timeout_ms)
{
    struct sockaddr_in sa;
    struct pollfd pfd;
    int fd, err = 0;
    socklen_t elen = sizeof(err);

    fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) {
        close(fd);
        return false;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 &&
        errno != EINPROGRESS) {
        close(fd);
        return false;
    }
    pfd.fd = fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    if (poll(&pfd, 1, timeout_ms) <= 0) {
        close(fd);
        return false;
    }
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 ||
        err != 0) {
        close(fd);
        return false;
    }
    close(fd);
    return true;
}

static void remember_ip(const char *ip)
{
    FILE *f;

    if (!is_ipv4(ip))
        return;
    f = fopen(LAST_IP_FILE, "w");
    if (!f)
        return;
    fprintf(f, "%s\n", ip);
    fclose(f);
}

static bool read_last_ip(char *out, size_t outsz)
{
    FILE *f = fopen(LAST_IP_FILE, "r");
    char line[IP_STR];

    if (!f)
        return false;
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return false;
    }
    fclose(f);
    line[strcspn(line, "\r\n")] = '\0';
    if (!is_ipv4(line))
        return false;
    snprintf(out, outsz, "%s", line);
    return true;
}

struct remote_dev {
    char busid[NAME_MAX_LEN];
    char vidpid[VIDPID_LEN];
};

static bool valid_busid(const char *s, size_t len)
{
    size_t i;

    if (len == 0 || len >= NAME_MAX_LEN)
        return false;
    if (!strncmp(s, "usbip-vudc.", 11))
        return true;
    if (!isdigit((unsigned char)s[0]))
        return false;
    for (i = 0; i < len; i++)
        if (!isdigit((unsigned char)s[i]) && s[i] != '-' && s[i] != '.')
            return false;
    return true;
}

static int parse_remote_listing(const char *listing, struct remote_dev *out,
                                int max)
{
    char *copy = strdup(listing ? listing : "");
    char *line, *save;
    int n = 0;

    if (!copy)
        return 0;
    for (line = strtok_r(copy, "\n", &save); line && n < max;
         line = strtok_r(NULL, "\n", &save)) {
        char *p = line, *colon, *lp;
        size_t len;
        unsigned vid, pid;

        while (*p == ' ' || *p == '\t')
            p++;
        if (!strncmp(p, "- busid ", 8)) {
            char *sp;

            p += 8;
            sp = strpbrk(p, " \t");
            len = sp ? (size_t)(sp - p) : strlen(p);
        } else {
            colon = strchr(p, ':');
            if (!colon)
                continue;
            len = (size_t)(colon - p);
            while (len && (p[len - 1] == ' ' || p[len - 1] == '\t'))
                len--;
        }
        if (!valid_busid(p, len))
            continue;
        lp = strrchr(line, '(');
        if (!lp || sscanf(lp, "(%x:%x)", &vid, &pid) != 2)
            continue;
        memcpy(out[n].busid, p, len);
        out[n].busid[len] = '\0';
        snprintf(out[n].vidpid, sizeof(out[n].vidpid), "%04x:%04x", vid, pid);
        n++;
    }
    free(copy);
    return n;
}

struct attached_dev {
    int port;
    char busid[NAME_MAX_LEN];
    char vidpid[VIDPID_LEN];
};

static int parse_ports(const char *text, struct attached_dev *out, int max)
{
    char *copy = strdup(text ? text : "");
    char *line, *save;
    int n = 0, cur = -1;

    if (!copy)
        return 0;
    for (line = strtok_r(copy, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        int p;
        char *lp, *ub, *q = line;
        unsigned vid, pid;

        while (*q == ' ' || *q == '\t')
            q++;
        if (sscanf(q, "Port %d:", &p) == 1) {
            cur = -1;
            if (n < max) {
                cur = n;
                memset(&out[n], 0, sizeof(out[n]));
                out[n].port = p;
                n++;
            }
            continue;
        }
        if (cur < 0)
            continue;
        lp = strrchr(line, '(');
        if (lp && sscanf(lp, "(%x:%x)", &vid, &pid) == 2)
            snprintf(out[cur].vidpid, sizeof(out[cur].vidpid),
                     "%04x:%04x", vid, pid);
        ub = strstr(line, "usbip://");
        if (ub) {
            char *slash = strrchr(ub, '/');

            if (slash) {
                char *end = slash + 1;
                size_t len = strcspn(end, " \t\r\n");

                if (valid_busid(end, len)) {
                    memcpy(out[cur].busid, end, len);
                    out[cur].busid[len] = '\0';
                }
            }
        }
    }
    free(copy);
    return n;
}

static void attached_vidpid_string(const char *text, char *out, size_t outsz)
{
    struct attached_dev devs[DEVS_MAX];
    int n = parse_ports(text, devs, 32);
    int i;
    size_t used = 0;

    out[0] = '\0';
    for (i = 0; i < n; i++) {
        size_t len;

        if (!devs[i].vidpid[0])
            continue;
        len = strlen(devs[i].vidpid);
        if (used + len + 2 >= outsz)
            break;
        if (used)
            out[used++] = ' ';
        memcpy(out + used, devs[i].vidpid, len);
        used += len;
        out[used] = '\0';
    }
}

static bool vidpid_in_list(const char *list, const char *vidpid)
{
    char needle[NEEDLE_STR];

    if (!vidpid[0] || !list[0])
        return false;
    snprintf(needle, sizeof(needle), " %s ", vidpid);
    {
        char haystack[CLIENT_CMD_MAX];
        snprintf(haystack, sizeof(haystack), " %s ", list);
        return strstr(haystack, needle) != NULL;
    }
}

static int usbip_port_cmd(const char *usbip, char *out, size_t outsz,
                          int *status)
{
    char *argv[3];

    argv[0] = (char *)usbip;
    argv[1] = "port";
    argv[2] = NULL;
    return run_capture(argv, out, outsz, status);
}

static int usbip_list_cmd(const char *usbip, const char *ip, int port,
                          char *out, size_t outsz, int *status)
{
    char portbuf[PORT_STR];
    char *argv[8];
    int i = 0;

    argv[i++] = (char *)usbip;
    if (port != USBIP_PORT) {
        snprintf(portbuf, sizeof(portbuf), "%d", port);
        argv[i++] = "--tcp-port";
        argv[i++] = portbuf;
    }
    argv[i++] = "list";
    argv[i++] = "-r";
    argv[i++] = (char *)ip;
    argv[i] = NULL;
    return run_capture(argv, out, outsz, status);
}

static int usbip_attach_cmd(const char *usbip, const char *ip, int port,
                            const char *busid)
{
    char portbuf[PORT_STR];
    char *argv[9];
    int i = 0;

    argv[i++] = (char *)usbip;
    if (port != USBIP_PORT) {
        snprintf(portbuf, sizeof(portbuf), "%d", port);
        argv[i++] = "--tcp-port";
        argv[i++] = portbuf;
    }
    argv[i++] = "attach";
    argv[i++] = "-r";
    argv[i++] = (char *)ip;
    argv[i++] = "-b";
    argv[i++] = (char *)busid;
    argv[i] = NULL;
    return run_wait(argv);
}

static int usbip_detach_cmd(const char *usbip, int port)
{
    char portbuf[PORT_STR];
    char *argv[5];

    snprintf(portbuf, sizeof(portbuf), "%d", port);
    argv[0] = (char *)usbip;
    argv[1] = "detach";
    argv[2] = "-p";
    argv[3] = portbuf;
    argv[4] = NULL;
    return run_wait(argv);
}
