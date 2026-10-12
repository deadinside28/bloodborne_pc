# SPDX-License-Identifier: GPL-2.0-or-later
"""Shader packs (launcher: "Experimental shaders"): folders of replacement shaders, one file per
translated shader, named as the port's patch files: <stage>_0x<hash>_<perm>.spv (docs/DEBUGGING.md).
At launch the enabled packs are copied into <user>/shader/patch and BB_PATCH_SHADERS=1 is set.
Files put into the patch folder by hand are left alone: only what the launcher copied before is
removed again (a manifest beside the profile).
"""
import json
import re
import shutil
from pathlib import Path

PATCH_FILE = re.compile(r"^[a-z]{2}_0x[0-9a-f]{16}_[0-9]+\.spv$")


def pack_files(folder):
    folder = Path(folder)
    if not folder.is_dir():
        return []
    return sorted((p for p in folder.iterdir() if p.is_file() and PATCH_FILE.match(p.name)),
                  key=lambda p: p.name)


def discover(root):
    """Named packs: folders of `root` holding at least one patch file."""
    root = Path(root)
    if not root.is_dir():
        return []
    return sorted((p.name for p in root.iterdir() if p.is_dir() and pack_files(p)),
                  key=lambda name: (name.casefold(), name))


def selected(root, config):
    """The enabled packs in load order (later packs win on the same shader)."""
    available = discover(root)
    try:
        profile = json.loads(Path(config).read_text(encoding="utf-8")) if config else {}
    except (OSError, ValueError):
        profile = {}
    disabled = set(profile.get("disabled", []))
    return [name for name in available if name not in disabled]


def apply(root, config, patch_dir, manifest=None):
    """Copies the enabled packs' files into `patch_dir`; returns how many files are in place.
    `manifest` (a JSON file) remembers what was copied, so a pack switched off is removed again."""
    patch_dir = Path(patch_dir)
    manifest = Path(manifest) if manifest else None
    previous = []
    if manifest and manifest.is_file():
        try:
            previous = json.loads(manifest.read_text(encoding="utf-8")).get("files", [])
        except (OSError, ValueError):
            previous = []
    for name in previous:
        if PATCH_FILE.match(name):
            (patch_dir / name).unlink(missing_ok=True)
    copied = []
    packs = selected(root, config)
    if packs:
        patch_dir.mkdir(parents=True, exist_ok=True)
        for name in packs:
            for source in pack_files(Path(root) / name):
                shutil.copyfile(source, patch_dir / source.name)
                if source.name not in copied:
                    copied.append(source.name)
    if manifest:
        manifest.parent.mkdir(parents=True, exist_ok=True)
        manifest.write_text(json.dumps({"files": copied}, indent=2) + "\n", encoding="utf-8")
    return len(copied)
