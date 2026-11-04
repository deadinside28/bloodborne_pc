# SPDX-License-Identifier: GPL-2.0-or-later
"""python -m recomp lift IMAGE -o OUT.ll [--tso] [--fde]"""
import argparse
import sys

from .image import load_elf
from .lift import ModuleLifter


def main():
    ap = argparse.ArgumentParser(prog="recomp")
    sub = ap.add_subparsers(dest="cmd", required=True)
    lp = sub.add_parser("lift", help="lift every function of an x86-64 ELF image to LLVM IR")
    lp.add_argument("image")
    lp.add_argument("-o", "--output", required=True)
    lp.add_argument("--tso", action="store_true", help="x86 memory ordering (for AArch64 targets)")
    lp.add_argument("--abi", action="store_true", help="assume the SysV ABI for vector registers at calls")
    lp.add_argument("--fde", action="store_true", help="function ranges from .eh_frame, not symbols")
    a = ap.parse_args()
    img = load_elf(a.image, use_symbols=not a.fde)
    m = ModuleLifter(img, tso=a.tso, abi=a.abi)
    m.discover()
    ir, errors = m.lift()
    with open(a.output, "w") as f:
        f.write(ir)
    blocks = sum(len(c.blocks) for c in m.cfgs.values())
    insns = sum(len(b.insns) for c in m.cfgs.values() for b in c.blocks.values())
    print(f"functions {len(m.lifted_names)}, blocks {blocks}, instructions {insns}", file=sys.stderr)
    for k, v in sorted(m.unsupported.items(), key=lambda kv: -kv[1]):
        print(f"unsupported: {k} x{v}", file=sys.stderr)
    for a_, e in errors:
        print(f"cfg {a_:#x}: {e}", file=sys.stderr)
    if m.unresolved:
        print("unresolved direct targets: " + " ".join(f"{t:#x}" for t in sorted(m.unresolved)), file=sys.stderr)


if __name__ == "__main__":
    main()
