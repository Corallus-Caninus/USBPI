# Development shell for building the USBPI image.
#
#   nix-shell
#
# Uses your system nixpkgs channel (<nixpkgs>) and does NOT copy the directory
# (including the multi-GiB image) into the Nix store.  flake.nix provides the
# same shell for `nix develop` users.
{ pkgs ? import <nixpkgs> { } }:

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
  ];

  shellHook = ''
    echo "USBPI build shell"
    echo "  ./flash.sh             # build (if needed) + write image to a drive"
    echo "  ./connect.sh           # attach the Pi's devices to this machine"
  '';
}
