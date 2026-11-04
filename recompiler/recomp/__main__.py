# SPDX-License-Identifier: GPL-2.0-or-later
"""python -m recomp lift IMAGE -o OUT.ll [--tso] [--fde]"""
import argparse
import sys

from .image import load_elf
from .lift import ModuleLifter


def read_addresses(path):
    """Link addresses, one per line (hex with 0x, or decimal); '#' starts a comment."""
    out = set()
    for line in open(path):
        line = line.split("#", 1)[0].strip()
        if line:
            out.add(int(line.split()[0], 0))
    return out


def main():
    ap = argparse.ArgumentParser(prog="recomp")
    sub = ap.add_subparsers(dest="cmd", required=True)
    lp = sub.add_parser("lift", help="lift every function of an x86-64 ELF image to LLVM IR")
    lp.add_argument("image")
    lp.add_argument("-o", "--output", required=True)
    lp.add_argument("--tso", action="store_true", help="x86 memory ordering (for AArch64 targets)")
    lp.add_argument("--abi", action="store_true", help="assume the SysV ABI for vector registers at calls")
    lp.add_argument("--tso-strict", action="store_true",
                    help="with --tso: barriers for rbp-based (frame) accesses too")
    lp.add_argument("--tso-mode", choices=("rcpc", "rcpc-fault", "barriers"), default="rcpc",
                    help="with --tso: rcpc (default): acquire loads / release stores (ldapr/stlr, ARMv8.3 RCpc), with "
                         "an inline check sending accesses that cross 16 bytes to a plain access + barrier; rcpc-fault: "
                         "no check, the runtime's SIGBUS handler does those; barriers: dmb after loads / before stores "
                         "(any ARMv8)"),
    lp.add_argument("--fde", action="store_true", help="function ranges from .eh_frame, not symbols")
    lp.add_argument("--select", metavar="MOD:REM",
                    help="hybrid tests: lift only the functions whose index (by address) is REM modulo MOD; "
                         "calls to the others go through __recomp_dispatch")
    lp.add_argument("--only", metavar="FILE", help="lift only the functions at these link addresses (one per line)")
    vhelp = ("x86-64 targets next to the original code (hybrid): guest memory accesses as volatile (no "
             "dropped, merged or hoisted loads and stores; the hardware keeps x86 ordering)")
    lp.add_argument("--volatile", action="store_true", help=vhelp)
    gp = sub.add_parser("game", help="survey or build the PS4 game executable (eboot.bin) in shards")
    gp.add_argument("eboot")
    gp.add_argument("--out", help="write part-NNNN.ll shards and table.ll here (default: survey only)")
    gp.add_argument("--report", help="write the survey as JSON")
    gp.add_argument("--shard", type=int, default=2000, help="functions per shard")
    gp.add_argument("--workers", type=int)
    gp.add_argument("--limit", type=int, help="only the first N functions")
    gp.add_argument("--abi", action="store_true")
    gp.add_argument("--tso", action="store_true")
    gp.add_argument("--only", metavar="FILE",
                    help="hybrid execution: lift only these functions (link addresses, one per line)")
    gp.add_argument("--live", metavar="IMAGE",
                    help="with --only: bbport's image as it runs (BB_AOT_DUMP); functions it changed stay original")
    gp.add_argument("--volatile", action="store_true", help=vhelp)
    a = ap.parse_args()
    if a.cmd == "game":
        from . import game
        from .ps4 import load_ps4
        img = load_ps4(a.eboot)
        only = read_addresses(a.only) if a.only else None
        live = open(a.live, "rb").read() if a.live else None
        total = game.run(img, out=a.out, shard_size=a.shard, workers=a.workers, tso=a.tso, abi=a.abi, limit=a.limit,
                         only=only, live=live, volatile=a.volatile)
        print(game.report(total, a.report))
        return
    img = load_elf(a.image, use_symbols=not a.fde)
    m = ModuleLifter(img, tso=a.tso, abi=a.abi, local_frames=not a.tso_strict,
                     rcpc={"rcpc": "check", "rcpc-fault": "fault", "barriers": None}[a.tso_mode], volatile=a.volatile)
    m.discover()
    if a.select or a.only:
        keep = set(m.cfgs)
        if a.select:
            mod, rem = (int(x) for x in a.select.split(":"))
            keep = {f for i, f in enumerate(sorted(m.cfgs)) if i % mod == rem}
        if a.only:
            keep &= read_addresses(a.only)
        m.cfgs = {f: c for f, c in m.cfgs.items() if f in keep}
        m.lifted_names = set(m.cfgs)
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
