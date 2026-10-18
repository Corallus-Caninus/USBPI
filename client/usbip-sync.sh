#!/usr/bin/env bash
# Keep the USB devices exported by the USB/IP server attached to this machine.
#
# Default: attach the device selected by USBIP_VIDPID / USBIP_BUSID.
# USBIP_ALL=1: attach *every* device the server exports, and keep them attached.
#
# The server is re-resolved every cycle (DNS name, mDNS/.local via avahi, or a
# literal IP), so it survives the Pi powering off/on, moving networks, changing
# IP, and devices being replugged.
#
# HID devices are exported by the Pi's buffered HID mirror through a second
# daemon in device mode (default port 3241).  Both ports are watched unless
# USBIP_GADGET=0.
#
#   sudo ./usbip-sync.sh [server]
#
# Environment:
#   USBIP_SERVER       server name/IP (default: usbip-pi.lan)
#   USBIP_CANDIDATES   extra fallback names/IPs to try (default: usbip-pi.local)
#   USBIP_ALL          1 = forward every exported device
#   USBIP_VIDPID       pick a single device by vendor:product (e.g. 413c:2113)
#   USBIP_BUSID        pick a single device by a fixed busid
#   USBIP_PORT         main USB/IP port (default 3240)
#   USBIP_GADGET       1 = also attach buffered HID gadgets (default 1)
#   USBIP_GADGET_PORT  device-mode daemon port (default 3241)
#   USBIP_INTERVAL     seconds between checks (default 5)
#   USBIP_ONCE         1 = run a single pass and exit
#
# For boot-time persistence use the NixOS module (client/usbip-client.nix),
# or just run ../connect.sh.
set -u

SERVER_RAW="${1:-${USBIP_SERVER:-usbip-pi.lan}}"
CANDIDATES="${USBIP_CANDIDATES:-usbip-pi.local}"
VIDPID="${USBIP_VIDPID:-}"
BUSID_WANT="${USBIP_BUSID:-}"
ALL="${USBIP_ALL:-}"
INTERVAL="${USBIP_INTERVAL:-5}"
PORT="${USBIP_PORT:-3240}"
GADGET="${USBIP_GADGET:-1}"
GADGET_PORT="${USBIP_GADGET_PORT:-3241}"

# consecutive failed lookups before we treat the server as gone and clear any
# stale local attachment
DOWN_THRESHOLD="${USBIP_DOWN_THRESHOLD:-2}"

# remembers the last IP the server was reachable at, as a fallback when DNS and
# mDNS both fail (the service runs as root, so this path is writable)
LAST_IP_FILE="${USBIP_LAST_IP_FILE:-/var/lib/usbip-sync.last-ip}"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

log() { printf '%s usbip-sync: %s\n' "$(date '+%F %T')" "$*"; }

gadget_enabled() { [ "$GADGET" != "0" ]; }

# --- locate (or build) the usbip binary --------------------------------------
# The build keeps a GC root at <repo>/result so it is not garbage-collected.
if command -v usbip >/dev/null 2>&1; then
    USBIP="$(command -v usbip)"
elif [ -x "$here/../result/bin/usbip" ]; then
    USBIP="$here/../result/bin/usbip"
elif [ -f "$here/../usbip.nix" ] && command -v nix-build >/dev/null 2>&1; then
    log "building usbip from the configured kernel (one time) ..."
    if nix-build --out-link "$here/../result" "$here/../usbip.nix" >/dev/null 2>&1; then
        USBIP="$here/../result/bin/usbip"
    else
        log "FATAL: could not build usbip"
        exit 1
    fi
else
    log "FATAL: usbip not found"
    exit 1
fi

# --- must be root (modprobe + usbip attach) ---------------------------------
if [ "$(id -u)" != "0" ]; then
    log "FATAL: must run as root (try: sudo $0 $SERVER_RAW)"
    exit 1
fi

modprobe vhci-hcd 2>/dev/null || true

