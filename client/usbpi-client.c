/*
 * usbpi - the single client binary.
 *
 *   usbpi attach [server] [vid:pid]   attach one exported device
 *   usbpi detach [vid:pid]            detach matching imported devices
 *   usbpi watch  [--once] [server]    keep every exported device attached
 *   usbpi keyd   [server]             keyboard fast-path daemon (uinput)
 *
 * Flat unity build: no headers, no Makefile, and every tunable lives in
 * configuration.c.  Build with ../build.sh client.
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/input.h>
#include <linux/uinput.h>

#include "configuration.c"
#include "shared/usbpi_wire.c"
#include "usbpi_util.c"
#include "usbpi_cli.c"
#include "usbpi_keyd.c"

static void usage(FILE *out)
{
    fprintf(out,
        "usage: usbpi <command> [options]\n"
        "\n"
        "  attach [server] [vid:pid]   attach one exported device\n"
        "  detach [vid:pid]            detach matching imported devices\n"
        "  watch  [--once] [server]    keep every exported device attached\n"
        "  keyd   [server]             keyboard fast-path daemon (uinput)\n"
        "\n"
        "Environment: USBIP_SERVER, USBIP_CANDIDATES, USBIP_VIDPID,\n"
        "             USBIP_INTERVAL, USBIP_GADGET, USBIP_GADGET_PORT,\n"
        "             USBIP_ONCE, USBIP_BIN\n");
}

static void self_elevate(int argc, char **argv)
{
    char **args;
    int i;

    if (geteuid() == 0)
        return;
    args = calloc((size_t)argc + 2, sizeof(*args));
    if (!args) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    args[0] = "sudo";
    for (i = 0; i < argc; i++)
        args[i + 1] = argv[i];
    args[argc + 1] = NULL;
    execvp("sudo", args);
    fprintf(stderr, "usbpi: cannot execute sudo: %s\n", strerror(errno));
    exit(1);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return 1;
    }
    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") ||
        !strcmp(argv[1], "help")) {
        usage(stdout);
        return 0;
    }
    if (!strcmp(argv[1], "attach")) {
        self_elevate(argc, argv);
        return cmd_attach(argc, argv);
    }
    if (!strcmp(argv[1], "detach")) {
        self_elevate(argc, argv);
        return cmd_detach(argc, argv);
    }
    if (!strcmp(argv[1], "watch")) {
        self_elevate(argc, argv);
        return cmd_watch(argc, argv);
    }
    if (!strcmp(argv[1], "keyd")) {
        self_elevate(argc, argv);
        return cmd_keyd(argc, argv);
    }
    usage(stderr);
    return 1;
}
