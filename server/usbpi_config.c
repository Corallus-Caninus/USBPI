/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */

struct config {
    bool enabled;
    bool keyboard;
    unsigned queue_limit;
    int rescan_ms;
    char exclude[MAX_EXCLUDE][16];   /* "vvvv:pppp", lower case */
    int nexc;
};

static struct config g_cfg;

static void parse_exclude(struct config *cfg, char *val)
{
    char *tok;
    char *save = NULL;

    for (tok = strtok_r(val, " ,\t\r\n", &save); tok;
         tok = strtok_r(NULL, " ,\t\r\n", &save)) {
        char entry[16];
        size_t i, len = strlen(tok);

        if (cfg->nexc >= MAX_EXCLUDE) {
            log_warn("exclude list full, ignoring '%s'", tok);
            break;
        }
        if (len != 9 || tok[4] != ':') {
            log_warn("ignoring malformed exclude entry '%s'", tok);
            continue;
        }
        for (i = 0; i < len && i < sizeof(entry) - 1; i++)
            entry[i] = (char)tolower((unsigned char)tok[i]);
        entry[i] = '\0';
        memcpy(cfg->exclude[cfg->nexc], entry, i + 1);
        cfg->nexc++;
    }
}

static void load_config(const char *path, struct config *cfg)
{
    FILE *f;
    char line[CONFIG_LINE_MAX];

    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = true;
    cfg->keyboard = true;
    cfg->queue_limit = DEFAULT_QUEUE;
    cfg->rescan_ms = DEFAULT_RESCAN_MS;

    f = fopen(path, "r");
    if (!f) {
        if (errno != ENOENT)
            log_warn("cannot read %s: %s (using defaults)", path,
                     strerror(errno));
        return;
    }

    while (fgets(line, sizeof(line), f)) {
        char *eq, *key, *val, *p;

        p = strchr(line, '#');
        if (p)
            *p = '\0';
        key = line;
        while (*key == ' ' || *key == '\t')
            key++;
        eq = strchr(key, '=');
        if (!eq)
            continue;
        *eq = '\0';
        val = eq + 1;
        while (*val == ' ' || *val == '\t')
            val++;
        for (p = key; *p; p++)
            *p = (char)tolower((unsigned char)*p);
        p = key + strlen(key);
        while (p > key && (p[-1] == ' ' || p[-1] == '\t'))
            *--p = '\0';

        if (!strcmp(key, "enabled")) {
            cfg->enabled = atoi(val) != 0;
        } else if (!strcmp(key, "keyboard")) {
            cfg->keyboard = atoi(val) != 0;
        } else if (!strcmp(key, "queue_limit")) {
            long v = atol(val);

            if (v >= 16 && v <= 1000000)
                cfg->queue_limit = (unsigned)v;
            else
                log_warn("ignoring queue_limit=%s", val);
        } else if (!strcmp(key, "rescan_ms")) {
            long v = atol(val);

            if (v >= RESCAN_MIN_MS && v <= 60000)
                cfg->rescan_ms = (int)v;
            else
                log_warn("ignoring rescan_ms=%s", val);
        } else if (!strcmp(key, "exclude")) {
            parse_exclude(cfg, val);
        }
    }
    fclose(f);
}

static bool is_excluded(const struct config *cfg, const char *vid,
                        const char *pid)
{
    char vidpid[16];
    int i;

    snprintf(vidpid, sizeof(vidpid), "%.4s:%.4s", vid, pid);
    for (i = 0; i < cfg->nexc; i++)
        if (!strcasecmp(vidpid, cfg->exclude[i]))
            return true;
    return false;
}

