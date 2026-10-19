#!/usr/bin/env bash
# build.sh - the one build entry point for every USBPI C program.
#
#   ./build.sh server    cross-compile the Pi server daemon (armv7, static)
#   ./build.sh client    build the client binary for this machine
#   ./build.sh native    build the server daemon for this machine (dev check)
#   ./build.sh all       server + client
#   ./build.sh test      native server + client build (compile check)
#
# Environment:
#   USBPI_ARM_CC         armv7 cross compiler
#   USBPI_ARM_STATIC_LIB directory containing the target libc.a
#   USBPI_ARM_STATIC     0 to produce a dynamically linked armv7 binary
#
# There is deliberately no Makefile and no header directory: the server is a
# unity build over flat .c files and configuration.c holds every #define.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$root"

out="$root/build"
mkdir -p "$out"

WARN=(-Wall -Wextra -Wno-format-truncation)
BASE=(-std=gnu11 -O2 -D_FORTIFY_SOURCE=2 -fstack-protector-strong -I "$root")
SERVER_SRC="$root/server/usbpi-hid-mirror.c"
CLIENT_SRC="$root/client/usbpi-client.c"

find_arm_cc() {
    if [ -n "${USBPI_ARM_CC:-}" ]; then
        printf '%s\n' "$USBPI_ARM_CC"
        return 0
    fi
    local c
    for c in armv7l-unknown-linux-gnueabihf-gcc arm-linux-gnueabihf-gcc; do
        if command -v "$c" >/dev/null 2>&1; then
            command -v "$c"
            return 0
        fi
    done
    return 1
}

build_server() {
    local cc
    if ! cc="$(find_arm_cc)"; then
        printf 'build.sh: no armv7 cross compiler found\n' >&2
        printf '  run inside ./flash.sh or nix-shell, or set USBPI_ARM_CC\n' >&2
        exit 1
    fi
    local ld=()
    if [ "${USBPI_ARM_STATIC:-1}" != 0 ]; then
        ld=(-static)
        if [ -n "${USBPI_ARM_STATIC_LIB:-}" ]; then
            ld+=(-L "$USBPI_ARM_STATIC_LIB")
        fi
    fi
    printf '>> server: %s -> %s/usbpi-hid-mirror\n' "$cc" "$out"
    "$cc" "${BASE[@]}" "${WARN[@]}" "${ld[@]}" \
        -o "$out/usbpi-hid-mirror" "$SERVER_SRC"
}

build_client() {
    local cc="${CC:-cc}"
    printf '>> client: %s -> %s/usbpi\n' "$cc" "$out"
    "$cc" "${BASE[@]}" "${WARN[@]}" -o "$out/usbpi" "$CLIENT_SRC"
}

build_native() {
    local cc="${CC:-cc}"
    printf '>> native server: %s -> %s/usbpi-hid-mirror-native\n' "$cc" "$out"
    "$cc" "${BASE[@]}" "${WARN[@]}" \
        -o "$out/usbpi-hid-mirror-native" "$SERVER_SRC"
}

case "${1:-all}" in
    server) build_server ;;
    client) build_client ;;
    native) build_native ;;
    all)    build_server; build_client ;;
    test)   build_native; build_client ;;
    -h|--help)
        sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
        ;;
    *)
        printf 'usage: %s [server|client|native|all|test]\n' "$0" >&2
        exit 1
        ;;
esac