# --- server resolution -------------------------------------------------------
resolve_one() {
    local h="$1" ip
    case "$h" in
        *[!0-9.]*) ;;                 # not a plain IPv4 literal
        *) printf '%s\n' "$h"; return 0 ;;
    esac
    case "$h" in
        *.local)
            # mDNS name: only avahi resolves it, and only on the same L2
            if command -v avahi-resolve >/dev/null 2>&1; then
                ip="$(avahi-resolve -n "$h" -4 2>/dev/null | awk '{print $2}')"
                [ -n "$ip" ] && { printf '%s\n' "$ip"; return 0; }
            fi
            return 1
            ;;
    esac
    # DNS name (.lan, ...): prefer IPv4; this host may have no usable IPv6
    if command -v getent >/dev/null 2>&1; then
        ip="$(getent ahostsv4 "$h" 2>/dev/null | awk 'NR==1 { print $1 }')"
        [ -n "$ip" ] && { printf '%s\n' "$ip"; return 0; }
    fi
    return 1
}

resolve_server() {
    local c ip
    for c in "$SERVER_RAW" $CANDIDATES; do
        [ -n "$c" ] || continue
        ip="$(resolve_one "$c")" || continue
        # only accept a candidate that is actually serving usbip
        if port_open "$ip"; then printf '%s\n' "$ip"; return 0; fi
    done
    # last resort: the IP the server was reachable at the previous cycle
    if [ -r "$LAST_IP_FILE" ]; then
        ip="$(cat "$LAST_IP_FILE" 2>/dev/null)"
        if [ -n "$ip" ] && port_open "$ip"; then printf '%s\n' "$ip"; return 0; fi
    fi
    return 1
}

port_open() {
    timeout 2 bash -c "exec 3<>/dev/tcp/$1/$PORT" 2>/dev/null
}

remember_ip() {
    case "$1" in
        *[!0-9.]*) return 0 ;;                # only remember IPv4 literals
    esac
    [ -r "$LAST_IP_FILE" ] && [ "$(cat "$LAST_IP_FILE" 2>/dev/null)" = "$1" ] && return 0
    printf '%s\n' "$1" > "$LAST_IP_FILE" 2>/dev/null || true
}

# --- helpers -----------------------------------------------------------------
all_ports() {
    "$USBIP" port 2>/dev/null | sed -n 's/^Port \([0-9][0-9]*\):.*/\1/p'
}

is_attached() {
    local target="$1" ports
    ports="$("$USBIP" port 2>/dev/null)" || ports=""
    if [ -n "$VIDPID" ]; then
        printf '%s\n' "$ports" | grep -qF "(${VIDPID})"
        return $?
    fi
    printf '%s\n' "$ports" | grep -qF "usbip://$target" \
        || printf '%s\n' "$ports" | grep -qF "usbip://$SERVER_RAW"
}

# Detach imported ports that belong to us (used to clear a stale attachment
# after the server went away).  In ALL mode that means every imported port.
clear_ports() {
    local p ports
    if [ -n "$ALL" ]; then
        ports="$(all_ports)"
    else
        ports="$("$USBIP" port 2>/dev/null | awk -v vp="$VIDPID" -v srv="$SERVER_RAW" '
            /^Port [0-9]+:/ { if (p != "" && hit) print p+0; p=$2; sub(":", "", p); hit=0 }
            vp != "" && index($0, "(" vp ")") { hit=1 }
            vp == "" && index($0, "usbip://" srv) { hit=1 }
            END { if (p != "" && hit) print p+0 }
        ')"
    fi
    for p in $ports; do
        log "detaching stale port $p"
        "$USBIP" detach -p "$p" 2>/dev/null || true
    done
}

# remote listings are human-readable; real devices show busids like "1-1.3",
# buffered HID gadgets like "usbip-vudc.0"
BUSID_RE='[0-9][0-9.-]*\|usbip-vudc\.[0-9]*'

list_remote() { # host port
    "$USBIP" --tcp-port "$2" list -r "$1" 2>/dev/null
}

remote_busids() { # host port
    list_remote "$1" "$2" | sed -n "s/^[[:space:]]*\\(${BUSID_RE}\\):.*/\\1/p"
}

