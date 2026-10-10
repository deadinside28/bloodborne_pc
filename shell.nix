# Build environment now is in flake.nix. 
# This file is just left here for backwords compatibility
{ system ? builtins.currentSystem }:
(builtins.getFlake (toString ./.)).devShells.${system}.default
