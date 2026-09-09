{ pkgs ? import <nixpkgs> { } }:

pkgs.mkShell {
  inputsFrom = [ (import ./default.nix { inherit pkgs; }) ];
  packages = with pkgs; [
    clang-tools
    kdePackages.qttools
  ];
}
