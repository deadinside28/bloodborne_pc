#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
package.py - Automated build & packaging script for BBPort releases.

Compiles the native probe, GPU core, and tools (via build.sh),
organizes all required runtime files, scripts, patches, and launcher into a
clean, self-contained distribution layout, and compresses it into a .tar.gz
(or .tar.xz / .zip) ready to publish on GitHub Releases or distribute.
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent

def run_command(cmd, cwd=ROOT_DIR, check=True):
    print(f"\n[*] Running: {' '.join(cmd)}")
    res = subprocess.run(cmd, cwd=cwd)
    if check and res.returncode != 0:
        print(f"\n[!] Command failed with exit code {res.returncode}: {' '.join(cmd)}", file=sys.stderr)
        sys.exit(res.returncode)
    return res.returncode

def get_git_version():
    try:
        res = subprocess.run(
            ["git", "describe", "--tags", "--always"],
            cwd=ROOT_DIR,
            capture_output=True,
            text=True,
            check=True
        )
        ver = res.stdout.strip()
        if ver.startswith("v"):
            ver = ver[1:]
        return ver
    except Exception:
        return "0.5"

def get_ldd_dependencies(binary_path):
    """Parses ldd output to find dynamic library dependencies."""
    libs = {}
    try:
        res = subprocess.run(
            ["ldd", str(binary_path)],
            capture_output=True,
            text=True,
            check=True
        )
        for line in res.stdout.splitlines():
            m = re.match(r"\s*([^\s]+)\s*=>\s*([^\s]+)\s*\(0x", line)
            if m:
                libs[m.group(1)] = Path(m.group(2))
    except Exception as e:
        print(f"[!] Warning: failed to run ldd on {binary_path}: {e}")
    return libs

def should_bundle_library(name, path_str):
    """
    Decides whether a library should be bundled with the release.
    System/core libraries (glibc, X11, Vulkan loader, GPU driver libs) are excluded.
    Libraries from ~/.local or specific portable libraries (SDL3, miniz, zydis, fmt, xxhash) are bundled.
    """
    # Core system libraries that must ALWAYS come from the host OS
    system_prefixes = (
        "libc.so", "libm.so", "libpthread.so", "libdl.so", "librt.so",
        "ld-linux-x86-64.so", "libstdc++.so", "libgcc_s.so",
        "libGL.so", "libEGL.so", "libGLX.so", "libGLdispatch.so",
        "libvulkan.so", "libdrm.so", "libX11.so", "libxcb.so",
        "libresolv.so", "libnss", "libutil.so", "libsystemd.so"
    )
    if name == "libbbgpu.so":
        return False

    for p in system_prefixes:
        if name.startswith(p):
            return False

    # Always bundle if installed in non-standard / home / local paths
    if ".local" in path_str or "shadowy" in path_str:
        return True

    # Bundle specific modern libraries that older distributions often lack
    target_keywords = (
        "sdl3", "miniz", "zydis", "zycore", "fmt", "xxhash",
        "avcodec", "avformat", "avutil", "swscale", "swresample",
    )
    name_lower = name.lower()
    for kw in target_keywords:
        if kw in name_lower:
            return True

    return False

def create_start_script(package_dir):
    """Creates the user-friendly start.sh entrypoint in the package root."""
    start_sh = package_dir / "start.sh"
    content = """#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# BBPort Launcher & Game Runner
# Usage:
#   ./start.sh          -> Abre o Launcher GTK4 (ou o jogo se o launcher não puder ser aberto)
#   ./start.sh --play   -> Inicia o jogo diretamente com as configurações salvas
#   ./start.sh --help   -> Mostra opções do runtime
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export BB_PREBUILT=1

# Configura busca de bibliotecas compartilhadas empacotadas
if [[ -d "$HERE/lib" ]]; then
    export LD_LIBRARY_PATH="$HERE/lib:$HERE/bin/gpu:${LD_LIBRARY_PATH:-}"
else
    export LD_LIBRARY_PATH="$HERE/bin/gpu:${LD_LIBRARY_PATH:-}"
fi

# Shaders do FSR 4
if [[ -d "$HERE/fsr4_shaders" && -z "${BB_FSR4_SHADERS:-}" ]]; then
    export BB_FSR4_SHADERS="$HERE/fsr4_shaders"
fi

# Modo direto (--play ou --game)
if [[ "${1:-}" == "--play" || "${1:-}" == "--game" ]]; then
    shift
    exec "$HERE/run.sh" "$@"
fi

# Tenta abrir o Launcher nativo GTK 4 / Libadwaita
if python3 -c 'import gi; gi.require_version("Gtk", "4.0"); gi.require_version("Adw", "1")' 2>/dev/null; then
    exec python3 "$HERE/launcher/bbport_launcher.py" "$@"
else
    echo "================================================================="
    echo "Aviso: GTK 4 e/ou Libadwaita não encontrados no sistema."
    echo "Iniciando Bloodborne diretamente via run.sh..."
    echo "================================================================="
    exec "$HERE/run.sh" "$@"
fi
"""
    start_sh.write_text(content, encoding="utf-8")
    start_sh.chmod(0o755)

