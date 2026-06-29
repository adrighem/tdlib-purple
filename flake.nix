{
  description = "libpurple Telegram plugin using tdlib";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-unstable";
  };

  outputs = { self, nixpkgs }: {
    packages = nixpkgs.lib.genAttrs [
      "x86_64-linux"
      "aarch64-linux"
      "i686-linux"
    ] (system: let
      pkgs = import nixpkgs {inherit system;};
    in rec {
      default = pkgs.callPackage ./package.nix {};
      tdlib-purple = default;
    });
  };
}
