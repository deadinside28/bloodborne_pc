{
  description = "bbport build environment and AppImage";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    nix-appimage.url = "github:ralismark/nix-appimage";
    nix-appimage.inputs.nixpkgs.follows = "nixpkgs";
    # Project source with git submodules (fsr-vulkan, imgui, LibAtrac9, SPIRV-Headers).
    bbsrc = {
      url = "git+https://github.com/deadinside28/bloodborne_pc.git?ref=master&submodules=1";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, nix-appimage, bbsrc }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      lib = pkgs.lib;

      buildTools = with pkgs; [
        gcc gnumake cmake ninja pkg-config python3 binutils gnupatch
      ];

      # Libraries for the GPU core (from original shell.nix)
      buildLibs = with pkgs; [
        vulkan-headers vulkan-loader sdl3
        ffmpeg-headless boost fmt magic-enum robin-map xxhash
        vulkan-memory-allocator glslang spirv-cross xbyak zydis
        spirv-headers miniz libx11 libxcb xorgproto wayland
      ];

      # GTK4 launcher dev environment (python + pygobject + gtk4).
      launcherDeps = with pkgs; [
        (python3.withPackages (ps: [ ps.pygobject3 ]))
        gtk4 libadwaita gobject-introspection
      ];

      # Build the binaries. stdenvNoCC mimics 'bash build.sh'
      bbportBinaries = pkgs.stdenvNoCC.mkDerivation {
        pname = "bbport-binaries";
        version = "0.4.0";
        src = bbsrc;
        nativeBuildInputs = buildTools;
        buildInputs = buildLibs;
        # CMake doesn't care about stdenv.
        dontConfigure = true;

        buildPhase = ''
          runHook preBuild
          set -euo pipefail

          # Apply this port's FSR-Vulkan changes (as build.sh does).
          for patch in gpu/patches/fsr-vulkan/*.patch; do
            patch -p1 -d gpu/third_party/fsr-vulkan < "$PWD/$patch"
          done

          # shadPS4 video core. Same CMake flags as build.sh.
          cmake -S gpu -B out/gpu -G Ninja \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DBB_PGO=off -DBB_LTO=ON \
            -DBB_PGO_DIR="$PWD/pgo"
          ninja -C out/gpu bbgpu

          rm -rf out/atrac9 && mkdir -p out/atrac9
          for source in third_party/LibAtrac9/C/src/*.c; do
            cc -std=c99 -O2 -g -w -c "$source" -o "out/atrac9/$(basename "''${source%.c}").o"
          done
          ar rcs out/libatrac9.a out/atrac9/*.o

          # Probe + runtime + smoke tests. $ORIGIN/gpu: bundled as bin/gpu/libbbgpu.so.
          read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
          read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3)"
          cc -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -no-pie "''${includes[@]}" \
            -I. -Isrc src/probe.c src/runtime*.c src/vulkan_smoke.c out/libatrac9.a -lm \
            -Lout/gpu -lbbgpu -Wl,-rpath,'$ORIGIN/gpu' -rdynamic "''${libraries[@]}" \
            -o out/bb-probe
          cc -std=c11 -O2 -Wall -Wextra -Werror "''${includes[@]}" \
            tools/gpu_capabilities.c "''${libraries[@]}" -o out/bb-gpu-capabilities

          runHook postBuild
        '';

        installPhase = ''
          runHook preInstall
          mkdir -p "$out/gpu"
          install -m755 out/bb-probe            "$out/bb-probe"
          install -m755 out/bb-gpu-capabilities "$out/bb-gpu-capabilities"
          install -m755 out/gpu/libbbgpu.so     "$out/gpu/libbbgpu.so"
          runHook postInstall
        '';
      };

      # Packaged port. Reuses packaging/default.nix and the binaries built above
      bbport = pkgs.callPackage ./packaging {
        inherit pkgs;
        binaries = bbportBinaries;
        source = bbsrc;
        runtimePaths = [ ];
      };

      appimage = nix-appimage.lib.${system}.mkAppImage {
        pname = "Bloodborne-bbport-x86_64";
        program = lib.getExe bbport;
      };

    in
    {
      packages.${system} = {
        inherit bbport appimage;
        binaries = bbportBinaries;
        default = bbport;
      };

      overlays.default = final: prev: {
        bbport = self.packages.${final.system}.bbport;
      };

      devShells.${system} = {
        default = pkgs.mkShell {
          packages = buildTools ++ buildLibs;
        };

        launcher = pkgs.mkShell {
          packages = launcherDeps;
        };

        full = pkgs.mkShell {
          packages = buildTools ++ buildLibs ++ launcherDeps;
        };
      };
    };
}
