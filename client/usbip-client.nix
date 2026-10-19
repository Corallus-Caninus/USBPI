# USBPI client - NixOS module
#
# Attaches USB devices exported by a remote USB/IP server (e.g. the
# Raspberry Pi "usbip-pi" appliance), and receives keyboards over the
# low-latency uinput fast path.
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

  # Single C client (attach/detach/watch/keyd), built by build.sh.
  usbpi = pkgs.stdenv.mkDerivation {
    pname = "usbpi";
    version = "0.1";

    src = lib.cleanSourceWith {
      src = ../.;
      filter = path: type:
        let base = baseNameOf path;
        in !(base == "build" || base == "result"
             || base == ".git" || lib.hasPrefix "usbip-pi.img" base);
    };

    nativeBuildInputs = [ pkgs.gcc ];
    dontConfigure = true;
    doCheck = false;
    buildPhase = ''
      bash ./build.sh client
    '';
    installPhase = ''
      install -Dm755 build/usbpi $out/bin/usbpi
    '';

    meta = {
      description = "USBPI client (USB/IP attach + keyboard fast path)";
      platforms = lib.platforms.linux;
    };
  };

  commonEnv = {
    USBIP_SERVER = cfg.server;
    USBIP_BUSID = lib.optionalString (cfg.busid != null) cfg.busid;
    USBIP_VIDPID = lib.optionalString (cfg.vidPid != null) cfg.vidPid;
    USBIP_ALL = if cfg.all then "1" else "0";
    USBIP_INTERVAL = toString cfg.interval;
    USBIP_GADGET = if cfg.gadget then "1" else "0";
    USBIP_GADGET_PORT = toString cfg.gadgetPort;
  };

  servicePath = [ usbip pkgs.kmod pkgs.avahi pkgs.glibc.bin pkgs.coreutils ];
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
      description = "Also attach the buffered HID gadgets exported by the server's legacy gadget port.";
    };

    gadgetPort = lib.mkOption {
      type = lib.types.port;
      default = 3241;
      description = "TCP port of the server's device-mode usbipd that exports buffered HID gadgets.";
    };

    keyboard = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Run the keyboard fast-path daemon (uinput). Disable to use plain USB/IP for keyboards.";
    };

    interval = lib.mkOption {
      type = lib.types.ints.positive;
      default = 10;
      description = "Seconds between attach attempts while the device is not attached.";
    };
  };

  config = lib.mkIf cfg.enable {
    boot.kernelModules = [ "vhci-hcd" "uinput" ];

    environment.systemPackages = [ usbip usbpi ];

    systemd.services.usbip-attach = {
      description = "Attach USB devices exported by ${cfg.server}";
      wants = [ "network-online.target" ];
      after = [ "network-online.target" ];
      wantedBy = [ "multi-user.target" ];
      path = servicePath;
      environment = commonEnv;
      serviceConfig = {
        Type = "simple";
        ExecStart = "${usbpi}/bin/usbpi watch";
        Restart = "always";
        RestartSec = "5s";
        Nice = -10;
        IOSchedulingClass = "realtime";
        LimitMEMLOCK = "infinity";
      };
    };

    systemd.services.usbip-keyd = lib.mkIf cfg.keyboard {
      description = "USBPI keyboard fast path from ${cfg.server}";
      wants = [ "network-online.target" ];
      after = [ "network-online.target" ];
      wantedBy = [ "multi-user.target" ];
      path = servicePath;
      environment = {
        USBIP_SERVER = cfg.server;
      };
      serviceConfig = {
        Type = "simple";
        ExecStart = "${usbpi}/bin/usbpi keyd";
        Restart = "always";
        RestartSec = "2s";
        Nice = -10;
        CPUSchedulingPolicy = "fifo";
        CPUSchedulingPriority = 40;
        IOSchedulingClass = "realtime";
        LimitRTPRIO = 99;
        LimitMEMLOCK = "infinity";
      };
    };
  };
}
