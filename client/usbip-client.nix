# USBPI client - NixOS module
#
# Attaches USB devices exported by a remote USB/IP server (e.g. the
# Raspberry Pi "usbip-pi" appliance) and keeps them attached.
#
# The usbip userspace tool is built from the kernel source that this system
# actually runs, so it always matches the running vhci-hcd driver.  (NixOS
# 24.11 does not package usbip yet.)
#
# Usage, in /etc/nixos/configuration.nix:
#
#   imports = [ /path/to/USBPI/client/usbip-client.nix ];
#
#   services.usbipClient = {
#     enable = true;
#     server = "usbip-pi.local";
#     # busid = "1-1.3";        # optional, otherwise autodetect
#     # vidPid = "046d:c31c";   # optional filter
#   };
#
# Then: nixos-rebuild switch
# The device is attached on boot and re-attached automatically if it drops.
{ config, lib, pkgs, ... }:

let
  cfg = config.services.usbipClient;

  kernel = config.boot.kernelPackages.kernel;

  # usbip userspace built against this system's kernel.
  usbip = pkgs.stdenv.mkDerivation {
    pname = "usbip";
    version = kernel.version;

    src = kernel.src;

    nativeBuildInputs = with pkgs; [ autoconf automake libtool ];
    buildInputs = [ pkgs.udev ];

    env.NIX_CFLAGS_COMPILE = "-Wno-error=address-of-packed-member";

    preConfigure = ''
      cd tools/usb/usbip
      ./autogen.sh
    '';

    configureFlags = [ "--with-usbids-dir=${pkgs.hwdata}/share/hwdata/" ];

    enableParallelBuilding = true;

    meta = {
      description = "Userspace tools for USB/IP, built against the running kernel";
      platforms = lib.platforms.linux;
    };
  };

  # single source of truth: the same script you can run by hand
  attachScript = pkgs.writeShellScript "usbip-sync" (builtins.readFile ./usbip-sync.sh);
in
{
  options.services.usbipClient = {
    enable = lib.mkEnableOption "USB/IP client that attaches devices from a remote server";

    server = lib.mkOption {
      type = lib.types.str;
      default = "usbip-pi.local";
      description = "Hostname or IP address of the USB/IP server.";
    };

    busid = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      example = "1-1.3";
      description = "busid of the device to attach. If null, the first exported device (optionally filtered by vidPid) is used.";
    };

    vidPid = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      example = "046d:c31c";
      description = "USB vendor:product ID used to pick a device when busid is null.";
    };

    all = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = "Attach every device the server exports instead of a single one.";
    };

    gadget = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Also attach the buffered HID gadgets exported by the server's HID mirror (device-mode usbipd).";
    };

    gadgetPort = lib.mkOption {
      type = lib.types.port;
      default = 3241;
      description = "TCP port of the server's device-mode usbipd that exports buffered HID gadgets.";
    };

    interval = lib.mkOption {
      type = lib.types.ints.positive;
      default = 10;
      description = "Seconds between attach attempts while the device is not attached.";
    };
  };

  config = lib.mkIf cfg.enable {
    boot.kernelModules = [ "vhci-hcd" ];

    environment.systemPackages = [ usbip ];

    systemd.services.usbip-attach = {
      description = "Attach USB device(s) exported by ${cfg.server}";
      wants = [ "network-online.target" ];
      after = [ "network-online.target" ];
      wantedBy = [ "multi-user.target" ];
      path = [ usbip pkgs.kmod pkgs.avahi pkgs.glibc.bin pkgs.coreutils pkgs.gnused pkgs.gnugrep pkgs.gawk ];
      environment = {
        USBIP_SERVER = cfg.server;
        USBIP_BUSID = lib.optionalString (cfg.busid != null) cfg.busid;
        USBIP_VIDPID = lib.optionalString (cfg.vidPid != null) cfg.vidPid;
        USBIP_ALL = lib.optionalString cfg.all "1";
        USBIP_INTERVAL = toString cfg.interval;
        USBIP_GADGET = if cfg.gadget then "1" else "0";
        USBIP_GADGET_PORT = toString cfg.gadgetPort;
      };
      serviceConfig = {
        Type = "simple";
        ExecStart = attachScript;
        Restart = "always";
        RestartSec = "5s";
      };
    };
  };
}
