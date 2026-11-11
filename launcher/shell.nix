# Launcher environment: `nix-shell launcher/shell.nix --run 'python3 launcher/bbport_launcher.py'`
{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell {
  packages = with pkgs; [
    (python3.withPackages (ps: [ ps.pygobject3 ]))
    gtk4 libadwaita gobject-introspection
  ];
  # The same data directories packaging/default.nix gives the packaged launcher: icons for hosts
  # without an Adwaita theme, and GTK4's schemas, which the file chooser aborts without.
  shellHook = ''
    export XDG_DATA_DIRS=${pkgs.adwaita-icon-theme}/share:${pkgs.hicolor-icon-theme}/share:${pkgs.gtk4}/share/gsettings-schemas/${pkgs.gtk4.name}''${XDG_DATA_DIRS:+:$XDG_DATA_DIRS}
  '';
}
