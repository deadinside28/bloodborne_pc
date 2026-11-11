# Build environment: `nix-shell native_probe/shell.nix --run 'bash native_probe/run.sh'`
{ pkgs ? import <nixpkgs> {} }:
let
  # libdecor (window frames on GNOME's Wayland) without its GTK 3 plugin (GTK 3, tinysparql, ICU:
  # ~100 MB); its cairo plugin draws the frames.
  libdecor = pkgs.libdecor.overrideAttrs (old: {
    buildInputs = pkgs.lib.remove pkgs.gtk3 old.buildInputs;
    mesonFlags = old.mesonFlags ++ [ "-Dgtk=disabled" ];
  });
  # SDL3 names zenity (its message boxes) by store path, and zenity brings a whole GTK4 with
  # GStreamer into the AppImage (~200 MB). This one runs the host's zenity when there is one.
  sdl3 = pkgs.sdl3.override {
    inherit libdecor;
    zenity = pkgs.writeShellScriptBin "zenity" ''
      host=$(command -v zenity) && exec "$host" "$@"
      exit 1
    '';
  };
in
pkgs.mkShell {
  packages = with pkgs; [
    gcc gnumake cmake ninja pkg-config python3 binutils
    vulkan-headers vulkan-loader sdl3
    # GPU library (gpu/): shadPS4 video core dependencies
    ffmpeg-headless boost fmt magic-enum robin-map xxhash vulkan-memory-allocator glslang spirv-cross xbyak zydis spirv-headers miniz libx11 libxcb libxau xorgproto wayland alsa-lib
    # Online module (gpu/bbnet): shadNet client, HTTPS, UPnP
    protobuf nlohmann_json openssl zlib miniupnpc
  ];
  # NixOS keeps the driver's libraries in /run/opengl-driver/lib, where the DLSS bridge's dlopen of
  # libnvidia-ngx.so.1 does not look (the AppImage links them in: launcher/bbport_vulkan.py).
  # Only driver libraries live there, so nothing the shell provides is replaced.
  shellHook = ''
    export LD_LIBRARY_PATH=''${LD_LIBRARY_PATH:+$LD_LIBRARY_PATH:}${pkgs.addDriverRunpath.driverLink}/lib
  '';
}
