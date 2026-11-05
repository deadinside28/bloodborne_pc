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


def _preds(insns, tables=None):
    """Predecessors: fall-through, direct branches, and the jump tables resolved so far (a block
    reached only through a table: 0x210e5c0's switches inside switches, their base registers
    loaded once at the entry)."""
    p = {}
    for jmp, tbl in (tables or {}).items():
        if jmp in insns:
            for t in set(tbl.targets):
                p.setdefault(t, []).append(insns[jmp])
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


def _defs(preds, start, full, limit=2000):
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
        # guard taken towards the table: `cmp idx, N; jbe/jb table` (N+1 / N entries)
        if p.mnemonic in (M.JBE, M.JB) and p.near_branch_target == cur.ip and p.next_ip != cur.ip:
            ps2 = [q for q in preds.get(p.ip, []) if q.next_ip == p.ip]
            if ps2 and ps2[0].mnemonic == M.CMP and ps2[0].op0_kind == OK.REGISTER \
                    and _full(ps2[0].op0_register) in regs and ps2[0].op1_kind in IMMS:
                n = ps2[0].immediate(1) & 0xffffffff
                return n + 1 if p.mnemonic == M.JBE else n
        # guard falling through to the table: `cmp idx, N; ja/jae default`
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


_bases_cache = {}


def _table_bases(image, fn_start, fn_end):
    """Every RIP-relative lea target in a linear sweep of the function: candidate table bases.
    (A sweep also decodes data in the function, but real leas are found; extra ones only make the
    scan stop earlier.)"""
    key = (fn_start, fn_end)
    if key not in _bases_cache:
        if len(_bases_cache) > 4096:
            _bases_cache.clear()
        data = image.read(fn_start, fn_end - fn_start) or b""
        bases = set()
        for i in X.Decoder(64, data, ip=fn_start):
            if i.mnemonic == M.LEA and i.is_ip_rel_memory_operand:
                bases.add(i.ip_rel_memory_address)
        _bases_cache[key] = bases
    return _bases_cache[key]


def _through_copies(preds, at, reg, depth=4):
    """The definitions of reg reaching `at`, following `mov reg, other` copies back."""
    out = []
    for d in _defs(preds, at, reg) or []:
        if depth and d.mnemonic == M.MOV and d.op1_kind == OK.REGISTER and X.RegisterInfo(d.op1_register).size == 8:
            more = _through_copies(preds, d, _full(d.op1_register), depth - 1)
            if not more:
                return []
            out += more
        else:
            out.append(d)
    return out


def match(image, insns, jmp, fn_start=0, fn_end=0, tables=None):
    if jmp.op_count != 1 or jmp.op0_kind != OK.REGISTER:
        return None
    preds = _preds(insns, tables)
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
    leas = _through_copies(preds, load, base)
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
        if all(image.is_code(t) and (not fn_end or fn_start <= t < fn_end) for t in targets):
            return Table(table, count, targets)
        # The bound found is an upper bound (a mask, `movzx` of a byte) looser than the switch's
        # own guard, which tested a separately computed copy of the index: read at most that many
        # entries, as the scan below does.
        limit = count
    else:
        limit = MAX_ENTRIES
    # entries while they land inside the function, stopping at the next table
    if not fn_end:
        return None
    others = sorted(b for b in _table_bases(image, fn_start, fn_end) if b > table)
    stop = others[0] if others else None
    targets = []
    while len(targets) < limit:
        if stop is not None and table + 4 * len(targets) >= stop:
            break
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