def create_readme(package_dir, version):
    """Creates README_RELEASE.md with execution instructions."""
    readme = package_dir / "README_RELEASE.md"
    content = f"""# Bloodborne PC Port (BBPort) - Release {version}

Versão compilada e pronta para execução em distribuições Linux (x86_64).

---

## 🎮 Como Jogar

1. **Coloque a pasta do jogo PS4 (`CUSA03173` contendo `eboot.bin`):**
   - Na mesma pasta deste pacote (ou ao lado dela), **OU**
   - Abra o inicializador e selecione a pasta da sua cópia do jogo.

2. **Inicie o jogo:**
   - **Com Launcher visual (GTK4 / Libadwaita):**
     ```bash
     ./start.sh
     ```
   - **Direto para o jogo (sem interface de launcher):**
     ```bash
     ./start.sh --play
     # ou
     ./run.sh
     ```

---

## ⚙️ Atalhos e Menus no Jogo
- **Menu de Configurações / UpScaler / Debugger:** Tecla `Insert` ou `L3 + R3` no controle.
- **Teclado Virtual:** Suporte a Gamepad e Teclado com navegação por D-pad / analógico.
- **Fechar:** `Alt + F4` ou `Esc` (no menu).

---

## 📦 Requisitos do Sistema
- **Sistema Operacional:** Linux x86_64 (glibc compatível).
- **GPU:** Suporte a **Vulkan 1.3** com drivers atualizados:
  - **AMD:** Mesa RADV (GCN 4 / Polaris, Vega, RDNA 1/2/3/4).
  - **NVIDIA:** Driver oficial proprietário (versão 535+ recomendada).
  - **Intel:** Mesa ANV (Intel Arc, Xe, Iris Xe).
- **Python 3:** Necessário para scripts de patches e carregamento.
- **Para o Launcher (Opcional):** `python3-gi`, `gir1.2-gtk-4.0`, `gir1.2-adw-1`.

---

*Gerado automaticamente pelo script package.py*
"""
    readme.write_text(content, encoding="utf-8")

def copy_tree_filtered(src_dir, dst_dir, ignore_patterns=(".pyc", "__pycache__", ".git")):
    """Recursively copies directory tree filtering out temporary and cache files."""
    dst_dir.mkdir(parents=True, exist_ok=True)
    for root, dirs, files in os.walk(src_dir):
        # Prune ignored directories
        dirs[:] = [d for d in dirs if not any(p in d for p in ignore_patterns)]
        rel = Path(root).relative_to(src_dir)
        target_sub = dst_dir / rel
        target_sub.mkdir(parents=True, exist_ok=True)
        for f in files:
            if any(f.endswith(p) or p in f for p in ignore_patterns):
                continue
            src_f = Path(root) / f
            dst_f = target_sub / f
            shutil.copy2(src_f, dst_f)

