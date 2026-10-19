/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */

static bool opt_verbose;
static bool opt_dry_run;

static void logmsg(const char *level, const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "usbpi-hid-mirror: %s: ", level);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

#define log_info(...)  logmsg("info", __VA_ARGS__)
#define log_warn(...)  logmsg("warn", __VA_ARGS__)
#define log_err(...)   logmsg("err", __VA_ARGS__)
#define log_dbg(...)   do { if (opt_verbose) logmsg("dbg", __VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ */
/* small filesystem helpers                                            */
/* ------------------------------------------------------------------ */

static int read_text(const char *path, char *buf, size_t sz)
{
    int fd;
    ssize_t n;

    if (sz == 0)
        return -1;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    n = read(fd, buf, sz - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    return (int)n;
}

static int read_hex_attr(const char *path, unsigned long *out)
{
    char buf[SYSFS_SMALL_BUF];
    char *end;
    unsigned long v;

    if (read_text(path, buf, sizeof(buf)) < 0)
        return -1;
    errno = 0;
    v = strtoul(buf, &end, 16);
    if (errno != 0 || end == buf)
        return -1;
    *out = v;
    return 0;
}

static int write_text(const char *path, const char *text)
{
    int fd;
    size_t len = strlen(text);
    ssize_t n;

    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    n = write(fd, text, len);
    close(fd);
    if (n < 0 || (size_t)n != len)
        return -1;
    return 0;
}

static int write_bin(const char *path, const void *data, size_t len)
{
    int fd;
    ssize_t n;

    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    n = write(fd, data, len);
    close(fd);
    if (n < 0 || (size_t)n != len)
        return -1;
    return 0;
}

static bool exists(const char *path)
{
    return access(path, F_OK) == 0;
}

static bool is_dir(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int remove_dir(const char *path)
{
    if (rmdir(path) < 0 && errno != ENOENT)
        return -1;
    return 0;
}

static int remove_file(const char *path)
{
    if (unlink(path) < 0 && errno != ENOENT)
        return -1;
    return 0;
}

/* formatted path in a rotating static buffer (never nest two calls) */
static char *gen_path(const char *fmt, ...)
{
    static char bufs[4][PATH_MAX];
    static unsigned idx;
    char *buf = bufs[idx++ % 4];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, PATH_MAX, fmt, ap);
    va_end(ap);
    return buf;
}

