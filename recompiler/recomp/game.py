# SPDX-License-Identifier: GPL-2.0-or-later
"""Whole-game lifting: a survey of every function, and a sharded build.

The game has ~163k functions and ~8.5M instructions (~4.5 GB of IR), so it is lifted in shards:
part-NNNN.ll each define a slice of the functions and declare the ones they call in other
shards; table.ll holds __recomp_table (link address -> function). Work is spread over processes
(fork: the image is shared copy-on-write).
"""
import collections
import json
import multiprocessing as mp
import os
import re
import time

import iced_x86 as X

from . import cfg as cfgmod
from .lift import (FunctionLifter, ModuleLifter, STATE_TYPE, X_MNEMONIC_NAMES, fname, sconst)

FC = X.FlowControl
_IMG = None
_STARTS = None
_OPTS = None


def _init(img, starts, opts):
    global _IMG, _STARTS, _OPTS
    _IMG, _STARTS, _OPTS = img, starts, opts


def _work(job):
    """Recover and lift one slice of functions. Returns statistics (and writes a shard)."""
    index, addrs = job
    img, starts = _IMG, _STARTS
    mod = ModuleLifter(img, tso=_OPTS["tso"], abi=_OPTS["abi"], rcpc="check")
    mod.lifted_names = starts
    st = collections.Counter()
    unsupported = collections.Counter()
    unsupported_at = {}
    cfg_errors = collections.Counter()
    cfg_error_at = {}
    mnemonics = collections.Counter()
    extra_calls = collections.Counter()
    bodies = []
    t0 = time.time()
    for a in addrs:
        fn = img.functions[a]
        try:
            c = cfgmod.recover(img, fn, starts, mod.is_noreturn)
        except Exception as ex:  # a recovery crash is a finding, not a stop
            cfg_errors[f"recover exception: {type(ex).__name__}"] += 1
            cfg_error_at.setdefault(f"recover exception: {type(ex).__name__}", a)
            continue
        st["functions"] += 1
        if not c.blocks:
            st["functions without code"] += 1
            continue
        for b in c.blocks.values():
            st["blocks"] += 1
            st["instructions"] += len(b.insns)
            for i in b.insns:
                mnemonics[i.mnemonic] += 1
            last = b.insns[-1]
            if last.flow_control == FC.INDIRECT_BRANCH:
                st["indirect jumps: jump table" if b.table else "indirect jumps: dispatcher"] += 1
            for i in b.insns:
                if i.flow_control == FC.INDIRECT_CALL:
                    st["indirect calls"] += 1
                elif i.flow_control == FC.CALL:
                    st["direct calls"] += 1
        for e_addr, e in c.errors:
            cfg_errors[e] += 1
            cfg_error_at.setdefault(e, e_addr)
        for t in c.call_targets:
            if t in starts:
                continue
            if mod.import_stub(t) is not None:
                st["direct calls to import stubs"] += 1
            else:
                extra_calls[t] += 1
        if _OPTS.get("cfg_only"):
            continue
        before = dict(mod.unsupported)
        try:
            ir = FunctionLifter(mod, c).lift()
        except Exception as ex:
            cfg_errors[f"lift exception: {type(ex).__name__}: {ex}"[:120]] += 1
            cfg_error_at.setdefault(f"lift exception: {type(ex).__name__}: {ex}"[:120], a)
            continue
        for k, v in mod.unsupported.items():
            if v != before.get(k, 0):
                unsupported_at.setdefault(k, a)
        st["ir bytes"] += len(ir)
        if _OPTS["out"]:
            bodies.append(ir)
    unsupported.update(mod.unsupported)
    if _OPTS["out"]:
        _write_shard(_OPTS["out"], index, bodies, mod.intrinsics, starts)
    st["seconds"] = time.time() - t0
    return dict(stats=st, unsupported=unsupported, unsupported_at=unsupported_at, cfg_errors=cfg_errors,
                cfg_error_at=cfg_error_at, mnemonics=mnemonics, extra_calls=extra_calls)


HEAD = [STATE_TYPE, "@__recomp_slide = external global i64",
        "declare void @__recomp_dispatch(ptr, i64)", "declare void @__recomp_trap(ptr, i64, i32)"]
_FREF = re.compile(r"@F_([0-9a-f]+)\b")


def _write_shard(out, index, bodies, intrinsics, starts):
    defined = set()
    for b in bodies:
        m = re.match(r"define void @F_([0-9a-f]+)\(", b)
        defined.add(m.group(1))
    used = set()
    for b in bodies:
        used.update(_FREF.findall(b))
    decls = [f"declare void @F_{h}(ptr)" for h in sorted(used - defined)]
    with open(os.path.join(out, f"part-{index:04d}.ll"), "w") as f:
        f.write("\n".join(HEAD + sorted(intrinsics) + decls) + "\n\n")
        f.write("\n\n".join(bodies))
        f.write("\n")