def build_and_package(args):
    print("=" * 70)
    print("      BBPort - Automated Build & Release Packager")
    print("=" * 70)

    version = args.name_version or get_git_version()
    print(f"[*] Detected Version: {version}")

    # 1. Build step
    if args.build:
        print("\n[+] Step 1: Building binaries with build.sh...")
        run_command(["bash", "build.sh"])
    else:
        print("\n[+] Step 1: Skipping build (--no-build specified).")

    # 2. Verify binary existence
    bin_probe = ROOT_DIR / "out" / "bb-probe"
    bin_caps = ROOT_DIR / "out" / "bb-gpu-capabilities"
    lib_gpu = ROOT_DIR / "out" / "gpu" / "libbbgpu.so"

    missing = [str(p) for p in [bin_probe, bin_caps, lib_gpu] if not p.exists() or p.stat().st_size == 0]
    if missing:
        print(f"\n[!] Error: Expected build output is missing or empty:\n    " + "\n    ".join(missing), file=sys.stderr)
        print("[!] Please run without --no-build or inspect out/gpu-build.log.", file=sys.stderr)
        sys.exit(1)

    # 3. Setup staging folder
    pkg_name = f"BBPort-{version}-linux-x86_64"
    staging_dir = ROOT_DIR / "out" / "package_staging" / pkg_name

    if staging_dir.exists():
        if args.clean or True:
            shutil.rmtree(staging_dir)
    staging_dir.mkdir(parents=True, exist_ok=True)

    print(f"\n[+] Step 2: Populating package structure in {staging_dir}...")

    # bin/ directory
    pkg_bin = staging_dir / "bin"
    pkg_bin_gpu = pkg_bin / "gpu"
    pkg_bin_gpu.mkdir(parents=True, exist_ok=True)

    shutil.copy2(bin_probe, pkg_bin / "bb-probe")
    (pkg_bin / "bb-probe").chmod(0o755)

    shutil.copy2(bin_caps, pkg_bin / "bb-gpu-capabilities")
    (pkg_bin / "bb-gpu-capabilities").chmod(0o755)

    shutil.copy2(lib_gpu, pkg_bin_gpu / "libbbgpu.so")
    (pkg_bin_gpu / "libbbgpu.so").chmod(0o755)

    if args.strip:
        print("[*] Stripping debug symbols (--strip-debug) from libbbgpu.so...")
        try:
            subprocess.run(["strip", "--strip-debug", str(pkg_bin_gpu / "libbbgpu.so")], check=True)
            subprocess.run(["strip", "--strip-debug", str(pkg_bin / "bb-gpu-capabilities")], check=True)
        except Exception as e:
            print(f"[!] Warning: strip failed: {e}")

    # lib/ directory for bundled dependencies
    if args.bundle_libs:
        print("[*] Collecting shared library dependencies for bundling...")
        pkg_lib = staging_dir / "lib"
        pkg_lib.mkdir(parents=True, exist_ok=True)

        found_deps = {}
        queue = [bin_probe, bin_caps, lib_gpu]
        processed = set()
        while queue:
            target = queue.pop(0)
            try:
                target_real = target.resolve()
            except Exception:
                target_real = target
            if target_real in processed:
                continue
            processed.add(target_real)
            deps = get_ldd_dependencies(target)
            for name, path in deps.items():
                if name not in found_deps:
                    found_deps[name] = path
                    if should_bundle_library(name, str(path)) and path.exists():
                        queue.append(path)

        bundled_count = 0
        for name, path in sorted(found_deps.items()):
            if should_bundle_library(name, str(path)):
                if path.exists():
                    target_file = pkg_lib / path.name
                    # Copy actual file and maintain symlink if any
                    real_file = path.resolve()
                    shutil.copy2(real_file, pkg_lib / real_file.name)
                    if real_file.name != path.name:
                        symlink_dest = pkg_lib / path.name
                        if not symlink_dest.exists():
                            symlink_dest.symlink_to(real_file.name)
                    print(f"    -> Bundled: {name} ({path})")
                    bundled_count += 1

        print(f"[*] Bundled {bundled_count} shared libraries into lib/")

    # Root execution scripts & configuration
    pkg_run = staging_dir / "run.sh"
    run_sh_src = (ROOT_DIR / "run.sh").read_text(encoding="utf-8")
    # Ensure BB_PREBUILT=1 defaults to on if running from package root
    if "export BB_PREBUILT=" not in run_sh_src:
        run_sh_src = run_sh_src.replace("set -euo pipefail\n", "set -euo pipefail\nexport BB_PREBUILT=${BB_PREBUILT:-1}\n", 1)
    pkg_run.write_text(run_sh_src, encoding="utf-8")
    pkg_run.chmod(0o755)

    create_start_script(staging_dir)
    create_readme(staging_dir, version)

    if (ROOT_DIR / "bbport.ini").exists():
        shutil.copy2(ROOT_DIR / "bbport.ini", staging_dir / "bbport.ini")
    if (ROOT_DIR / "mods.json").exists():
        shutil.copy2(ROOT_DIR / "mods.json", staging_dir / "mods.json")
    if (ROOT_DIR / "patches.json").exists():
        shutil.copy2(ROOT_DIR / "patches.json", staging_dir / "patches.json")

    # Directory trees
    print("[*] Copying launcher...")
    copy_tree_filtered(ROOT_DIR / "launcher", staging_dir / "launcher")
    # Make launcher scripts executable
    if (staging_dir / "launcher" / "bb-launcher.sh").exists():
        (staging_dir / "launcher" / "bb-launcher.sh").chmod(0o755)

    print("[*] Copying scripts...")
    copy_tree_filtered(ROOT_DIR / "scripts", staging_dir / "scripts")

    print("[*] Copying patches...")
    copy_tree_filtered(ROOT_DIR / "patches", staging_dir / "patches")

    if args.bundle_shaders and (ROOT_DIR / "fsr4_shaders").exists():
        print("[*] Copying fsr4_shaders...")
        copy_tree_filtered(ROOT_DIR / "fsr4_shaders", staging_dir / "fsr4_shaders")

    print("[*] Copying tools...")
    copy_tree_filtered(ROOT_DIR / "tools", staging_dir / "tools")
    if (staging_dir / "tools" / "fetch_fsr4_assets.sh").exists():
        (staging_dir / "tools" / "fetch_fsr4_assets.sh").chmod(0o755)

    # Empty user and mods directory placeholders
    (staging_dir / "mods").mkdir(exist_ok=True)
    (staging_dir / "mods" / ".gitkeep").touch()
    (staging_dir / "user").mkdir(exist_ok=True)
    (staging_dir / "user" / ".gitkeep").touch()

    # 4. Create compressed tarball
    out_dist_dir = Path(args.output_dir).resolve()
    out_dist_dir.mkdir(parents=True, exist_ok=True)

    archive_ext = ".tar.gz"
    if args.format == "tar.xz" or args.format == "xztar":
        archive_ext = ".tar.xz"
        tar_mode = "w:xz"
    else:
        archive_ext = ".tar.gz"
        tar_mode = "w:gz"

    archive_path = out_dist_dir / f"{pkg_name}{archive_ext}"
    print(f"\n[+] Step 3: Compressing into {archive_path.name}...")

    with tarfile.open(archive_path, tar_mode) as tar:
        tar.add(staging_dir, arcname=pkg_name)

    # 5. Compute SHA256 checksum
    hasher = hashlib.sha256()
    with open(archive_path, "rb") as f:
        while chunk := f.read(65536):
            hasher.update(chunk)
    sha256_hash = hasher.hexdigest()

    checksum_file = out_dist_dir / f"{archive_path.name}.sha256"
    checksum_file.write_text(f"{sha256_hash}  {archive_path.name}\n", encoding="utf-8")

    archive_size_mb = archive_path.stat().st_size / (1024 * 1024)

    print("\n" + "=" * 70)
    print("             RELEASE PACKAGE CREATED SUCCESSFULLY!")
    print("=" * 70)
    print(f"📦 Archive:  {archive_path}")
    print(f"📊 Size:     {archive_size_mb:.2f} MB")
    print(f"🔑 SHA256:   {sha256_hash}")
    print(f"📄 Checksum: {checksum_file}")
    print("=" * 70)
    print("\nPronto para publicação! Você pode fazer upload deste .tar.gz diretamente no GitHub Releases.")

