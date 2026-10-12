# SPDX-License-Identifier: GPL-2.0-or-later
"""Shader packs (scripts/shader_patches.py): discovery, the profile, the copy at launch."""
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import shader_patches  # noqa: E402

NAME_A = "fs_0x0000000083da8467_0.spv"
NAME_B = "fs_0x00000000c45c51cb_0.spv"


class ShaderPatchesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name) / "shaders"
        (self.root / "gtao").mkdir(parents=True)
        (self.root / "gtao" / NAME_A).write_bytes(b"A1")
        (self.root / "gtao" / "notes.txt").write_text("not a patch")
        (self.root / "upsample").mkdir()
        (self.root / "upsample" / NAME_A).write_bytes(b"A2")
        (self.root / "upsample" / NAME_B).write_bytes(b"B")
        (self.root / "empty").mkdir()
        (self.root / "bad").mkdir()
        (self.root / "bad" / "shader.spv").write_bytes(b"x")  # not named by hash: not a pack
        self.config = Path(self.tmp.name) / "shaders.json"
        self.manifest = Path(self.tmp.name) / "installed.json"
        self.patch_dir = Path(self.tmp.name) / "user" / "shader" / "patch"

    def tearDown(self):
        self.tmp.cleanup()

    def test_discover_only_folders_with_patch_files(self):
        self.assertEqual(shader_patches.discover(self.root), ["gtao", "upsample"])
        self.assertEqual(shader_patches.discover(self.root / "missing"), [])

    def test_apply_copies_enabled_packs_later_pack_wins(self):
        count = shader_patches.apply(self.root, self.config, self.patch_dir, self.manifest)
        self.assertEqual(count, 2)
        self.assertEqual((self.patch_dir / NAME_A).read_bytes(), b"A2")
        self.assertEqual((self.patch_dir / NAME_B).read_bytes(), b"B")
        self.assertEqual(json.loads(self.manifest.read_text())["files"], [NAME_A, NAME_B])

    def test_disabled_pack_removed_again_manual_files_kept(self):
        shader_patches.apply(self.root, self.config, self.patch_dir, self.manifest)
        (self.patch_dir / "fs_0x0000000000000001_0.spv").write_bytes(b"mine")
        self.config.write_text(json.dumps({"disabled": ["upsample"]}))
        count = shader_patches.apply(self.root, self.config, self.patch_dir, self.manifest)
        self.assertEqual(count, 1)
        self.assertEqual((self.patch_dir / NAME_A).read_bytes(), b"A1")
        self.assertFalse((self.patch_dir / NAME_B).exists())
        self.assertTrue((self.patch_dir / "fs_0x0000000000000001_0.spv").exists())
        self.config.write_text(json.dumps({"disabled": ["upsample", "gtao"]}))
        self.assertEqual(shader_patches.apply(self.root, self.config, self.patch_dir, self.manifest), 0)
        self.assertFalse((self.patch_dir / NAME_A).exists())


if __name__ == "__main__":
    unittest.main()
