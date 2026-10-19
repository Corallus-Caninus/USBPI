# USBPI

> ## Turn your Raspberry Pi — **especially the Pi 3B and other older Pis** — into a **USB-to-Ethernet converter.**

Flash one image, plug in your USB devices + Ethernet, power on — and the Pi
becomes a dedicated **USB-over-IP appliance** on your LAN. No desktop, no
monitor, no login: the Pi only forwards USB.

Old hardware is the point. A dusty **Pi 3B** (or any older Pi with Ethernet)
is exactly the right machine for this job: give it a keyboard, mouse, serial
dongle or other USB device on one end, and it appears on any Linux machine on
the network as if it were plugged in locally.

On the client the exported devices show up as ordinary USB devices (mice,
serial adapters, …); keyboards are injected through a low-latency `uinput`
channel. Everything is re-attached automatically whenever the Pi or a device
comes back.

Wireless is not used; Ethernet only. Wi-Fi/Bluetooth stay enabled but idle.

> Status: hobby project, working and in daily use. See [Caveats](#caveats).

---

## Quick start

You need a Raspberry Pi (the **Pi 3B** and other older Pis are ideal — see
[Caveats](#caveats)), a microSD card (or USB drive), Ethernet, and a Linux
machine to connect from. The client-side tooling is built for NixOS, but the
attach daemon works on any Linux with `usbip` and `uinput` available.

**1. Build the image and flash it** — one command. If no device is given you get
a menu of attached drives:

```sh
git clone https://github.com/Corallus-Caninus/USBPI && cd USBPI
./flash.sh /dev/sdX      # or just ./flash.sh to pick from a menu
```

`flash.sh` builds `usbip-pi.img` first if it is missing or out of date. The
build downloads Raspberry Pi OS Lite once, patches it, and needs no root or
loop mounts. Missing tools? It enters the bundled nix shell automatically.

**2. Boot the Pi** — insert the medium, plug in the USB device(s) to export,
plug in Ethernet, and power on. The green ACT LED pulses steadily once Linux is
running (a handy headless "it's alive" indicator).

- **First boot** installs the configuration and reboots (~1–2 min).
- **Second boot** needs internet: it installs the `usbip` package and starts the
  server. After a few minutes the Pi is reachable at **`usbip-pi.local`** and
  exports every attached USB device on **TCP 3240**; keyboards use **TCP 3250**.

No keyboard, monitor or login is ever required for normal operation.

**3. Connect from your machine** — forward every exported device:

```sh
./connect.sh             # keep everything attached (Ctrl-C to stop)
./connect.sh --once      # attach everything once, then exit
```

It builds the C client if needed, self-elevates with sudo, finds the Pi by
`usbip-pi.lan` (DNS), `usbip-pi.local` (mDNS via avahi) or an IP, and keeps
devices attached. Keyboards are received by `usbpi keyd` through `uinput`.

---

## Repository layout

```
flash.sh            build the image (if needed) + write it to a block device
connect.sh          build/launch the C client (keyboard daemon + attach loop)
build.sh            the one build entry point (server/client/native/all/test)
configuration.c     every tunable interval, buffer size and constant (#define)
shared/             wire-format helpers shared by the Pi and the client
client/             single C client (attach/detach/watch/keyd) + NixOS module
server/             flat unity C daemon + systemd units and configuration
scripts/            image build + drive-inspection helpers
shell.nix, flake.nix, usbip.nix   build environment / client usbip tool
```

The C is deliberately flat: no header files and no Makefile. `build.sh` is the
only build system, and `configuration.c` holds nothing but `#define`s.

---

## How it works

```
first boot
  cmdline.txt carries
      systemd.run=/boot/firmware/usbpi/firstrun.sh systemd.run_success_action=reboot
  -> firstrun.sh creates the break-glass user, installs /etc/systemd/system/*
     units + /usr/local/sbin scripts, sets the hostname, then reboots and
     removes the kernel parameter.

second boot
  usbip-provision.service (needs network)
  -> apt-get install usbip
  -> systemctl enable --now usbpi-hid-mirror.service usbipd.service
                              usbipd-device.service

steady state
  usbipd                 listens on 0.0.0.0:3240
  usbpi-hid-mirror       watches kernel uevents, exports non-keyboard devices
                         directly through usbip-host (no helper script), and
                         keeps keyboards on the Pi's usbhid for the fast path
                         on TCP 3250
  usbipd -e              device mode on 3241 for legacy HID gadgets (unused
                         when the keyboard fast path owns keyboards)
```

The daemon deliberately **excludes**:

* the onboard Ethernet (`smsc95xx`, `0424:ec00`) and its hub
  (`0424:9514`/`0424:9512`) — exporting them would kill the network;
* USB hubs / root hubs — not exportable anyway;
* any mass-storage interface (USB class 08) — this protects the USB flash
  drive the Pi boots from;
* anything claimed by the keyboard fast path.

Everything else (serial adapters, mice, gamepads, …) is exported the plain
USB/IP way.

---

## Keyboard fast path (no dropped keys, low latency)

Keyboards are not forwarded as USB devices. Instead:

* the physical keyboard stays on the Pi's own `usbhid`, which polls it at its
  native rate no matter what the network is doing;
* `usbpi-hid-mirror` reads `/dev/input/event*` (evdev, so the kernel already
  parsed HID), numbers every key transition with a sequence number, and sends
  it to the client over TCP 3250 with immediate event packets plus 20 Hz full
  state snapshots;
* the client's `usbpi keyd` creates a real `uinput` keyboard (with `EV_REP`, so
  the OS handles auto-repeat normally) and applies the events;
* every transition is acknowledged and retained in a **4096-event** ring, so a
  reconnect replays exactly what the client missed — once. If the client was
  away longer than the ring, the server sends the current state with a `GAP`
  marker and the middle is lost by design.

Held keys are protected by a dead-man watchdog: if three snapshot intervals
(default 150 ms, auto-raised from measured jitter and capped at 500 ms) pass
without a packet, the client releases everything. A key that is still held is
re-pressed from the next snapshot, with modifiers first so `Shift+A` never
degrades to a lowercase `a`. This deliberately favours "never spam repeats"
over "keep holding through interference": a bad stall may release and re-press
a held key once.

Non-keyboard HID (mice, gamepads, tablets) stays on plain USB/IP. Composite
devices (keyboard plus something else) keep the whole device on `usbhid`; only
the keyboard interface is forwarded, so the other interfaces of that device do
not appear on the client.

Configuration lives in `/etc/usbpi/hid-mirror.conf` on the Pi:

```
enabled = 1
keyboard = 1
queue_limit = 4096
rescan_ms = 2000
# exclude = 046d:c539 1234:5678
```

Set `keyboard = 0` to fall back to the legacy buffered HID gadget. On the
client, `USBIP_KEYBOARD=0 ./connect.sh` disables the keyboard daemon.

Limitations:

* the fast path needs `/dev/uinput` on the client and root (or a udev rule);
* the keyboard channel is LAN-bound but **unauthenticated** — see
  [Security](#security);
* only keyboards get the replay/dead-man treatment;
* the queue lives in RAM: a Pi reboot or daemon restart loses whatever was
  buffered at that moment;
* errors on the physical USB link between device and Pi cannot be recovered by
  any software.

---

## Connecting: options

### One command (recommended)

```sh
./connect.sh                 # all devices, kept attached
./connect.sh --once          # all devices, one pass
USBIP_VIDPID=413c:2113 ./connect.sh   # only this vendor:product
```

### Manual

Find the Pi on the LAN first. Many machines have avahi but not nss-mdns, so
`ping usbip-pi.local` may not resolve — use avahi directly:

```sh
avahi-resolve -n usbip-pi.local          # prints: usbip-pi.local  192.168.1.xx
# or check your router's DHCP leases if that fails
```

The C client self-elevates and resolves `.local` via `avahi-resolve`:

```sh
sudo build/usbpi attach usbip-pi.local            # first exported device
sudo build/usbpi attach usbip-pi.local 413c:2113  # pick by vendor:product
sudo build/usbpi attach 192.168.1.55              # or give an IP directly
sudo build/usbpi detach 413c:2113                 # detach it again
```

`usbpi watch` is the resilient re-attach loop behind `connect.sh` — it
survives the Pi powering off/on, network drops, a changed IP, the device being
replugged, and the client rebooting.

```sh
sudo USBIP_VIDPID=413c:2113 build/usbpi watch usbip-pi.local
```

### Permanent (NixOS module)

In `/etc/nixos/configuration.nix`:

```nix
{
  imports = [ /path/to/USBPI/client/usbip-client.nix ];

  services.usbipClient = {
    enable = true;
    server = "usbip-pi.local";   # default
    # busid = "1-1.3";           # optional, otherwise auto-detected
    # vidPid = "046d:c31c";      # optional filter
    # keyboard = true;           # keyboard fast path (default)
  };
}
```

`sudo nixos-rebuild switch`. The attach and keyboard daemons start at boot and
restart automatically. Check them with
`systemctl status usbip-attach usbip-keyd` /
`journalctl -u usbip-attach -u usbip-keyd -f`.

If you prefer nss-mdns so ordinary programs resolve `.local`, add
`services.avahi.nssmdns = true;` to your NixOS configuration.

---

## Security

* **`usbipd` has no authentication.** Debian builds it against `libwrap`, so
  `server/hosts.allow` limits connections to RFC1918 and link-local ranges;
  everything else is denied by `server/hosts.deny`. The device-mode daemon on
  port 3241 is covered by the same rules. If your LAN uses a
  different range, edit `server/hosts.allow` and reflash. Rebuild with
  `USBIP_LOCKDOWN=0 ./flash.sh /dev/sdX` to export to the whole LAN with no
  restriction (not recommended).
* **The keyboard fast path (TCP 3250) is not authenticated.** It is intended
  for a trusted LAN. It is not covered by `hosts.allow` (that is a TCP-wrapper
  rule for `usbipd` only), so bind it to the LAN interface and add a firewall
  rule, or run it inside WireGuard, if your LAN is not trusted.
* **Break-glass SSH** is enabled (`usbip` / password in `CREDENTIALS.txt`, plus
  your `~/.ssh/*.pub` key). It is only for debugging — normal operation never
  needs it. `CREDENTIALS.txt` is git-ignored; delete it if you don't want it
  lying around (the hash lives only on the Pi).

---

## Troubleshooting

| Symptom | Check |
|---------|-------|
| `usbip-pi.local` doesn't resolve | If the Pi is on another subnet, mDNS won't cross it — use the router's DNS name (e.g. `usbip-pi.lan`) or the Pi's IP. Otherwise `avahi-resolve -n usbip-pi.local`. |
| `usbip ... -r <name>` fails "Unknown Op Common Status" | The name has an IPv6 (AAAA) record that plain `usbip` tries first. Use `./connect.sh` or the C client (it resolves IPv4), or pass the IPv4 address. |
| `usbip list -r` shows nothing | Provisioning may still be running. SSH in and run `journalctl -u usbip-provision -f` (needs internet on first boot). |
| Device missing after replug | The daemon re-exports it on the next uevent. On the client, `systemctl restart usbip-attach` (module) or re-run `connect.sh`. |
| Keyboard repeats or lags | `systemctl status usbpi-hid-mirror` / `journalctl -u usbpi-hid-mirror -f` on the Pi; `journalctl -u usbip-keyd -f` on the client. Check that port 3250 is reachable. |
| Keyboard not appearing on the client | The client needs `/dev/uinput`; run `usbpi keyd` as root and check `lsmod | grep uinput`. |
| Device exported raw instead of using the fast path | It is excluded in `/etc/usbpi/hid-mirror.conf`, or `keyboard = 0`. |
| Did the Pi actually boot the drive? | `./scripts/check-drive.sh /dev/sdX` (read-only) reports whether `firstrun.sh` ran. |
| Server logs | `journalctl -u usbpi-hid-mirror -u usbipd -u usbipd-device`; provisioning log in `/var/log/usbip-provision.log`. |
| Wrong LAN range for restrictions | edit `server/hosts.allow`, rebuild, reflash. |

---

## Caveats

* Target is the **Raspberry Pi 3 Model B**. It can only boot from USB if the
  one-time OTP bit is set, and even then its USB boot is unreliable; booting
  from the microSD card always works and is recommended.
* Isochronous USB devices (webcams, USB audio) do not work well over USB/IP;
  interrupt/bulk devices (mice, serial, storage) are fine.
* Keyboards are re-created as `uinput` devices, so the client sees a synthetic
  keyboard with the USBPI vendor/product rather than the physical device.
* Composite devices with a keyboard keep the whole device on the Pi's `usbhid`;
  only the keyboard interfaces are forwarded.
* Only one client can use an exported USB device at a time — a USB limitation.
  The keyboard fast path supports one client as well.
* The keyboard plugged into the Pi is **not** usable locally on the Pi (by
  design).
* The keyboard channel has no authentication; see [Security](#security).

---

## Building, inspecting, resetting

`flash.sh` runs the image build for you, but you can drive it directly:

```sh
./build.sh client                          # native client (build/usbpi)
./build.sh server                          # armv7 static server
./build.sh native                          # host compile check
./build.sh all                             # server + client
nix-shell --run ./scripts/build-image.sh   # produce usbip-pi.img
USBPI_IMAGE_URL=... ./scripts/build-image.sh   # override the base image
./scripts/check-drive.sh /dev/sdX          # read-only diagnosis of a flashed drive
rm -rf build usbip-pi.img usbip-pi.img.sha256   # force a clean rebuild
```

All tunables — intervals, queue sizes, ports, buffer sizes, scheduler
priorities — are `#define`s in `configuration.c`. Override the armv7 compiler
with `USBIP_ARM_CC` (and `USBIP_ARM_STATIC_LIB` for the static `libc.a`
directory) if you supply your own toolchain.

The image applies automatic latency defaults: the server and client daemons
request real-time scheduling, lock their memory, the server pins itself to the
last CPU and moves the USB/Ethernet IRQs to CPU 0, the CPU governor is set to
`performance`, and USB autosuspend is disabled. No manual tuning is required;
failures fall back silently on kernels that do not expose a knob.

---

## License

[WTFPL](LICENSE) — do what you want.