def main():
    parser = argparse.ArgumentParser(description="Build and package BBPort release tarball.")
    parser.add_argument("--no-build", dest="build", action="store_false", default=True,
                        help="Skip compiling binaries with build.sh (use existing out/)")
    parser.add_argument("--output-dir", "-o", default="dist",
                        help="Output directory for the packaged archive (default: dist/)")
    parser.add_argument("--format", choices=["gztar", "tar.gz", "xztar", "tar.xz"], default="gztar",
                        help="Archive compression format (default: gztar)")
    parser.add_argument("--name-version", default=None,
                        help="Custom version string for package naming (default: from git)")
    parser.add_argument("--no-bundle-libs", dest="bundle_libs", action="store_false", default=True,
                        help="Do not bundle third-party dynamic libraries (SDL3, miniz, zydis, etc.)")
    parser.add_argument("--no-bundle-shaders", dest="bundle_shaders", action="store_false", default=True,
                        help="Do not bundle fsr4_shaders/ folder")
    parser.add_argument("--no-strip", dest="strip", action="store_false", default=True,
                        help="Do not strip debug symbols from libbbgpu.so")
    parser.add_argument("--docker", action="store_true", default=False,
                        help="Build inside an Ubuntu 22.04 Docker container for maximum Linux distribution compatibility (glibc 2.35)")
    parser.add_argument("--clean", action="store_true", default=True,
                        help="Clean staging directory before packaging")

    args = parser.parse_args()
    if args.docker:
        docker_script = ROOT_DIR / "packaging" / "docker-build.sh"
        if not docker_script.exists():
            print("[!] Error: packaging/docker-build.sh not found", file=sys.stderr)
            sys.exit(1)
        sys.exit(subprocess.run(["bash", str(docker_script)]).returncode)

    build_and_package(args)

if __name__ == "__main__":
    main()