pick_from_listing() { # listing
    local listing="$1"
    if [ -n "$VIDPID" ]; then
        printf '%s\n' "$listing" \
            | sed -n "s/^[[:space:]]*\\(${BUSID_RE}\\):.*(${VIDPID})[[:space:]]*$/\\1/p" | head -n1
    else
        printf '%s\n' "$listing" \
            | sed -n "s/^[[:space:]]*\\(${BUSID_RE}\\):.*/\\1/p" | head -n1
    fi
}

# pick a device to attach, searching the main port first and then the buffered
# HID gadget port; prints "<port> <busid>"
pick_remote() { # host
    local h="$1" listing b
    if [ -n "$BUSID_WANT" ]; then
        case "$BUSID_WANT" in
            usbip-vudc.*) printf '%s %s\n' "$GADGET_PORT" "$BUSID_WANT" ;;
            *)            printf '%s %s\n' "$PORT" "$BUSID_WANT" ;;
        esac
        return 0
    fi
    listing="$(list_remote "$h" "$PORT")" || listing=""
    b="$(pick_from_listing "$listing")"
    if [ -n "$b" ]; then printf '%s %s\n' "$PORT" "$b"; return 0; fi
    if gadget_enabled; then
        listing="$(list_remote "$h" "$GADGET_PORT")" || listing=""
        b="$(pick_from_listing "$listing")"
        if [ -n "$b" ]; then printf '%s %s\n' "$GADGET_PORT" "$b"; return 0; fi
    fi
    return 1
}

attach_one() { # host port busid
    local args=()
    [ "$2" != "3240" ] && args=(--tcp-port "$2")
    "$USBIP" "${args[@]}" attach -r "$1" -b "$3"
}

# attach every device a port currently exports; returns 0 if at least one
# attach succeeded
attach_port() { # host port
    local t="$1" p="$2" b busids found=""
    busids="$(remote_busids "$t" "$p")"
    [ -n "$busids" ] || return 1
    for b in $busids; do
        log "attaching $b from $t:$p"
        if attach_one "$t" "$p" "$b"; then
            log "attached $b from $t:$p"
            found=1
        else
            log "attach failed ($b from $t:$p); will retry"
        fi
    done
    [ -n "$found" ]
}

# --- main loop ---------------------------------------------------------------
if [ -n "$ALL" ]; then
    log "watching $SERVER_RAW (ALL devices, every ${INTERVAL}s)"
else
    log "watching $SERVER_RAW (vid:pid=${VIDPID:-any}, every ${INTERVAL}s)"
fi

state=""
down=0
while true; do
    target="$(resolve_server || true)"

    if [ -z "$target" ]; then
        down=$((down + 1))
        if [ "$down" -eq "$DOWN_THRESHOLD" ]; then
            log "server unreachable - clearing any stale attachment"
            clear_ports
            state="offline"
        fi
    else
        remember_ip "$target"
        if [ "$down" -ge "$DOWN_THRESHOLD" ]; then
            log "server is back"
            state=""
        fi
        down=0

        if [ -n "$ALL" ]; then
            any=""
            attach_port "$target" "$PORT" && any=1
            if gadget_enabled; then
                attach_port "$target" "$GADGET_PORT" && any=1
            fi
            if [ -z "$any" ]; then
                [ "$state" = "idle" ] || log "no devices exported by $target yet"
                state="idle"
            else
                state="attached"
            fi
        elif is_attached "$target"; then
            [ "$state" = "attached" ] || log "attached via $target"
            state="attached"
        else
            if picked="$(pick_remote "$target")"; then
                port="${picked%% *}"
                busid="${picked#* }"
                log "attaching $busid from $target:$port"
                if attach_one "$target" "$port" "$busid"; then
                    log "attached $busid from $target:$port"
                    state="attached"
                else
                    [ "$state" = "attach-failed" ] || log "attach failed; will keep retrying"
                    state="attach-failed"
                fi
            else
                [ "$state" = "no-device" ] || log "$target reachable, requested device not exported yet"
                state="no-device"
            fi
        fi
    fi

    if [ -n "${USBIP_ONCE:-}" ]; then
        exit 0
    fi
    sleep "$INTERVAL"
done