def _write_table(out, starts):
    names = sorted(starts)
    with open(os.path.join(out, "table.ll"), "w") as f:
        f.write("\n".join(f"declare void {fname(a)}(ptr)" for a in names) + "\n")
        f.write(f"@__recomp_table = constant [{len(names)} x {{ i64, ptr }}] [\n")
        f.write(",\n".join(f"  {{ i64, ptr }} {{ i64 {sconst(a, 64)}, ptr {fname(a)} }}" for a in names))
        f.write("\n]\n")
        f.write(f"@__recomp_table_size = constant i64 {len(names)}\n")


def _extra_functions(img, starts, workers, shard_size):
    """Phase 1: direct call targets that have no FDE and are not import stubs (real code the
    unwind table does not describe). They are lifted as functions of unknown extent."""
    from .image import Function
    addrs = sorted(starts)
    jobs = [(i, addrs[k:k + shard_size]) for i, k in enumerate(range(0, len(addrs), shard_size))]
    ctx = mp.get_context("fork")
    with ctx.Pool(workers, initializer=_init, initargs=(img, starts, dict(out=None, tso=False, abi=False, cfg_only=True))) as pool:
        results = pool.map(_work, jobs, chunksize=1)
    extra = set()
    for r in results:
        extra.update(t for t in r["extra_calls"] if img.is_code(t))
    for t in extra:
        img.functions.setdefault(t, Function(t, 0, f"sub_{t:x}"))
    return extra


def run(img, out=None, shard_size=2000, workers=None, tso=False, abi=False, limit=None):
    workers = workers or os.cpu_count()
    starts = frozenset(a for a, f in img.functions.items() if img.is_code(a))
    if limit:
        starts = frozenset(sorted(starts)[:limit])
    t_extra = time.time()
    extra = _extra_functions(img, starts, workers, shard_size)
    starts = starts | frozenset(extra)
    addrs = sorted(starts)
    jobs = [(i, addrs[k:k + shard_size]) for i, k in enumerate(range(0, len(addrs), shard_size))]
    opts = dict(out=out, tso=tso, abi=abi)
    if out:
        os.makedirs(out, exist_ok=True)
        _write_table(out, starts)
    t0 = time.time()
    ctx = mp.get_context("fork")
    with ctx.Pool(workers, initializer=_init, initargs=(img, starts, opts)) as pool:
        results = pool.map(_work, jobs, chunksize=1)
    total = dict(stats=collections.Counter(), unsupported=collections.Counter(), cfg_errors=collections.Counter(),
                 mnemonics=collections.Counter(), extra_calls=collections.Counter(), unsupported_at={},
                 cfg_error_at={})
    for r in results:
        for k in ("stats", "unsupported", "cfg_errors", "mnemonics", "extra_calls"):
            total[k].update(r[k])
        for k in ("unsupported_at", "cfg_error_at"):
            for kk, v in r[k].items():
                total[k].setdefault(kk, v)
    total["wall seconds"] = time.time() - t0
    total["phase 1 seconds"] = t0 - t_extra
    total["shards"] = len(jobs)
    total["functions without an FDE (lifted)"] = len(extra)
    return total


def report(total, path=None):
    st = total["stats"]
    insns = st["instructions"]
    unsup_insns = sum(total["unsupported"].values())
    lines = [
        f"functions {st['functions']} ({st['functions without code']} without code), blocks {st['blocks']}, "
        f"instructions {insns}",
        f"lifted in {total['wall seconds']:.0f} s wall ({st['seconds']:.0f} s CPU), {total['shards']} shards, "
        f"IR {st['ir bytes'] / 2**30:.2f} GiB",
        f"calls: {st['direct calls']} direct, {st['indirect calls']} indirect; indirect jumps: "
        f"{st['indirect jumps: jump table']} jump tables, {st['indirect jumps: dispatcher']} left to the dispatcher",
        f"direct calls to import stubs (resolved through their GOT slot): {st['direct calls to import stubs']}",
        f"functions without an FDE, found from direct calls and lifted: {total['functions without an FDE (lifted)']}; "
        f"direct calls still unresolved: {sum(total['extra_calls'].values())}",
        f"instruction sites lifted as traps (unsupported): {unsup_insns}",
    ]
    for k, v in total["unsupported"].most_common(40):
        lines.append(f"  unsupported {k}: {v} (e.g. {total['unsupported_at'].get(k, 0):#x})")
    for k, v in total["cfg_errors"].most_common(20):
        lines.append(f"  cfg {k}: {v} (e.g. {total['cfg_error_at'].get(k, 0):#x})")
    text = "\n".join(lines)
    if path:
        js = {k: (dict(v) if isinstance(v, collections.Counter) else v) for k, v in total.items()}
        js["mnemonics"] = {X_MNEMONIC_NAMES.get(k, str(k)): v for k, v in total["mnemonics"].most_common()}
        js["extra_calls"] = {hex(k): v for k, v in total["extra_calls"].most_common(200)}
        js["unsupported_at"] = {k: hex(v) for k, v in total["unsupported_at"].items()}
        js["cfg_error_at"] = {k: hex(v) for k, v in total["cfg_error_at"].items()}
        with open(path, "w") as f:
            json.dump(js, f, indent=1)
    return text
