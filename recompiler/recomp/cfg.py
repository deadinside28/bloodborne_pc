# SPDX-License-Identifier: GPL-2.0-or-later
"""Control-flow recovery for one function: recursive descent from its entry.

A direct jump to an address outside the function's range, or to another function's entry, is a
tail call. Indirect jumps are resolved as jump tables when the pattern is recognised
(jumptable.py); otherwise they leave the function through the dispatcher.
"""
from dataclasses import dataclass, field

import iced_x86 as X

from . import jumptable

FC = X.FlowControl


@dataclass
class Block:
    start: int
    insns: list = field(default_factory=list)
    succs: list = field(default_factory=list)  # intra-function successor addresses
    table: object = None  # jumptable.Table for an indirect jmp resolved as a switch


@dataclass
class FunctionCFG:
    start: int
    end: int
    blocks: dict = field(default_factory=dict)  # start -> Block
    errors: list = field(default_factory=list)
    tail_targets: set = field(default_factory=set)
    call_targets: set = field(default_factory=set)
    noreturn_calls: set = field(default_factory=set)  # calls followed by bytes that do not decode


def decode_at(image, addr):
    data = image.code_window(addr, 16)
    if not data:
        return None
    ins = X.Decoder(64, data, ip=addr).decode()
    if ins.is_invalid:
        return None
    return ins


def recover(image, fn, function_starts, noreturn=None, _limits=None, _noret=frozenset()):
    """noreturn(target) -> True for a direct call target that never returns (the path ends).
    _limits: {jmp address: entries} for tables cut short (see the end of pass 1); _noret: calls
    found not to return (a decode failure further down their straight-line path)."""
    cfg = FunctionCFG(fn.start, fn.end)
    _limits = _limits or {}
    noreturn = noreturn or (lambda t: False)
    new_noret = set()

    def inside(a):
        if a != fn.start and a in function_starts:
            return False
        if fn.end:
            return fn.start <= a < fn.end
        return image.is_code(a)

    # Pass 1: find instruction addresses and block leaders.
    insns = {}
    leaders = {fn.start}
    work = [fn.start]
    tables = {}
    while work:
        a = work.pop()
        last_call = None  # the last direct call on this straight-line path
        while a not in insns:
            ins = decode_at(image, a)
            if ins is None:
                if last_call is not None:
                    # The path ran from a call into bytes that end in an invalid instruction:
                    # data after a call that never returns, decoded until it broke (xchg, sti,
                    # out, std...). The call is taken as noreturn and the function recovered again.
                    new_noret.add(last_call)
                else:
                    cfg.errors.append((a, "decode failure"))
                break
            insns[a] = ins
            fc = ins.flow_control
            nxt = ins.next_ip
            if fc == FC.NEXT or fc == FC.INTERRUPT and ins.mnemonic != X.Mnemonic.INT3 \
                    or fc == FC.XBEGIN_XABORT_XEND:
                a = nxt
                if not inside(a):
                    if not fn.end or a != fn.end:
                        cfg.errors.append((ins.ip, "falls out of function"))
                    cfg.tail_targets.add(a)
                    break
                continue
            if fc in (FC.CALL, FC.INDIRECT_CALL):
                if fc == FC.CALL:
                    cfg.call_targets.add(ins.near_branch_target)
                    if noreturn(ins.near_branch_target):
                        break
                    if ins.ip in _noret:
                        cfg.noreturn_calls.add(ins.ip)
                        break
                    last_call = ins.ip
                    if inside(nxt) and decode_at(image, nxt) is None:
                        # What follows the call does not decode (padding or data): the callee
                        # never returns here (if it did, the original would run into the same
                        # bytes). A function the import tables do not mark noreturn (0x13b1960).
                        cfg.noreturn_calls.add(ins.ip)
                        break
                a = nxt
                if not inside(a):
                    cfg.tail_targets.add(a)
                    break
                continue
            if fc == FC.CONDITIONAL_BRANCH:
                t = ins.near_branch_target
                for s in (t, nxt):
                    if inside(s):
                        leaders.add(s)
                        work.append(s)
                    else:
                        cfg.tail_targets.add(s)
                break
            if fc == FC.UNCONDITIONAL_BRANCH:
                t = ins.near_branch_target
                if inside(t):
                    leaders.add(t)
                    work.append(t)
                else:
                    cfg.tail_targets.add(t)
                break
            if fc == FC.INDIRECT_BRANCH:
                tbl = jumptable.match(image, insns, ins, fn.start, fn.end)
                if tbl is not None and ins.ip in _limits:
                    tbl.targets = tbl.targets[:_limits[ins.ip]]
                    tbl.count = len(tbl.targets)
                if tbl is not None:
                    targets = [t for t in tbl.targets if inside(t)]
                    if len(targets) == len(tbl.targets):
                        tables[ins.ip] = tbl
                        for t in set(targets):
                            leaders.add(t)
                            work.append(t)
                break
            # RETURN, INTERRUPT (int3), EXCEPTION (ud2): end of the path
            break

    # A table bounded only by an index mask (and rcx, 1Fh) may run into the next table of the
    # function: entries past that table's start are its entries added to the wrong base, cases
    # the program never takes, and their "targets" fall into the middle of instructions
    # (0x2f06e0). Cut there and recover again without them; a computed target outside the
    # table's set still goes to the dispatcher at run time.
    bases = {t.address for t in tables.values()}
    cut = {}
    for jmp, t in tables.items():
        inner = [b for b in bases if t.address < b < t.address + 4 * len(t.targets)]
        if inner and jmp not in _limits:
            cut[jmp] = (min(inner) - t.address) // 4
    if cut or new_noret:
        return recover(image, fn, function_starts, noreturn, {**_limits, **cut}, _noret | new_noret)

    # Pass 2: blocks.
    for lead in sorted(leaders):
        if lead not in insns:
            continue
        b = Block(lead)
        a = lead
        while a in insns:
            ins = insns[a]
            b.insns.append(ins)
            fc = ins.flow_control
            nxt = ins.next_ip
            if fc == FC.CONDITIONAL_BRANCH:
                b.succs = [s for s in (ins.near_branch_target, nxt) if inside(s)]
                break
            if fc == FC.UNCONDITIONAL_BRANCH:
                t = ins.near_branch_target
                b.succs = [t] if inside(t) else []
                break
            if fc == FC.INDIRECT_BRANCH:
                if a in tables:
                    b.table = tables[a]
                    b.succs = sorted(set(b.table.targets))
                break
            if fc in (FC.RETURN, FC.EXCEPTION) or fc == FC.INTERRUPT and ins.mnemonic == X.Mnemonic.INT3:
                break
            if fc == FC.CALL and (noreturn(ins.near_branch_target) or ins.ip in cfg.noreturn_calls):
                break
            if nxt in leaders:
                if inside(nxt):
                    b.succs = [nxt]
                break
            if nxt not in insns:
                break  # falls out (tail) or decode failure: lifter emits the exit
            a = nxt
        cfg.blocks[lead] = b
    return cfg
