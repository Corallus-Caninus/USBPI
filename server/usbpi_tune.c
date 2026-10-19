/* ------------------------------------------------------------------ */
/* automatic scheduling / latency tuning (no user configuration)       */
/* ------------------------------------------------------------------ */

static void tune_governor(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    long i;

    for (i = 0; i < n; i++) {
        char path[PBUF];

        snprintf(path, sizeof(path),
                 CPU_GOVERNOR_DIR "/cpu%ld/cpufreq/scaling_governor", i);
        write_text(path, TUNE_GOVERNOR);
    }
}

static void tune_irq_affinity(void)
{
    FILE *f = fopen("/proc/interrupts", "r");
    char line[TUNE_LINE_MAX];
    char mask[CPU_MASK_STR];
    unsigned long irq;
    char *end;

    if (!f)
        return;
    snprintf(mask, sizeof(mask), "%x", 1u << TUNE_IRQ_CORE);
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, "dwc_otg") && !strstr(line, "smsc95xx"))
            continue;
        irq = strtoul(line, &end, 10);
        if (end == line)
            continue;
        {
            char path[PBUF];

            snprintf(path, sizeof(path), "/proc/irq/%lu/smp_affinity", irq);
            write_text(path, mask);
        }
    }
    fclose(f);
}

/* Run the daemons at a stable priority on their own core, keep the USB and
 * Ethernet IRQs on another core, and stop the CPU from dropping to a low
 * frequency or autosuspending the keyboard.  Failures are harmless: the
 * appliance still works on kernels/userspace that do not expose these knobs. */
static void tune_auto(void)
{
    struct sched_param sp;
    long n;

    mlockall(MCL_CURRENT | MCL_FUTURE);
    setpriority(PRIO_PROCESS, 0, -10);

    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = SCHED_PRIO_SERVER;
    sched_setscheduler(0, SCHED_FIFO, &sp);

    n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > TUNE_DAEMON_CORE_SLOT) {
        cpu_set_t set;

        CPU_ZERO(&set);
        CPU_SET(TUNE_DAEMON_CORE_SLOT, &set);
        sched_setaffinity(0, sizeof(set), &set);
    }

    write_text(USB_AUTOSUSPEND, "-1");
    tune_governor();
    tune_irq_affinity();
}
