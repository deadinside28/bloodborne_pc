# Launcher environment lives in flake.nix (`.#launcher`, pinned nixos-26.05).
# This shim keeps `nix-shell launcher/shell.nix` working.
{ system ? builtins.currentSystem }:
(builtins.getFlake (toString ./..)).devShells.${system}.launcher
