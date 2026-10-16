# USBPI

> ## Turn your Raspberry Pi — **especially the Pi 3B and other older Pis** — into a **USB-to-Ethernet converter.**

Flash one image, plug in your USB devices + Ethernet, power on — and the Pi
becomes a dedicated **USB-over-IP appliance** on your LAN. No desktop, no
monitor, no login: the Pi only forwards USB.

Old hardware is the point. A dusty **Pi 3B** (or any older Pi with Ethernet)
is exactly the right machine for this job: give it a keyboard, mouse, serial
dongle or other USB device on one end, and it appears on any Linux machine on
the network as if it were plugged in locally.

On the client the exported devices show up as ordinary USB devices (keyboards,
mice, serial adapters, …) and are re-attached automatically whenever the Pi or
a device comes back.

Wireless is not used; Ethernet only. Wi-Fi/Bluetooth stay enabled but idle.

> Status: hobby project, working and in daily use. See [Caveats](#caveats).

---

## Quick start

You need a Raspberry Pi (the **Pi 3B** and other older Pis are ideal — see
[Caveats](#caveats)), a microSD card (or USB drive), Ethernet, and a Linux
machine to connect from. The client-side tooling is built for NixOS, but the
attach scripts work on any Linux with `usbip` available.

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
  exports every attached USB device on **TCP 3240**.

No keyboard, monitor or login is ever required for normal operation.

**3. Connect from your machine** — forward every exported device:

```sh
./connect.sh             # keep everything attached (Ctrl-C to stop)
./connect.sh --once      # attach everything once, then exit
```

It self-elevates with sudo, finds the Pi by `usbip-pi.lan` (DNS),
`usbip-pi.local` (mDNS via avahi) or an IP, builds the matching `usbip` tool if
needed, and re-attaches whenever the Pi or a device comes back.

---

## Repository layout

```
flash.sh            build the image (if needed) + write it to a block device
connect.sh          attach every device the Pi exports to this machine
client/             attach/detach/sync scripts + a NixOS module
server/             the configuration baked into the image
scripts/            low-level build + drive-inspection helpers
shell.nix, flake.nix, usbip.nix   build environment / client tool
```

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
  -> systemctl enable --now usbipd.service usbip-autobind.timer

steady state
  usbipd                 listens on 0.0.0.0:3240
  usbip-autobind.timer   every 5s runs usbip-bind-all.sh, which exports every
                         local USB device not yet bound to usbip-host
```

`usbip-bind-all.sh` deliberately **excludes**:

* the onboard Ethernet (`smsc95xx`, `0424:ec00`) and its hub
  (`0424:9514`/`0424:9512`) — exporting them would kill the network;
* USB hubs / root hubs — not exportable anyway;
* any mass-storage interface (USB class 08) — this protects the USB flash
  drive the Pi boots from.

Everything else (keyboards, mice, serial adapters, …) is exported.

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

The scripts also auto-resolve `.local` via `avahi-resolve`, so the hostname
works even without nss-mdns:

```sh
./client/usbip-attach.sh usbip-pi.local            # first exported device
./client/usbip-attach.sh usbip-pi.local 413c:2113  # pick by vendor:product
./client/usbip-attach.sh 192.168.1.55              # or give an IP directly
./client/usbip-detach.sh 413c:2113                 # detach it again
```

`client/usbip-sync.sh` is the resilient re-attach loop behind `connect.sh` — it
survives the Pi powering off/on, network drops, a changed IP, the device being
replugged, and the client rebooting.

```sh
sudo USBIP_VIDPID=413c:2113 ./client/usbip-sync.sh usbip-pi.local
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
  };
}
```

`sudo nixos-rebuild switch`. The device is attached at boot and re-attached
automatically if it drops. Check it with
`systemctl status usbip-attach` / `journalctl -u usbip-attach -f`.

If you prefer nss-mdns so ordinary programs resolve `.local`, add
`services.avahi.nssmdns = true;` to your NixOS configuration.

---

## Security

* **`usbipd` has no authentication.** Debian builds it against `libwrap`, so
  `server/hosts.allow` limits connections to RFC1918 and link-local ranges;
  everything else is denied by `server/hosts.deny`. If your LAN uses a
  different range, edit `server/hosts.allow` and reflash. Rebuild with
  `USBIP_LOCKDOWN=0 ./flash.sh /dev/sdX` to export to the whole LAN with no
  restriction (not recommended).
* **Break-glass SSH** is enabled (`usbip` / password in `CREDENTIALS.txt`, plus
  your `~/.ssh/*.pub` key). It is only for debugging — normal operation never
  needs it. `CREDENTIALS.txt` is git-ignored; delete it if you don't want it
  lying around (the hash lives only on the Pi).

---

## Troubleshooting

| Symptom | Check |
|---------|-------|
| `usbip-pi.local` doesn't resolve | If the Pi is on another subnet, mDNS won't cross it — use the router's DNS name (e.g. `usbip-pi.lan`) or the Pi's IP. Otherwise `avahi-resolve -n usbip-pi.local`. |
| `usbip ... -r <name>` fails "Unknown Op Common Status" | The name has an IPv6 (AAAA) record that plain `usbip` tries first. Use `./connect.sh` or the `client/` scripts (they resolve IPv4), or pass the IPv4 address. |
| `usbip list -r` shows nothing | Provisioning may still be running. SSH in and run `journalctl -u usbip-provision -f` (needs internet on first boot). |
| Device missing after replug | It is re-exported within 5 s. On the client, restart `usbip-attach` (module) or re-run the attach script. |
| Did the Pi actually boot the drive? | `./scripts/check-drive.sh /dev/sdX` (read-only) reports whether `firstrun.sh` ran. |
| Server logs | `journalctl -u usbipd -u usbip-autobind`; bind script log in `/var/log/usbip-provision.log`. |
| Wrong LAN range for restrictions | edit `server/hosts.allow`, rebuild, reflash. |

---

## Caveats

* Target is the **Raspberry Pi 3 Model B**. It can only boot from USB if the
  one-time OTP bit is set, and even then its USB boot is unreliable; booting
  from the microSD card always works and is recommended.
* Isochronous USB devices (webcams, USB audio) do not work well over USB/IP;
  interrupt/bulk devices (keyboards, mice, serial, storage) are fine.
* Only one client can use an exported device at a time — a USB limitation.
* The keyboard plugged into the Pi is **not** usable locally on the Pi (by
  design).

---

## Building, inspecting, resetting

`flash.sh` runs the build for you, but you can drive it directly:

```sh
nix-shell --run ./scripts/build-image.sh   # produce usbip-pi.img
USBPI_IMAGE_URL=... ./scripts/build-image.sh   # override the base image
./scripts/check-drive.sh /dev/sdX          # read-only diagnosis of a flashed drive
rm -rf build usbip-pi.img usbip-pi.img.sha256   # force a clean rebuild
```

---

## License

[WTFPL](LICENSE) — do what you want.
