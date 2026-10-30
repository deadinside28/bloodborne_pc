# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""tools/fsr4cap/driver_model.py: which DLL holds FSR 4.1.2's providers and which device checks
`force` answers (synthetic PE32+ files with the RTTI chain the tool walks: type descriptor ->
complete object locator -> vtable -> slot 2). build_assets.sh patches the upscaler DLL itself when
`info` accepts it (AMD's FSR SDK 2.4 DLL holds all four providers), else the driver DLL."""
from paths import ROOT
import importlib.util
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = ROOT / "tools/fsr4cap/driver_model.py"
spec = importlib.util.spec_from_file_location("driver_model", TOOL)
driver_model = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver_model)

BASE = 0x180000000
SECTION_RVA, SECTION_RAW = 0x1000, 0x200
CHECK = b"\x48\x83\xec\x48\x48\x8b\xc2\x48"  # the device check's first bytes (sub rsp; mov rax, rdx)


def fake_dll(path, models):
    """A PE32+ whose one section holds, per model, the provider's RTTI chain and a device check."""
    body = bytearray()

    def add(data, align=16):
        body.extend(bytes(-len(body) % align))
        at = len(body)
        body.extend(data)
        return SECTION_RVA + at

    checks = {}
    for model in models:
        name = driver_model.PROVIDERS[model].encode() + b"\0"
        descriptor = add(bytes(16) + name)                      # vftable pointer, spare, name
        locator_at = len(body) + (-len(body) % 16)
        locator = SECTION_RVA + locator_at
        add(struct.pack("<IIIIII", 1, 0, 0, descriptor, 0, locator))  # x64 COL: signature 1, self
        checks[model] = add(CHECK)
        slots = [BASE + checks[model] if i == driver_model.CHECK_SLOT else 0 for i in range(4)]
        add(struct.pack("<Q", BASE + locator) + struct.pack("<4Q", *slots), align=8)
    header = bytearray(SECTION_RAW)
    header[0:2] = b"MZ"
    struct.pack_into("<I", header, 0x3C, 0x40)
    header[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", header, 0x44, 0x8664, 1, 0, 0, 0, 240, 0x2022)
    struct.pack_into("<HQ", header, 0x58, 0x20B, 0)
    struct.pack_into("<Q", header, 0x58 + 24, BASE)
    struct.pack_into("<8sIIII", header, 0x58 + 240, b".rdata", len(body), SECTION_RVA, len(body),
                     SECTION_RAW)
    path.write_bytes(bytes(header) + bytes(body))
    return path, {m: SECTION_RAW + rva - SECTION_RVA for m, rva in checks.items()}


def run(*args):
    return subprocess.run([sys.executable, str(TOOL), *map(str, args)], capture_output=True, text=True)


class DriverModelTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def test_sdk_2_4_dll_holds_the_four_providers(self):
        # AMD's FSR SDK 2.4 upscaler DLL (4.1.2): build_assets.sh then patches it, not the driver.
        dll, _ = fake_dll(self.root / "amd_fidelityfx_upscaler_dx12.dll", list(driver_model.PROVIDERS))
        out = run("info", dll)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        for model in driver_model.PROVIDERS:
            self.assertIn(f"{model}: found", out.stdout)

    def test_4_1_1_dll_is_refused(self):
        # A 4.1.1 upscaler DLL has its own provider only: the driver DLL has to give the models.
        dll, _ = fake_dll(self.root / "amd_fidelityfx_upscaler_dx12.dll", ["fp8"])
        out = run("info", dll)
        self.assertEqual(out.returncode, 2)
        self.assertIn("navi2: missing", out.stdout)

    def test_force_answers_yes_for_the_model_and_no_for_the_others(self):
        dll, checks = fake_dll(self.root / "amd_fidelityfx_upscaler_dx12.dll", list(driver_model.PROVIDERS))
        for want in ("navi2", "lite"):
            out_dll = self.root / f"{want}.dll"
            out = run("force", dll, out_dll, want)
            self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
            data = out_dll.read_bytes()
            for model, at in checks.items():
                expected = driver_model.YES if model == want else driver_model.NO
                self.assertEqual(data[at:at + 3], expected, f"{want}: {model}'s check")
                self.assertEqual(data[at + 3:at + 8], CHECK[3:], f"{want}: {model} beyond the stub")
            self.assertEqual(len(data), len(dll.read_bytes()))

    def test_force_refuses_without_all_providers(self):
        dll, _ = fake_dll(self.root / "amdxcffx64.dll", ["fp8", "int8"])
        out = run("force", dll, self.root / "out.dll", "navi2")
        self.assertEqual(out.returncode, 2)
        self.assertFalse((self.root / "out.dll").exists())


if __name__ == "__main__":
    unittest.main()
