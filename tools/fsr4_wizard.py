#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Automated helper and wizard for FSR 4.1.1 setup in BBPort.

Scans the system for amd_fidelityfx_upscaler_dx12.dll (Goverlay, OptiScaler, Steam, etc.),
verifies compatibility, and automates building the full SPIR-V/weight asset set.
Can be run standalone from CLI or imported by bbport_launcher.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
FSR4CAP_DIR = HERE / "fsr4cap"
sys.path.insert(0, str(FSR4CAP_DIR))

try:
    import dll_info
except ImportError:
    dll_info = None

SEARCH_DIRECTORIES = [
    Path.home() / ".local" / "share" / "goverlay",
    Path.home() / ".var" / "app" / "io.github.benjamimgois.goverlay",
    Path.home() / ".local" / "share" / "Steam",
    Path.home() / ".steam",
    Path.home() / "Downloads",
    Path.home() / "Documentos",
    Path.home() / "Games",
    Path.home() / ".local" / "share" / "lutris",
    Path.home() / ".var" / "app" / "net.lutris.Lutris",
    Path.home() / ".var" / "app" / "com.heroicgameslauncher.hgl",
    Path.home() / ".config" / "heroic",
]


def scan_system_dlls():
    """Scans common Linux directories for compatible FSR 4.1.x upscaler DLLs.

    Returns a list of dicts:
        [{
            'path': Path,
            'version': str,
            'models': list,
            'loader': str or None,
            'source': str
        }, ...]
    """
    if not dll_info:
        return []

    found = []
    seen_paths = set()

    for base_dir in SEARCH_DIRECTORIES:
        if not base_dir.is_dir():
            continue
        try:
            # Look for amd_fidelityfx_upscaler_dx12.dll
            for p in base_dir.rglob("amd_fidelityfx_upscaler_dx12.dll"):
                resolved = p.resolve()
                if resolved in seen_paths:
                    continue
                seen_paths.add(resolved)

                try:
                    status, problem, details = dll_info.inspect(p)
                    if status == 0 and dll_info.SUPPORTED_MODEL in details.get("models", []):
                        # Determine source label
                        p_str = str(resolved)
                        if "goverlay" in p_str:
                            source = "Goverlay"
                        elif "Steam" in p_str or ".steam" in p_str:
                            source = "Steam"
                        elif "lutris" in p_str.lower():
                            source = "Lutris"
                        elif "heroic" in p_str.lower():
                            source = "Heroic"
                        elif "Downloads" in p_str:
                            source = "Downloads"
                        else:
                            source = "Sistema"

                        found.append({
                            "path": resolved,
                            "version": details.get("version", "?"),
                            "models": details.get("models", []),
                            "loader": details.get("loader"),
                            "source": source
                        })
                except Exception:
                    continue
        except (PermissionError, OSError):
            continue

    # Sort so that highest version or Goverlay/Latest comes first
    def sort_key(item):
        p_str = str(item["path"])
        score = 0
        if "Latest" in p_str:
            score += 100
        if "goverlay" in p_str:
            score += 50
        v_parts = [int(x) if x.isdigit() else 0 for x in item["version"].split(".")]
        return (score, v_parts)

    found.sort(key=sort_key, reverse=True)
    return found


def auto_select_best_dll():
    """Returns the best candidate DLL path and loader, or (None, None)."""
    candidates = scan_system_dlls()
    if candidates:
        best = candidates[0]
        return best["path"], best["loader"]
    return None, None


def download_optiscaler_release(dest_dir=None):
    """Downloads OptiScaler release from GitHub and extracts amd_fidelityfx_upscaler_dx12.dll."""
    if dest_dir is None:
        dest_dir = ROOT / "out" / "fsr4cap" / "download"
    dest_dir = Path(dest_dir)
    dest_dir.mkdir(parents=True, exist_ok=True)

    api_url = "https://api.github.com/repos/optiscaler/OptiScaler/releases/latest"
    req = urllib.request.Request(api_url, headers={"User-Agent": "BBPort-FSR4-Wizard"})

    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            data = json.loads(resp.read().decode())
    except Exception as e:
        raise RuntimeError(f"Erro ao consultar API do OptiScaler no GitHub: {e}")

    archive_url = None
    archive_name = None
    for asset in data.get("assets", []):
        name = asset.get("name", "")
        if name.endswith(".7z") or name.endswith(".zip"):
            archive_url = asset.get("browser_download_url")
            archive_name = name
            break

    if not archive_url:
        raise RuntimeError("Nenhum arquivo .7z ou .zip encontrado na release mais recente do OptiScaler.")

    archive_path = dest_dir / archive_name
    print(f"Baixando OptiScaler de {archive_url}...")
    urllib.request.urlretrieve(archive_url, archive_path)

    # Extract using 7z or unzip
    extract_dir = dest_dir / "extracted"
    extract_dir.mkdir(parents=True, exist_ok=True)

    if archive_name.endswith(".7z"):
        if shutil.which("7z"):
            subprocess.run(["7z", "x", "-y", f"-o{extract_dir}", str(archive_path)], check=True)
        elif shutil.which("7za"):
            subprocess.run(["7za", "x", "-y", f"-o{extract_dir}", str(archive_path)], check=True)
        else:
            raise RuntimeError("7z/7za não encontrado no sistema para descompactar o arquivo .7z.")
    else:
        shutil.unpack_archive(str(archive_path), extract_dir)

    # Look for amd_fidelityfx_upscaler_dx12.dll
    for p in extract_dir.rglob("amd_fidelityfx_upscaler_dx12.dll"):
        if dll_info:
            status, _, details = dll_info.inspect(p)
            if status == 0:
                return p, details.get("loader")
        else:
            return p, None

    raise RuntimeError("DLL amd_fidelityfx_upscaler_dx12.dll compatível não encontrada dentro do pacote do OptiScaler.")


