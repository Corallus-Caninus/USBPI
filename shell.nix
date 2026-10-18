# Development shell for building the USBPI image.
#
#   nix-shell
#
# Uses your system nixpkgs channel (<nixpkgs>) and does NOT copy the directory
# (including the multi-GiB image) into the Nix store.  flake.nix provides the
# same shell for `nix develop` users.
{ pkgs ? import <nixpkgs> { } }:

let
  # armv7 hard-float cross toolchain for the buffered HID mirror daemon
  cross = pkgs.pkgsCross.armv7l-hf-multiplatform;
  glibcStatic = cross.glibc.static;   # libc.a for a fully static binary
in
pkgs.mkShell {
  packages = with pkgs; [
    curl
    xz
    mtools          # edits the FAT partition inside the raw image
    util-linux      # sfdisk / lsblk
    coreutils
    gnused
    gnugrep
    openssl         # break-glass password hash
    python3         # partition table parsing
    file
    cross.stdenv.cc # armv7l-unknown-linux-gnueabihf-gcc
    glibcStatic
  ];

  shellHook = ''
    export USBIP_ARM_CC="$(echo ${cross.stdenv.cc}/bin/*-gcc | head -n1)"
    export USBIP_ARM_STATIC_LIB="${glibcStatic}/lib"
    echo "USBPI build shell"
    echo "  ./flash.sh             # build (if needed) + write image to a drive"
    echo "  ./connect.sh           # attach the Pi's devices to this machine"
  '';
}
