{
  description = "USBPI - Raspberry Pi 3 USB/IP appliance builder + NixOS client";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      lib = nixpkgs.lib;
      systems = [ "x86_64-linux" "aarch64-linux" ];
      eachSystem = f: lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      # Import into a NixOS configuration.  Either by path:
      #   imports = [ /path/to/USBPI/client/usbip-client.nix ];
      # or through the flake:
      #   imports = [ (builtins.getFlake "/path/to/USBPI").nixosModules.usbip-client ];
      nixosModules.usbip-client = import ./client/usbip-client.nix;

      devShells = eachSystem (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [
            # image build tooling (no root, no loop mounts needed)
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
        };
      });
    };
}
