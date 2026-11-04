# SPDX-License-Identifier: GPL-2.0-or-later
"""Jump tables behind `jmp reg`.

Recognised form (GCC and Clang, position-independent code; the PS4 toolchain is Clang):

    lea    base, [rip + T]                 (may be hoisted out of a loop)
    ...    idx bounded by `cmp idx, N-1; ja` / `and idx, 2^k-1` / bextr / movzx
    movsxd tgt, dword [base + idx*4]
    add    tgt, base
    jmp    tgt

Definitions are found by walking backwards over the instructions decoded so far (fall-through and
direct-branch predecessors), so a hoisted `lea` is found too. Without a recognisable bound, the
table is read while its entries land inside the function (needs the function's end, which the
game's FDEs give).

Only the set of targets matters: the lifter switches on the computed target address itself.
"""
from dataclasses import dataclass
import struct

import iced_x86 as X

M = X.Mnemonic
OK = X.OpKind
IMMS = (OK.IMMEDIATE8, OK.IMMEDIATE8TO32, OK.IMMEDIATE8TO64, OK.IMMEDIATE32, OK.IMMEDIATE32TO64, OK.IMMEDIATE16)
MAX_ENTRIES = 4096


@dataclass
class Table:
    address: int
    count: int
    targets: list


def _full(reg):
    return X.RegisterInfo(reg).full_register


def _preds(insns):
    p = {}
    for i in insns.values():
        fc = i.flow_control
        if fc not in (X.FlowControl.UNCONDITIONAL_BRANCH, X.FlowControl.RETURN, X.FlowControl.INDIRECT_BRANCH):
            p.setdefault(i.next_ip, []).append(i)
        if fc in (X.FlowControl.UNCONDITIONAL_BRANCH, X.FlowControl.CONDITIONAL_BRANCH):
            p.setdefault(i.near_branch_target, []).append(i)
    return p


def _writes(ins, full):
    """Does ins write the full register `full` (any width)?"""
    if ins.flow_control in (X.FlowControl.CALL, X.FlowControl.INDIRECT_CALL):
        return full in (X.Register.RAX, X.Register.RCX, X.Register.RDX, X.Register.RSI, X.Register.RDI,
                        X.Register.R8, X.Register.R9, X.Register.R10, X.Register.R11)
    if ins.op_count and ins.op0_kind == OK.REGISTER and _full(ins.op0_register) == full:
        return ins.mnemonic not in (M.CMP, M.TEST, M.BT, M.PUSH)
    if ins.mnemonic in (M.XCHG, M.XADD) and ins.op_count > 1 and ins.op1_kind == OK.REGISTER \
            and _full(ins.op1_register) == full:
        return True
    if ins.mnemonic in (M.MUL, M.IMUL, M.DIV, M.IDIV, M.CQO, M.CDQ) and ins.op_count <= 1:
        return full in (X.Register.RAX, X.Register.RDX)
    return False


def _defs(preds, start, full, limit=200):
    """The instructions that last wrote `full` on each path reaching `start` (exclusive)."""
    found, seen, work = [], set(), [start]
    steps = 0
    while work and steps < limit:
        cur = work.pop()
        for p in preds.get(cur.ip, []):
            if p.ip in seen:
                continue
            seen.add(p.ip)
            steps += 1
            if _writes(p, full):
                found.append(p)
            else:
                work.append(p)
    return found if steps < limit else None


def _bound(preds, load, idx_full):
    """Number of table entries implied by the index's definition or a dominating `cmp; ja`."""
    # 1. a guard `cmp idx, N; ja/jae default` on the path (idx or the register it was copied from)
    regs = {idx_full}
    cur = load
    for _ in range(40):
        ps = [p for p in preds.get(cur.ip, []) if p.next_ip == cur.ip or p.flow_control == X.FlowControl.CONDITIONAL_BRANCH]
        if len(ps) != 1:
            break
        p = ps[0]
        if p.mnemonic in (M.JA, M.JAE) and p.next_ip == cur.ip:
            ps2 = [q for q in preds.get(p.ip, []) if q.next_ip == p.ip]
            if ps2 and ps2[0].mnemonic == M.CMP and ps2[0].op0_kind == OK.REGISTER \
                    and _full(ps2[0].op0_register) in regs and ps2[0].op1_kind in IMMS:
                n = ps2[0].immediate(1) & 0xffffffff
                return n + 1 if p.mnemonic == M.JA else n
        if p.op_count and p.op0_kind == OK.REGISTER and _full(p.op0_register) in regs:
            if p.mnemonic == M.AND and p.op1_kind in IMMS:
                m = p.immediate(1) & 0xffffffff
                if m and (m & (m + 1)) == 0:
                    return m + 1
            if p.mnemonic == M.MOVZX:
                byte = (X.MemorySizeInfo(p.memory_size).size if p.op1_kind == OK.MEMORY
                        else X.RegisterInfo(p.op1_register).size) == 1
                if byte:
                    return 256
            if p.mnemonic == M.BEXTR:
                return None  # length operand is a register: unknown statically here
            if p.mnemonic == M.MOV and p.op1_kind == OK.REGISTER:
                regs.add(_full(p.op1_register))
                cur = p
                continue
            if p.mnemonic not in (M.MOV, M.MOVSXD, M.MOVZX):
                break
        cur = p
    return None


def match(image, insns, jmp, fn_start=0, fn_end=0):
    if jmp.op_count != 1 or jmp.op0_kind != OK.REGISTER:
        return None
    preds = _preds(insns)
    tgt = _full(jmp.op0_register)
    adds = _defs(preds, jmp, tgt)
    if not adds or len(adds) != 1:
        return None
    add = adds[0]
    if add.mnemonic != M.ADD or add.op1_kind != OK.REGISTER:
        return None
    base = _full(add.op1_register)
    loads = _defs(preds, add, tgt)
    if not loads or len(loads) != 1:
        return None
    load = loads[0]
    if load.mnemonic != M.MOVSXD or load.op1_kind != OK.MEMORY or load.memory_index_scale != 4 \
            or _full(load.memory_base) != base or load.memory_displacement:
        return None
    leas = _defs(preds, load, base)
    if not leas or {(l.mnemonic, l.is_ip_rel_memory_operand and l.ip_rel_memory_address) for l in leas} != \
            {(M.LEA, leas[0].ip_rel_memory_address)} or not leas[0].is_ip_rel_memory_operand:
        return None
    table = leas[0].ip_rel_memory_address
    count = _bound(preds, load, _full(load.memory_index))
    if count is not None and 0 < count <= MAX_ENTRIES:
        raw = image.read(table, 4 * count)
        if raw is None:
            return None
        targets = [table + v for v in struct.unpack(f"<{count}i", raw)]
        if not all(image.is_code(t) for t in targets):
            return None
        return Table(table, count, targets)
    # no bound: entries while they land inside the function
    if not fn_end:
        return None
    targets = []
    while len(targets) < MAX_ENTRIES:
        raw = image.read(table + 4 * len(targets), 4)
        if raw is None:
            break
        t = table + struct.unpack("<i", raw)[0]
        if not fn_start <= t < fn_end:
            break
        targets.append(t)
    if not targets:
        return None
    return Table(table, len(targets), targets)
