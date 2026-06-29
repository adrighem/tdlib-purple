{ pkgs ? import <nixpkgs> {} }:

{
  default = pkgs.callPackage ./package.nix {};
}
