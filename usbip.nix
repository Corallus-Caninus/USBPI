# Build the USB/IP userspace tools (usbip + usbipd) from the kernel source of
# the kernel your NixOS is configured to use, via the system nixpkgs channel.
#
#   nix-build --no-out-link ./usbip.nix   ->  /nix/store/.../bin/usbip
#
# The client scripts use this automatically when usbip is not in PATH.
{ pkgs ? import <nixpkgs> { } }:

let
  kernel = pkgs.linuxPackages.kernel;
in
pkgs.stdenv.mkDerivation {
  pname = "usbip";
  version = kernel.version;

  src = kernel.src;
  patches = kernel.patches or [ ];

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
    description = "USB/IP userspace tools built from the configured kernel";
    platforms = pkgs.lib.platforms.linux;
  };
}