def run_build(upscaler_path, loader_path=None, on_progress=None):
    """Executes build_assets.sh with the specified upscaler and optional loader.

    Calls on_progress(line) if provided. Returns (status, output_log).
    """
    build_script = FSR4CAP_DIR / "build_assets.sh"
    cmd = ["bash", str(build_script), str(upscaler_path)]
    if loader_path:
        cmd.append(str(loader_path))

    env = dict(os.environ)
    proc = subprocess.Popen(
        cmd,
        cwd=str(ROOT),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1
    )

    lines = []
    for line in proc.stdout:
        lines.append(line)
        if on_progress:
            on_progress(line.rstrip())
        else:
            print(line, end="")

    proc.wait()
    return proc.returncode, "".join(lines)


def main():
    parser = argparse.ArgumentParser(description="BBPort FSR 4.1.1 Setup Wizard")
    parser.add_argument("--scan", action="store_true", help="Varre o sistema e lista as DLLs encontradas")
    parser.add_argument("--auto", action="store_true", help="Seleciona automaticamente a melhor DLL e compila os assets")
    parser.add_argument("--download", action="store_true", help="Baixa a versão mais recente do OptiScaler do GitHub")
    parser.add_argument("--dll", type=str, help="Caminho manual para amd_fidelityfx_upscaler_dx12.dll")
    parser.add_argument("--loader", type=str, help="Caminho manual para o loader DLL (se necessário)")

    args = parser.parse_args()

    print("=== Assistente de Configuração FSR 4.1.1 (BBPort) ===")

    if args.scan:
        print("Escaneando sistema por amd_fidelityfx_upscaler_dx12.dll...")
        found = scan_system_dlls()
        if not found:
            print("Nenhuma DLL compatível encontrada nas pastas padrão.")
            return 1
        print(f"Encontradas {len(found)} DLL(s) compatíveis:")
        for idx, item in enumerate(found, 1):
            print(f" [{idx}] [{item['source']}] v{item['version']} - {item['path']}")
            if item["loader"]:
                print(f"     Loader: {item['loader']}")
        return 0

    target_dll = None
    target_loader = None

    if args.dll:
        target_dll = Path(args.dll).resolve()
        if not target_dll.is_file():
            print(f"Erro: Arquivo não encontrado: {target_dll}", file=sys.stderr)
            return 1
        target_loader = Path(args.loader).resolve() if args.loader else None
    elif args.download:
        print("Baixando do OptiScaler...")
        target_dll, target_loader = download_optiscaler_release()
        print(f"DLL obtida: {target_dll}")
    elif args.auto:
        print("Detectando melhor DLL automaticamente...")
        target_dll, target_loader = auto_select_best_dll()
        if not target_dll:
            print("Nenhuma DLL encontrada no sistema. Tentando baixar do OptiScaler...")
            try:
                target_dll, target_loader = download_optiscaler_release()
            except Exception as e:
                print(f"Falha ao baixar: {e}", file=sys.stderr)
                return 1
        print(f"DLL selecionada: {target_dll}")
    else:
        # Modo Interativo
        print("1. Varrendo sistema...")
        found = scan_system_dlls()
        if found:
            print(f"Encontradas {len(found)} DLL(s) no seu computador:")
            for idx, item in enumerate(found, 1):
                print(f" [{idx}] {item['source']}: v{item['version']} ({item['path']})")
            print(f" [d] Baixar versão mais recente do OptiScaler (GitHub)")
            print(f" [m] Inserir caminho manualmente")
            print(f" [0] Cancelar")

            choice = input(f"\nEscolha uma opção [1-{len(found)}/d/m/0] (padrão: 1): ").strip().lower()
            if not choice:
                choice = "1"

            if choice == "0":
                print("Operação cancelada.")
                return 0
            elif choice == "d":
                target_dll, target_loader = download_optiscaler_release()
            elif choice == "m":
                path_str = input("Caminho completo da DLL: ").strip()
                target_dll = Path(path_str).resolve()
            elif choice.isdigit() and 1 <= int(choice) <= len(found):
                sel = found[int(choice) - 1]
                target_dll = sel["path"]
                target_loader = sel["loader"]
            else:
                print("Opção inválida.")
                return 1
        else:
            print("Nenhuma DLL compatível encontrada no sistema.")
            choice = input("Deseja baixar o OptiScaler automaticamente do GitHub? [S/n]: ").strip().lower()
            if choice in ("", "s", "sim", "y", "yes"):
                target_dll, target_loader = download_optiscaler_release()
            else:
                print("Cancelado.")
                return 0

    print(f"\nIniciando geração de assets FSR 4.1.1 a partir de:\n  {target_dll}")
    if target_loader:
        print(f"  Loader: {target_loader}")

    status, _ = run_build(target_dll, target_loader)
    if status == 0:
        print("\nSucesso! Os assets do FSR 4.1.1 foram gerados e validados em fsr4_411/")
        print("Você já pode selecionar 'FSR 4.1.1' no launcher ou definir upscaler=fsr411 no bbport.ini.")
        return 0
    else:
        print(f"\nFalha ao gerar assets (código {status}). Verifique as mensagens acima.", file=sys.stderr)
        return status


if __name__ == "__main__":
    sys.exit(main())
