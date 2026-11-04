# SPDX-License-Identifier: GPL-2.0-or-later
"""SSE/SSE2/SSSE3/SSE4/AVX semantics on LLVM vector types.

Registered into lift.H on import. Operands follow x86 forms: legacy SSE `op dst, src`
(dst = dst OP src, bits 128..255 of the ymm kept) and VEX `vop dst, src1, src2` (upper bits
zeroed for 128-bit forms). LLVM lowers the vector IR to SSE/AVX on x86-64 and to NEON on AArch64
(256-bit vectors are split in two).

Float semantics follow x86 where it is defined: min/max return the second operand when either is
NaN or both are zero; float->int conversions return the "integer indefinite" value
(0x80000000...) on NaN or overflow. rcp/rsqrt (implementation-defined approximations on x86)
are computed exactly.
"""
import iced_x86 as X

from .lift import H, Unsupported, sconst, xmm_index, OK, R

M = X.Mnemonic


def sem(*ms):
    def deco(fn):
        for m in ms:
            H[m] = fn
        return fn
    return deco


def both(*names):
    """Mnemonics for the legacy and VEX forms: both('PADDD') -> [PADDD, VPADDD]."""
    out = []
    for n in names:
        for v in (n, "V" + n):
            if hasattr(M, v):
                out.append(getattr(M, v))
    return out


def ety(ebits, fp):
    if fp:
        return {32: "float", 64: "double"}[ebits]
    return f"i{ebits}"


def vty(bits, ebits, fp=False):
    return f"<{bits // ebits} x {ety(ebits, fp)}>"


def is_vex(ins):
    return ins.encoding in (X.EncodingKind.VEX, X.EncodingKind.EVEX)


def dst_bits(L, ins):
    return L.op_bits(ins, 0)


def src(L, ins, i, bits):
    """Operand i as i<bits> (register truncated/extended, memory loaded at bits)."""
    k = ins.op_kind(i)
    if k == OK.MEMORY:
        return L.load(L.ea(ins), bits, L.stack_relative(ins))
    if k == OK.REGISTER:
        v, b = L.read_reg(ins.op_register(i))
        return L.resize(v, b, bits) if b != bits else v
    return sconst(ins.immediate(i), bits)


def srcs(L, ins, bits):
    """(first, second) source operands for the 2-operand legacy / 3-operand VEX forms."""
    if is_vex(ins) and ins.op_count >= 3 and ins.op_kind(2) != OK.IMMEDIATE8:
        return src(L, ins, 1, bits), src(L, ins, 2, bits), 3
    if is_vex(ins) and ins.op_count >= 4:
        return src(L, ins, 1, bits), src(L, ins, 2, bits), 3
    return src(L, ins, 0, bits), src(L, ins, 1, bits), 2


def tov(L, v, bits, ebits, fp=False):
    return L.op(f"bitcast i{bits} {v} to {vty(bits, ebits, fp)}")


def fromv(L, v, bits, ebits, fp=False):
    return L.op(f"bitcast {vty(bits, ebits, fp)} {v} to i{bits}")


def wdst(L, ins, v, bits):
    L.write(ins, 0, v, bits)


def intrinsic(L, decl):
    L.mod.intrinsics.add(decl)


def vsuffix(bits, ebits, fp=False):
    return f"v{bits // ebits}{'f' if fp else 'i'}{ebits}"


# ---- integer lane-wise binary ops ---------------------------------------------------------------

INT_BIN = {}
for _n, _e in (("B", 8), ("W", 16), ("D", 32), ("Q", 64)):
    INT_BIN["PADD" + _n] = ("add", _e)
    INT_BIN["PSUB" + _n] = ("sub", _e)
    INT_BIN["PCMPEQ" + _n] = ("eq", _e)
    INT_BIN["PCMPGT" + _n] = ("sgt", _e)
INT_BIN.update({"PMULLW": ("mul", 16), "PMULLD": ("mul", 32),
                "PADDSB": ("sadd.sat", 8), "PADDSW": ("sadd.sat", 16), "PADDUSB": ("uadd.sat", 8), "PADDUSW": ("uadd.sat", 16),
                "PSUBSB": ("ssub.sat", 8), "PSUBSW": ("ssub.sat", 16), "PSUBUSB": ("usub.sat", 8), "PSUBUSW": ("usub.sat", 16),
                "PMINSB": ("smin", 8), "PMINSW": ("smin", 16), "PMINSD": ("smin", 32),
                "PMINUB": ("umin", 8), "PMINUW": ("umin", 16), "PMINUD": ("umin", 32),
                "PMAXSB": ("smax", 8), "PMAXSW": ("smax", 16), "PMAXSD": ("smax", 32),
                "PMAXUB": ("umax", 8), "PMAXUW": ("umax", 16), "PMAXUD": ("umax", 32),
                "PAVGB": ("avg", 8), "PAVGW": ("avg", 16),
                "PMULHW": ("mulhs", 16), "PMULHUW": ("mulhu", 16),
                "PSIGNB": ("sign", 8), "PSIGNW": ("sign", 16), "PSIGND": ("sign", 32)})
INT_BIN_M = {}
for _k, _v in INT_BIN.items():
    for _m in both(_k):
        INT_BIN_M[_m] = _v


@sem(*INT_BIN_M)
def _int_bin(L, ins, b):
    kind, e = INT_BIN_M[ins.mnemonic]
    bits = dst_bits(L, ins)
    x, y, _ = srcs(L, ins, bits)
    vt = vty(bits, e)
    a, c = tov(L, x, bits, e), tov(L, y, bits, e)
    if kind in ("add", "sub", "mul"):
        r = L.op(f"{kind} {vt} {a}, {c}")
    elif kind in ("eq", "sgt"):
        m = L.op(f"icmp {kind} {vt} {a}, {c}")
        r = L.op(f"sext <{bits // e} x i1> {m} to {vt}")
    elif kind in ("sadd.sat", "uadd.sat", "ssub.sat", "usub.sat", "smin", "smax", "umin", "umax"):
        s = vsuffix(bits, e)
        intrinsic(L, f"declare {vt} @llvm.{kind}.{s}({vt}, {vt})")
        r = L.op(f"call {vt} @llvm.{kind}.{s}({vt} {a}, {vt} {c})")
    elif kind == "avg":
        wt = vty(bits * 2, e * 2)
        za = L.op(f"zext {vt} {a} to {wt}")
        zc = L.op(f"zext {vt} {c} to {wt}")
        s = L.op(f"add {wt} {za}, {zc}")
        ones = ", ".join([f"i{e * 2} 1"] * (bits // e))
        s1 = L.op(f"add {wt} {s}, <{ones}>")
        sh = L.op(f"lshr {wt} {s1}, <{ones}>")
        r = L.op(f"trunc {wt} {sh} to {vt}")
    elif kind in ("mulhs", "mulhu"):
        wt = vty(bits * 2, e * 2)
        ext = "sext" if kind == "mulhs" else "zext"
        za = L.op(f"{ext} {vt} {a} to {wt}")
        zc = L.op(f"{ext} {vt} {c} to {wt}")
        p = L.op(f"mul {wt} {za}, {zc}")
        sh = ", ".join([f"i{e * 2} {e}"] * (bits // e))
        hs = L.op(f"lshr {wt} {p}, <{sh}>")
        r = L.op(f"trunc {wt} {hs} to {vt}")
    elif kind == "sign":
        zero = "zeroinitializer"
        neg = L.op(f"sub {vt} {zero}, {a}")
        lt = L.op(f"icmp slt {vt} {c}, {zero}")
        eq = L.op(f"icmp eq {vt} {c}, {zero}")
        t = L.op(f"select <{bits // e} x i1> {lt}, {vt} {neg}, {vt} {a}")
        r = L.op(f"select <{bits // e} x i1> {eq}, {vt} {zero}, {vt} {t}")
    else:
        raise Unsupported(kind)
    wdst(L, ins, fromv(L, r, bits, e), bits)


@sem(*both("PMULUDQ", "PMULDQ"))
def _pmuludq(L, ins, b):
    bits = dst_bits(L, ins)
    x, y, _ = srcs(L, ins, bits)
    signed = ins.mnemonic in (M.PMULDQ, M.VPMULDQ)
    a, c = tov(L, x, bits, 64), tov(L, y, bits, 64)
    vt = vty(bits, 64)
    if signed:
        sh = ", ".join(["i64 32"] * (bits // 64))
        a = L.op(f"ashr {vt} {L.op(f'shl {vt} {a}, <{sh}>')}, <{sh}>")
        c = L.op(f"ashr {vt} {L.op(f'shl {vt} {c}, <{sh}>')}, <{sh}>")
    else:
        mk = ", ".join(["i64 4294967295"] * (bits // 64))
        a = L.op(f"and {vt} {a}, <{mk}>")
        c = L.op(f"and {vt} {c}, <{mk}>")
    wdst(L, ins, fromv(L, L.op(f"mul {vt} {a}, {c}"), bits, 64), bits)


@sem(*both("PMADDWD"))
def _pmaddwd(L, ins, b):
    bits = dst_bits(L, ins)
    x, y, _ = srcs(L, ins, bits)
    n = bits // 16
    a = L.op(f"sext {vty(bits, 16)} {tov(L, x, bits, 16)} to <{n} x i32>")
    c = L.op(f"sext {vty(bits, 16)} {tov(L, y, bits, 16)} to <{n} x i32>")
    p = L.op(f"mul <{n} x i32> {a}, {c}")
    ev = ", ".join(f"i32 {2 * i}" for i in range(n // 2))
    od = ", ".join(f"i32 {2 * i + 1}" for i in range(n // 2))
    pe = L.op(f"shufflevector <{n} x i32> {p}, <{n} x i32> poison, <{n // 2} x i32> <{ev}>")
    po = L.op(f"shufflevector <{n} x i32> {p}, <{n} x i32> poison, <{n // 2} x i32> <{od}>")
    r = L.op(f"add <{n // 2} x i32> {pe}, {po}")
    wdst(L, ins, fromv(L, r, bits, 32), bits)


@sem(*both("PSADBW"))
def _psadbw(L, ins, b):
    bits = dst_bits(L, ins)
    x, y, _ = srcs(L, ins, bits)
    n = bits // 8
    a = L.op(f"zext <{n} x i8> {tov(L, x, bits, 8)} to <{n} x i16>")
    c = L.op(f"zext <{n} x i8> {tov(L, y, bits, 8)} to <{n} x i16>")
    d = L.op(f"sub <{n} x i16> {a}, {c}")
    intrinsic(L, f"declare <{n} x i16> @llvm.abs.v{n}i16(<{n} x i16>, i1)")
    ad = L.op(f"call <{n} x i16> @llvm.abs.v{n}i16(<{n} x i16> {d}, i1 false)")
    out = "0"
    lanes = []
    for q in range(bits // 64):
        s = None
        for i in range(8):
            el = L.op(f"extractelement <{n} x i16> {ad}, i32 {q * 8 + i}")
            s = el if s is None else L.op(f"add i16 {s}, {el}")
        lanes.append(L.op(f"zext i16 {s} to i{bits}"))
    for q, v in enumerate(lanes):
        sh = L.op(f"shl i{bits} {v}, {q * 64}") if q else v
        out = L.op(f"or i{bits} {out}, {sh}")
    wdst(L, ins, out, bits)


# ---- shifts ---------------------------------------------------------------------------------------

SHIFTS = {}
for _k, _op, _e in (("PSLLW", "shl", 16), ("PSLLD", "shl", 32), ("PSLLQ", "shl", 64),
                    ("PSRLW", "lshr", 16), ("PSRLD", "lshr", 32), ("PSRLQ", "lshr", 64),
                    ("PSRAW", "ashr", 16), ("PSRAD", "ashr", 32)):
    for _m in both(_k):
        SHIFTS[_m] = (_op, _e)


@sem(*SHIFTS)
def _shift(L, ins, b):
    op, e = SHIFTS[ins.mnemonic]
    bits = dst_bits(L, ins)
    last = ins.op_count - 1
    if ins.op_kind(last) == OK.IMMEDIATE8:
        cnt = ins.immediate(last) & 0xff
        cnt64 = str(cnt)
    else:
        cnt64 = src(L, ins, last, 64)
    a_i = 1 if is_vex(ins) and ins.op_count == 3 else 0
    x = src(L, ins, a_i, bits)
    vt = vty(bits, e)
    n = bits // e
    a = tov(L, x, bits, e)
    # counts >= element width: 0 (logical) or sign fill (arithmetic)
    big = L.op(f"icmp ugt i64 {cnt64}, {e - 1}")
    c = L.op(f"select i1 {big}, i64 {e - 1 if op == 'ashr' else 0}, i64 {cnt64}")
    ce = L.resize(c, 64, e) if e != 64 else c
    splat_i = L.op(f"insertelement {vt} poison, i{e} {ce}, i32 0")
    splat = L.op(f"shufflevector {vt} {splat_i}, {vt} poison, <{n} x i32> zeroinitializer")
    r = L.op(f"{op} {vt} {a}, {splat}")
    if op != "ashr":
        r = L.op(f"select i1 {big}, {vt} zeroinitializer, {vt} {r}")
    wdst(L, ins, fromv(L, r, bits, e), bits)


@sem(*both("PSLLDQ", "PSRLDQ"))
def _byteshift(L, ins, b):
    bits = dst_bits(L, ins)
    a_i = 1 if is_vex(ins) and ins.op_count == 3 else 0
    x = src(L, ins, a_i, bits)
    k = min(ins.immediate(ins.op_count - 1) & 0xff, 16)
    left = ins.mnemonic in (M.PSLLDQ, M.VPSLLDQ)
    out = "0"
    for lane in range(bits // 128):
        part = L.op(f"lshr i{bits} {x}, {lane * 128}") if lane else x
        p = L.resize(part, bits, 128) if bits != 128 else part
        s = L.op(f"{'shl' if left else 'lshr'} i128 {p}, {8 * k}") if k < 16 else "0"
        z = L.resize(s, 128, bits) if bits != 128 else s
        out = L.op(f"or i{bits} {out}, {L.op(f'shl i{bits} {z}, {lane * 128}') if lane else z}")
    wdst(L, ins, out, bits)


# ---- shuffles -------------------------------------------------------------------------------------

def shuffle(L, a, c, bits, e, idx):
    n = bits // e
    vt = vty(bits, e)
    mask = ", ".join(f"i32 {i}" if i is not None else "i32 poison" for i in idx)
    return L.op(f"shufflevector {vt} {a}, {vt} {c}, <{len(idx)} x i32> <{mask}>")


@sem(*both("PSHUFD"), *both("PSHUFLW"), *both("PSHUFHW"))
def _pshufd(L, ins, b):
    bits = dst_bits(L, ins)
    x = src(L, ins, 1, bits)
    imm = ins.immediate(2)
    mn = ins.mnemonic
    if mn in (M.PSHUFD, M.VPSHUFD):
        e, idx = 32, []
        for lane in range(bits // 128):
            idx += [lane * 4 + ((imm >> (2 * i)) & 3) for i in range(4)]
    else:
        e, idx = 16, []
        hi = mn in (M.PSHUFHW, M.VPSHUFHW)
        for lane in range(bits // 128):
            base = lane * 8
            lo = [base + i for i in range(4)]
            sh = [base + (4 if hi else 0) + ((imm >> (2 * i)) & 3) for i in range(4)]
            idx += (lo + sh) if hi else (sh + [base + 4 + i for i in range(4)])
    a = tov(L, x, bits, e)
    wdst(L, ins, fromv(L, shuffle(L, a, a, bits, e, idx), bits, e), bits)


UNPCK = {}
for _k, _e, _hi in (("PUNPCKLBW", 8, 0), ("PUNPCKHBW", 8, 1), ("PUNPCKLWD", 16, 0), ("PUNPCKHWD", 16, 1),
                    ("PUNPCKLDQ", 32, 0), ("PUNPCKHDQ", 32, 1), ("PUNPCKLQDQ", 64, 0), ("PUNPCKHQDQ", 64, 1),
                    ("UNPCKLPS", 32, 0), ("UNPCKHPS", 32, 1), ("UNPCKLPD", 64, 0), ("UNPCKHPD", 64, 1)):
    for _m in both(_k):
        UNPCK[_m] = (_e, _hi)


@sem(*UNPCK)
def _unpck(L, ins, b):
    e, hi = UNPCK[ins.mnemonic]
    bits = dst_bits(L, ins)
    x, y, _ = srcs(L, ins, bits)
    per = 128 // e
    n = bits // e
    idx = []
    for lane in range(bits // 128):
        base = lane * per + (per // 2 if hi else 0)
        for i in range(per // 2):
            idx += [base + i, n + base + i]
    a, c = tov(L, x, bits, e), tov(L, y, bits, e)
    wdst(L, ins, fromv(L, shuffle(L, a, c, bits, e, idx), bits, e), bits)


@sem(*both("SHUFPS"), *both("SHUFPD"))
def _shufps(L, ins, b):
    bits = dst_bits(L, ins)
    x, y, nxt = srcs(L, ins, bits)
    imm = ins.immediate(ins.op_count - 1)
    pd = ins.mnemonic in (M.SHUFPD, M.VSHUFPD)
    e = 64 if pd else 32
    n = bits // e
    idx = []
    for lane in range(bits // 128):
        if pd:
            idx += [lane * 2 + ((imm >> (lane * 2)) & 1), n + lane * 2 + ((imm >> (lane * 2 + 1)) & 1)]
        else:
            base = lane * 4
            idx += [base + ((imm >> 0) & 3), base + ((imm >> 2) & 3),
                    n + base + ((imm >> 4) & 3), n + base + ((imm >> 6) & 3)]
    a, c = tov(L, x, bits, e), tov(L, y, bits, e)
    wdst(L, ins, fromv(L, shuffle(L, a, c, bits, e, idx), bits, e), bits)


@sem(*both("PALIGNR"))
def _palignr(L, ins, b):
    bits = dst_bits(L, ins)
    x, y, _ = srcs(L, ins, bits)
    k = ins.immediate(ins.op_count - 1) & 0xff
    n = bits // 8
    a, c = tov(L, x, bits, 8), tov(L, y, bits, 8)
    # per 128-bit lane: bytes of (x:y) >> k*8, y the low half
    zero = L.op(f"bitcast i{bits} 0 to {vty(bits, 8)}")
    idx = []
    for lane in range(bits // 128):
        for i in range(16):
            j = i + k
            if j < 16:
                idx.append(n + lane * 16 + j)  # from y (second shuffle operand)
            elif j < 32:
                idx.append(lane * 16 + j - 16)  # from x
            else:
                idx.append(None)
    r = shuffle(L, a, c, bits, 8, [i if i is not None else 0 for i in idx])
    if any(i is None for i in idx):
        sel = ", ".join("i1 true" if i is None else "i1 false" for i in idx)
        r = L.op(f"select <{n} x i1> <{sel}>, {vty(bits, 8)} {zero}, {vty(bits, 8)} {r}")
    wdst(L, ins, fromv(L, r, bits, 8), bits)


@sem(*both("PSHUFB"))
def _pshufb(L, ins, b):
    bits = dst_bits(L, ins)
    x, y, _ = srcs(L, ins, bits)
    n = bits // 8
    a, m = tov(L, x, bits, 8), tov(L, y, bits, 8)
    r = L.op(f"bitcast i{bits} 0 to {vty(bits, 8)}")
    for i in range(n):
        lane = i // 16
        mi = L.op(f"extractelement <{n} x i8> {m}, i32 {i}")
        ix = L.op(f"and i8 {mi}, 15")
        ix32 = L.op(f"zext i8 {ix} to i32")
        src_i = L.op(f"add i32 {ix32}, {lane * 16}")
        v = L.op(f"extractelement <{n} x i8> {a}, i32 {src_i}")
        neg = L.op(f"icmp slt i8 {mi}, 0")
        v2 = L.op(f"select i1 {neg}, i8 0, i8 {v}")
        r = L.op(f"insertelement <{n} x i8> {r}, i8 {v2}, i32 {i}")
    wdst(L, ins, fromv(L, r, bits, 8), bits)


@sem(*both("MOVHLPS"), *both("MOVLHPS"))
def _movhl(L, ins, b):
    x, y, _ = srcs(L, ins, 128)
    a, c = tov(L, x, 128, 64), tov(L, y, 128, 64)
    idx = [3, 1] if ins.mnemonic in (M.MOVHLPS, M.VMOVHLPS) else [0, 2]
    wdst(L, ins, fromv(L, shuffle(L, a, c, 128, 64, idx), 128, 64), 128)


@sem(*both("MOVHPS"), *both("MOVHPD"), *both("MOVLPS"), *both("MOVLPD"))
def _movhps(L, ins, b):
    high = ins.mnemonic in (M.MOVHPS, M.VMOVHPS, M.MOVHPD, M.VMOVHPD)
    if ins.op_kind(0) == OK.MEMORY:
        v = src(L, ins, 1, 128)
        part = L.op(f"lshr i128 {v}, 64") if high else v
        L.write(ins, 0, L.resize(part, 128, 64), 64)
        return
    base = src(L, ins, 1 if is_vex(ins) else 0, 128)
    m = src(L, ins, ins.op_count - 1, 64)
    z = L.resize(m, 64, 128)
    if high:
        keep = L.op(f"and i128 {base}, 18446744073709551615")
        r = L.op(f"or i128 {keep}, {L.op(f'shl i128 {z}, 64')}")
    else:
        keep = L.op(f"and i128 {base}, {sconst(~((1 << 64) - 1), 128)}")
        r = L.op(f"or i128 {keep}, {z}")
    wdst(L, ins, r, 128)


@sem(*both("MOVSHDUP"), *both("MOVSLDUP"), *both("MOVDDUP"))
def _movdup(L, ins, b):
    bits = dst_bits(L, ins)
    mn = ins.mnemonic
    if mn in (M.MOVDDUP, M.VMOVDDUP):
        x = src(L, ins, 1, 64 if bits == 128 and ins.op_kind(1) == OK.MEMORY else bits)
        if bits == 128 and ins.op_kind(1) == OK.MEMORY:
            x = L.resize(x, 64, 128)
        a = tov(L, x, bits, 64)
        idx = sum(([2 * l, 2 * l] for l in range(bits // 128)), [])
        wdst(L, ins, fromv(L, shuffle(L, a, a, bits, 64, idx), bits, 64), bits)
        return
    x = src(L, ins, 1, bits)
    a = tov(L, x, bits, 32)
    off = 1 if mn in (M.MOVSHDUP, M.VMOVSHDUP) else 0
    idx = sum(([4 * l + off, 4 * l + off, 4 * l + 2 + off, 4 * l + 2 + off] for l in range(bits // 128)), [])
    wdst(L, ins, fromv(L, shuffle(L, a, a, bits, 32, idx), bits, 32), bits)


@sem(*both("PACKSSWB"), *both("PACKUSWB"), *both("PACKSSDW"), *both("PACKUSDW"))
def _pack(L, ins, b):
    bits = dst_bits(L, ins)
    x, y, _ = srcs(L, ins, bits)
    mn = ins.mnemonic
    e = 16 if mn in (M.PACKSSWB, M.VPACKSSWB, M.PACKUSWB, M.VPACKUSWB) else 32
    unsigned = mn in (M.PACKUSWB, M.VPACKUSWB, M.PACKUSDW, M.VPACKUSDW)
    h = e // 2
    lo, hi = (0, (1 << h) - 1) if unsigned else (-(1 << (h - 1)), (1 << (h - 1)) - 1)
    n = bits // e
    vt = vty(bits, e)
    out = []
    for v in (x, y):
        a = tov(L, v, bits, e)
        s = vsuffix(bits, e)
        intrinsic(L, f"declare {vt} @llvm.smax.{s}({vt}, {vt})")
        intrinsic(L, f"declare {vt} @llvm.smin.{s}({vt}, {vt})")
        lov = ", ".join([f"i{e} {lo}"] * n)
        hiv = ", ".join([f"i{e} {hi}"] * n)
        c1 = L.op(f"call {vt} @llvm.smax.{s}({vt} {a}, {vt} <{lov}>)")
        c2 = L.op(f"call {vt} @llvm.smin.{s}({vt} {c1}, {vt} <{hiv}>)")
        out.append(L.op(f"trunc {vt} {c2} to <{n} x i{h}>"))
    per = 128 // e
    idx = []
    for lane in range(bits // 128):
        idx += [lane * per + i for i in range(per)] + [n + lane * per + i for i in range(per)]
    r = L.op(f"shufflevector <{n} x i{h}> {out[0]}, <{n} x i{h}> {out[1]}, <{2 * n} x i32> <"
             + ", ".join(f"i32 {i}" for i in idx) + ">")
    wdst(L, ins, L.op(f"bitcast <{2 * n} x i{h}> {r} to i{bits}"), bits)


PMOVX = {}
for _k, _f, _t in (("BW", 8, 16), ("BD", 8, 32), ("BQ", 8, 64), ("WD", 16, 32), ("WQ", 16, 64), ("DQ", 32, 64)):
    for _s in ("ZX", "SX"):
        for _m in both(f"PMOV{_s}{_k}"):
            PMOVX[_m] = (_f, _t, _s == "SX")


@sem(*PMOVX)
def _pmovx(L, ins, b):
    f, t, signed = PMOVX[ins.mnemonic]
    bits = dst_bits(L, ins)
    n = bits // t
    x = src(L, ins, 1, n * f)
    a = L.op(f"bitcast i{n * f} {x} to <{n} x i{f}>")
    r = L.op(f"{'sext' if signed else 'zext'} <{n} x i{f}> {a} to <{n} x i{t}>")
    wdst(L, ins, fromv(L, r, bits, t), bits)


@sem(*both("PINSRB"), *both("PINSRW"), *both("PINSRD"), *both("PINSRQ"))
def _pinsr(L, ins, b):
    e = {M.PINSRB: 8, M.VPINSRB: 8, M.PINSRW: 16, M.VPINSRW: 16, M.PINSRD: 32, M.VPINSRD: 32,
         M.PINSRQ: 64, M.VPINSRQ: 64}[ins.mnemonic]
    base = src(L, ins, 1 if is_vex(ins) else 0, 128)
    vi = ins.op_count - 2
    v = src(L, ins, vi, e) if ins.op_kind(vi) == OK.MEMORY else L.resize(*_reg_any(L, ins, vi), e)
    k = ins.immediate(ins.op_count - 1) % (128 // e)
    a = tov(L, base, 128, e)
    r = L.op(f"insertelement {vty(128, e)} {a}, i{e} {v}, i32 {k}")
    wdst(L, ins, fromv(L, r, 128, e), 128)


def _reg_any(L, ins, i):
    v, bb = L.read_reg(ins.op_register(i))
    return v, bb


@sem(*both("PEXTRB"), *both("PEXTRW"), *both("PEXTRD"), *both("PEXTRQ"), *both("EXTRACTPS"))
def _pextr(L, ins, b):
    e = {M.PEXTRB: 8, M.VPEXTRB: 8, M.PEXTRW: 16, M.VPEXTRW: 16, M.PEXTRD: 32, M.VPEXTRD: 32,
         M.PEXTRQ: 64, M.VPEXTRQ: 64, M.EXTRACTPS: 32, M.VEXTRACTPS: 32}[ins.mnemonic]
    x = src(L, ins, 1, 128)
    k = ins.immediate(2) % (128 // e)
    v = L.op(f"extractelement {vty(128, e)} {tov(L, x, 128, e)}, i32 {k}")
    if ins.op_kind(0) == OK.MEMORY:
        L.write(ins, 0, v, e)
    else:
        rb = L.op_bits(ins, 0)
        L.write(ins, 0, L.resize(v, e, rb) if rb != e else v, rb)


@sem(*both("PMOVMSKB"), *both("MOVMSKPS"), *both("MOVMSKPD"))
def _movmsk(L, ins, b):
    sb = L.op_bits(ins, 1)
    e = {M.PMOVMSKB: 8, M.VPMOVMSKB: 8, M.MOVMSKPS: 32, M.VMOVMSKPS: 32, M.MOVMSKPD: 64, M.VMOVMSKPD: 64}[ins.mnemonic]
    n = sb // e
    x = src(L, ins, 1, sb)
    a = tov(L, x, sb, e)
    neg = L.op(f"icmp slt {vty(sb, e)} {a}, zeroinitializer")
    m = L.op(f"bitcast <{n} x i1> {neg} to i{n}")
    rb = L.op_bits(ins, 0)
    L.write(ins, 0, L.resize(m, n, rb) if n != rb else m, rb)


@sem(*both("PTEST"))
def _ptest(L, ins, b):
    bits = L.op_bits(ins, 0)
    x = src(L, ins, 0, bits)
    y = src(L, ins, 1, bits)
    a = L.op(f"and i{bits} {x}, {y}")
    nx = L.op(f"xor i{bits} {x}, -1")
    c = L.op(f"and i{bits} {nx}, {y}")
    L.setf("zf", L.op(f"icmp eq i{bits} {a}, 0"))
    L.setf("cf", L.op(f"icmp eq i{bits} {c}, 0"))
    for f in ("of", "sf", "af", "pf"):
        L.setf(f, "false")


@sem(*both("PBLENDW"), *both("BLENDPS"), *both("BLENDPD"))
def _blend(L, ins, b):
    bits = dst_bits(L, ins)
    x, y, _ = srcs(L, ins, bits)
    e = {M.PBLENDW: 16, M.VPBLENDW: 16, M.BLENDPS: 32, M.VBLENDPS: 32, M.BLENDPD: 64, M.VBLENDPD: 64}[ins.mnemonic]
    imm = ins.immediate(ins.op_count - 1)
    n = bits // e
    per = 8 if e == 16 else n  # pblendw repeats its 8-bit mask per 128-bit lane
    idx = [(n + i) if (imm >> (i % per)) & 1 else i for i in range(n)]
    a, c = tov(L, x, bits, e), tov(L, y, bits, e)
    wdst(L, ins, fromv(L, shuffle(L, a, c, bits, e, idx), bits, e), bits)


@sem(*both("PBLENDVB"), *both("BLENDVPS"), *both("BLENDVPD"))
def _blendv(L, ins, b):
    bits = dst_bits(L, ins)
    e = {M.PBLENDVB: 8, M.VPBLENDVB: 8, M.BLENDVPS: 32, M.VBLENDVPS: 32, M.BLENDVPD: 64, M.VBLENDVPD: 64}[ins.mnemonic]
    if is_vex(ins):
        x, y, msk = src(L, ins, 1, bits), src(L, ins, 2, bits), src(L, ins, 3, bits)
    else:
        x, y = src(L, ins, 0, bits), src(L, ins, 1, bits)
        msk = L.op("load i128, ptr %x0")  # implicit xmm0
    vt = vty(bits, e)
    m = L.op(f"icmp slt {vt} {tov(L, msk, bits, e)}, zeroinitializer")
    r = L.op(f"select <{bits // e} x i1> {m}, {vt} {tov(L, y, bits, e)}, {vt} {tov(L, x, bits, e)}")
    wdst(L, ins, fromv(L, r, bits, e), bits)


@sem(M.VBROADCASTSS, M.VBROADCASTSD, M.VPBROADCASTB, M.VPBROADCASTW, M.VPBROADCASTD, M.VPBROADCASTQ)
def _bcast(L, ins, b):
    bits = dst_bits(L, ins)
    e = {M.VBROADCASTSS: 32, M.VBROADCASTSD: 64, M.VPBROADCASTB: 8, M.VPBROADCASTW: 16,
         M.VPBROADCASTD: 32, M.VPBROADCASTQ: 64}[ins.mnemonic]
    v = src(L, ins, 1, e)
    n = bits // e
    vt = vty(bits, e)
    i0 = L.op(f"insertelement {vt} poison, i{e} {v}, i32 0")
    s = L.op(f"shufflevector {vt} {i0}, {vt} poison, <{n} x i32> zeroinitializer")
    wdst(L, ins, fromv(L, s, bits, e), bits)


@sem(M.VBROADCASTF128, M.VBROADCASTI128)
def _bcast128(L, ins, b):
    v = src(L, ins, 1, 128)
    z = L.resize(v, 128, 256)
    wdst(L, ins, L.op(f"or i256 {z}, {L.op(f'shl i256 {z}, 128')}"), 256)


@sem(M.VINSERTF128, M.VINSERTI128)
def _vinsert128(L, ins, b):
    base = src(L, ins, 1, 256)
    v = L.resize(src(L, ins, 2, 128), 128, 256)
    hi = ins.immediate(3) & 1
    if hi:
        keep = L.op(f"and i256 {base}, {(1 << 128) - 1}")
        r = L.op(f"or i256 {keep}, {L.op(f'shl i256 {v}, 128')}")
    else:
        keep = L.op(f"and i256 {base}, {sconst(~((1 << 128) - 1), 256)}")
        r = L.op(f"or i256 {keep}, {v}")
    wdst(L, ins, r, 256)


@sem(M.VEXTRACTF128, M.VEXTRACTI128)
def _vextract128(L, ins, b):
    v = src(L, ins, 1, 256)
    hi = ins.immediate(2) & 1
    part = L.op(f"lshr i256 {v}, 128") if hi else v
    L.write(ins, 0, L.resize(part, 256, 128), 128)


@sem(M.VPERM2F128, M.VPERM2I128)
def _vperm2f128(L, ins, b):
    x, y = src(L, ins, 1, 256), src(L, ins, 2, 256)
    imm = ins.immediate(3)
    halves = []
    for v in (x, y):
        halves.append(L.resize(v, 256, 128))
        halves.append(L.resize(L.op(f"lshr i256 {v}, 128"), 256, 128))
    parts = []
    for k in (imm & 0xf, imm >> 4):
        parts.append("0" if k & 8 else halves[k & 3])
    lo = L.resize(parts[0], 128, 256) if parts[0] != "0" else "0"
    hi = L.resize(parts[1], 128, 256) if parts[1] != "0" else "0"
    r = L.op(f"or i256 {lo}, {L.op(f'shl i256 {hi}, 128') if hi != '0' else '0'}")
    wdst(L, ins, r, 256)


@sem(M.VPERMILPS, M.VPERMILPD)
def _vpermil(L, ins, b):
    bits = dst_bits(L, ins)
    pd = ins.mnemonic == M.VPERMILPD
    e = 64 if pd else 32
    if ins.op_kind(ins.op_count - 1) != OK.IMMEDIATE8:  # control vector: per-lane selector
        x, ctl = src(L, ins, 1, bits), src(L, ins, 2, bits)
        n = bits // e
        a, cv = tov(L, x, bits, e), tov(L, ctl, bits, e)
        r = a
        per = 128 // e
        for i in range(n):
            c = L.op(f"extractelement {vty(bits, e)} {cv}, i32 {i}")
            sel = L.op(f"lshr i{e} {c}, 1") if pd else c
            k = L.op(f"and i{e} {sel}, {per - 1}")
            k32 = L.resize(k, e, 32) if e != 32 else k
            j = L.op(f"add i32 {k32}, {(i // per) * per}")
            v = L.op(f"extractelement {vty(bits, e)} {a}, i32 {j}")
            r = L.op(f"insertelement {vty(bits, e)} {r}, i{e} {v}, i32 {i}")
        wdst(L, ins, fromv(L, r, bits, e), bits)
        return
    x = src(L, ins, 1, bits)
    imm = ins.immediate(2)
    pd = ins.mnemonic == M.VPERMILPD
    e = 64 if pd else 32
    idx = []
    for lane in range(bits // 128):
        if pd:
            idx += [lane * 2 + ((imm >> (lane * 2 + i)) & 1) for i in range(2)]
        else:
            idx += [lane * 4 + ((imm >> (2 * i)) & 3) for i in range(4)]
    a = tov(L, x, bits, e)
    wdst(L, ins, fromv(L, shuffle(L, a, a, bits, e, idx), bits, e), bits)


@sem(M.INSERTPS, M.VINSERTPS)
def _insertps(L, ins, b):
    base = src(L, ins, 1 if is_vex(ins) else 0, 128)
    si = ins.op_count - 2
    imm = ins.immediate(ins.op_count - 1)
    if ins.op_kind(si) == OK.MEMORY:
        v = src(L, ins, si, 32)
    else:
        sv = src(L, ins, si, 128)
        v = L.op(f"extractelement <4 x i32> {tov(L, sv, 128, 32)}, i32 {(imm >> 6) & 3}")
    a = tov(L, base, 128, 32)
    r = L.op(f"insertelement <4 x i32> {a}, i32 {v}, i32 {(imm >> 4) & 3}")
    for i in range(4):
        if imm & (1 << i):
            r = L.op(f"insertelement <4 x i32> {r}, i32 0, i32 {i}")
    wdst(L, ins, fromv(L, r, 128, 32), 128)


# ---- floating point -------------------------------------------------------------------------------

FP_OPS = {"ADD": "fadd", "SUB": "fsub", "MUL": "fmul", "DIV": "fdiv", "MIN": "min", "MAX": "max",
          "SQRT": "sqrt", "RCP": "rcp", "RSQRT": "rsqrt"}
FP_PACKED, FP_SCALAR = {}, {}
for _op, _k in FP_OPS.items():
    for _sfx, _e in (("PS", 32), ("PD", 64)):
        for _m in both(_op + _sfx):
            FP_PACKED[_m] = (_k, _e)
    for _sfx, _e in (("SS", 32), ("SD", 64)):
        for _m in both(_op + _sfx):
            FP_SCALAR[_m] = (_k, _e)


def fp_apply(L, kind, vt, a, c, scalar_t=None):
    t = scalar_t or vt
    if kind in ("fadd", "fsub", "fmul", "fdiv"):
        return L.op(f"{kind} {t} {a}, {c}")
    if kind in ("min", "max"):
        cmp = L.op(f"fcmp {'olt' if kind == 'min' else 'ogt'} {t} {a}, {c}")
        return L.op(f"select {'i1' if scalar_t else '<' + vt.split(' x')[0][1:] + ' x i1>'} {cmp}, {t} {a}, {t} {c}")
    if kind in ("sqrt", "rsqrt"):
        nm = _intr_name(t)
        L.mod.intrinsics.add(f"declare {t} @llvm.sqrt.{nm}({t})")
        s = L.op(f"call {t} @llvm.sqrt.{nm}({t} {c})")
        if kind == "sqrt":
            return s
        return L.op(f"fdiv {t} {_fp_one(t)}, {s}")
    if kind == "rcp":
        return L.op(f"fdiv {t} {_fp_one(t)}, {c}")
    raise Unsupported(kind)


def _intr_name(t):
    if t == "float":
        return "f32"
    if t == "double":
        return "f64"
    n, e = t[1:-1].split(" x ")
    return f"v{n}{'f32' if e == 'float' else 'f64'}"


def _fp_one(t):
    if t in ("float", "double"):
        return "1.0"
    n, e = t[1:-1].split(" x ")
    return "<" + ", ".join([f"{e} 1.0"] * int(n)) + ">"


@sem(*FP_PACKED)
def _fp_packed(L, ins, b):
    kind, e = FP_PACKED[ins.mnemonic]
    bits = dst_bits(L, ins)
    unary = kind in ("sqrt", "rcp", "rsqrt")
    if unary:
        x = y = src(L, ins, 1, bits)
    else:
        x, y, _ = srcs(L, ins, bits)
    vt = vty(bits, e, True)
    a, c = tov(L, x, bits, e, True), tov(L, y, bits, e, True)
    r = fp_apply(L, kind, vt, a, c)
    wdst(L, ins, fromv(L, r, bits, e, True), bits)


def scalar_merge(L, ins, e, value_fp):
    """Write a scalar FP result into lane 0; the rest from the destination (legacy) or src1 (VEX)."""
    base = src(L, ins, 1 if is_vex(ins) else 0, 128)
    a = tov(L, base, 128, e, True)
    r = L.op(f"insertelement {vty(128, e, True)} {a}, {ety(e, True)} {value_fp}, i32 0")
    wdst(L, ins, fromv(L, r, 128, e, True), 128)


def scalar_src(L, ins, i, e):
    v = src(L, ins, i, e)
    return L.op(f"bitcast i{e} {v} to {ety(e, True)}")


@sem(*FP_SCALAR)
def _fp_scalar(L, ins, b):
    kind, e = FP_SCALAR[ins.mnemonic]
    t = ety(e, True)
    last = ins.op_count - 1
    c = scalar_src(L, ins, last, e)
    a = scalar_src(L, ins, last - 1, e) if kind not in ("sqrt", "rcp", "rsqrt") else c
    r = fp_apply(L, kind, None, a, c, scalar_t=t)
    scalar_merge(L, ins, e, r)


@sem(*both("HADDPS"), *both("HADDPD"), *both("HSUBPS"), *both("HSUBPD"))
def _hadd(L, ins, b):
    bits = dst_bits(L, ins)
    x, y, _ = srcs(L, ins, bits)
    mn = ins.mnemonic
    e = 64 if mn in (M.HADDPD, M.VHADDPD, M.HSUBPD, M.VHSUBPD) else 32
    op = "fsub" if mn in (M.HSUBPS, M.VHSUBPS, M.HSUBPD, M.VHSUBPD) else "fadd"
    n = bits // e
    per = 128 // e
    ev, od = [], []
    for lane in range(bits // 128):
        base = lane * per
        for src_off in (0, n):
            for i in range(per // 2):
                ev.append(src_off + base + 2 * i)
                od.append(src_off + base + 2 * i + 1)
    vt = vty(bits, e, True)
    a, c = tov(L, x, bits, e, True), tov(L, y, bits, e, True)
    mask = lambda idx: "<" + ", ".join(f"i32 {i}" for i in idx) + ">"
    pe = L.op(f"shufflevector {vt} {a}, {vt} {c}, <{n} x i32> {mask(ev)}")
    po = L.op(f"shufflevector {vt} {a}, {vt} {c}, <{n} x i32> {mask(od)}")
    wdst(L, ins, fromv(L, L.op(f"{op} {vt} {pe}, {po}"), bits, e, True), bits)


@sem(*both("DPPS"))
def _dpps(L, ins, b):
    bits = dst_bits(L, ins)
    x, y, _ = srcs(L, ins, bits)
    imm = ins.immediate(ins.op_count - 1)
    out = []
    for lane in range(bits // 128):
        a = tov(L, L.resize(L.op(f"lshr i{bits} {x}, {lane * 128}") if lane else x, bits, 128) if bits != 128 else x, 128, 32, True)
        c = tov(L, L.resize(L.op(f"lshr i{bits} {y}, {lane * 128}") if lane else y, bits, 128) if bits != 128 else y, 128, 32, True)
        p = L.op(f"fmul <4 x float> {a}, {c}")
        s = None
        for i in range(4):  # x86 adds pairwise: (p0+p1)+(p2+p3)
            el = L.op(f"extractelement <4 x float> {p}, i32 {i}") if imm & (0x10 << i) else "0.0"
            out_el = el
            if i % 2 == 0:
                first = out_el
            else:
                pair = L.op(f"fadd float {first}, {out_el}")
                s = pair if s is None else L.op(f"fadd float {s}, {pair}")
        r = "zeroinitializer"
        r = L.op(f"bitcast i128 0 to <4 x float>")
        for i in range(4):
            if imm & (1 << i):
                r = L.op(f"insertelement <4 x float> {r}, float {s}, i32 {i}")
        out.append(fromv(L, r, 128, 32, True))
    if bits == 128:
        wdst(L, ins, out[0], 128)
    else:
        lo = L.resize(out[0], 128, 256)
        hi = L.op(f"shl i256 {L.resize(out[1], 128, 256)}, 128")
        wdst(L, ins, L.op(f"or i256 {lo}, {hi}"), 256)


CMP_PRED = ["oeq", "olt", "ole", "uno", "une", "uge", "ugt", "ord",
            "ueq", "ult", "ule", "false", "one", "oge", "ogt", "true"]


@sem(*both("CMPPS"), *both("CMPPD"), *both("CMPSS"), *both("CMPSD"))
def _cmpps(L, ins, b):
    mn = ins.mnemonic
    scalar = mn in (M.CMPSS, M.VCMPSS, M.CMPSD, M.VCMPSD)
    if mn == M.CMPSD and ins.is_string_instruction:
        raise Unsupported("cmpsd string")
    e = 64 if mn in (M.CMPPD, M.VCMPPD, M.CMPSD, M.VCMPSD) else 32
    imm = ins.immediate(ins.op_count - 1) & (0x1f if is_vex(ins) else 0x7)  # legacy SSE: 8 predicates
    pred = CMP_PRED[imm & 0xf]  # bit 4 only changes signalling behaviour
    if scalar:
        last = ins.op_count - 2
        c = scalar_src(L, ins, last, e)
        a = scalar_src(L, ins, last - 1, e)
        t = ety(e, True)
        m = L.op(f"fcmp {pred} {t} {a}, {c}")
        iv = L.op(f"sext i1 {m} to i{e}")
        fv = L.op(f"bitcast i{e} {iv} to {t}")
        scalar_merge(L, ins, e, fv)
        return
    bits = dst_bits(L, ins)
    x, y, _ = srcs(L, ins, bits)
    vt = vty(bits, e, True)
    m = L.op(f"fcmp {pred} {vt} {tov(L, x, bits, e, True)}, {tov(L, y, bits, e, True)}")
    r = L.op(f"sext <{bits // e} x i1> {m} to {vty(bits, e)}")
    wdst(L, ins, fromv(L, r, bits, e), bits)


@sem(*both("UCOMISS"), *both("COMISS"), *both("UCOMISD"), *both("COMISD"))
def _ucomi(L, ins, b):
    e = 64 if ins.mnemonic in (M.UCOMISD, M.VUCOMISD, M.COMISD, M.VCOMISD) else 32
    a = scalar_src(L, ins, 0, e)
    c = scalar_src(L, ins, 1, e)
    t = ety(e, True)
    uno = L.op(f"fcmp uno {t} {a}, {c}")
    lt = L.op(f"fcmp olt {t} {a}, {c}")
    eq = L.op(f"fcmp oeq {t} {a}, {c}")
    L.setf("zf", L.op(f"or i1 {uno}, {eq}"))
    L.setf("pf", uno)
    L.setf("cf", L.op(f"or i1 {uno}, {lt}"))
    for f in ("of", "sf", "af"):
        L.setf(f, "false")


def _fp_to_int(L, f, t, ibits, rounding):
    """x86 cvt(t)s*2si: NaN or out of range -> integer indefinite (INT_MIN)."""
    if rounding:
        nm = _intr_name(t)
        L.mod.intrinsics.add(f"declare {t} @llvm.rint.{nm}({t})")
        f = L.op(f"call {t} @llvm.rint.{nm}({t} {f})")
    lo = f"{-(2.0 ** (ibits - 1))!r}"
    hi = f"{2.0 ** (ibits - 1)!r}"
    ge = L.op(f"fcmp oge {t} {f}, {_fp_lit(t, -(2.0 ** (ibits - 1)))}")
    lt = L.op(f"fcmp olt {t} {f}, {_fp_lit(t, 2.0 ** (ibits - 1))}")
    ok = L.op(f"and i1 {ge}, {lt}")
    safe = L.op(f"select i1 {ok}, {t} {f}, {t} 0.0")
    v = L.op(f"fptosi {t} {safe} to i{ibits}")
    return L.op(f"select i1 {ok}, i{ibits} {v}, i{ibits} {sconst(1 << (ibits - 1), ibits)}")


def _fp_lit(t, x):
    import struct
    if t == "double":
        return "0x%016X" % struct.unpack("<Q", struct.pack("<d", x))[0]
    # LLVM float literals are written as the double that holds the float value exactly
    return "0x%016X" % struct.unpack("<Q", struct.pack("<d", struct.unpack("<f", struct.pack("<f", x))[0]))[0]


@sem(*both("CVTTSS2SI"), *both("CVTTSD2SI"), *both("CVTSS2SI"), *both("CVTSD2SI"))
def _cvt2si(L, ins, b):
    mn = ins.mnemonic
    e = 64 if mn in (M.CVTTSD2SI, M.VCVTTSD2SI, M.CVTSD2SI, M.VCVTSD2SI) else 32
    rounding = mn in (M.CVTSS2SI, M.VCVTSS2SI, M.CVTSD2SI, M.VCVTSD2SI)
    ib = L.op_bits(ins, 0)
    f = scalar_src(L, ins, 1, e)
    L.write(ins, 0, _fp_to_int(L, f, ety(e, True), ib, rounding), ib)


@sem(*both("CVTSI2SS"), *both("CVTSI2SD"))
def _cvtsi2s(L, ins, b):
    e = 64 if ins.mnemonic in (M.CVTSI2SD, M.VCVTSI2SD) else 32
    si = ins.op_count - 1
    ib = L.op_bits(ins, si)
    v = src(L, ins, si, ib)
    f = L.op(f"sitofp i{ib} {v} to {ety(e, True)}")
    scalar_merge(L, ins, e, f)


@sem(*both("CVTSS2SD"), *both("CVTSD2SS"))
def _cvtss2sd(L, ins, b):
    to_d = ins.mnemonic in (M.CVTSS2SD, M.VCVTSS2SD)
    fe, te = (32, 64) if to_d else (64, 32)
    f = scalar_src(L, ins, ins.op_count - 1, fe)
    r = L.op(f"{'fpext' if to_d else 'fptrunc'} {ety(fe, True)} {f} to {ety(te, True)}")
    scalar_merge(L, ins, te, r)


@sem(*both("CVTDQ2PS"))
def _cvtdq2ps(L, ins, b):
    bits = dst_bits(L, ins)
    x = src(L, ins, 1, bits)
    r = L.op(f"sitofp {vty(bits, 32)} {tov(L, x, bits, 32)} to {vty(bits, 32, True)}")
    wdst(L, ins, fromv(L, r, bits, 32, True), bits)


@sem(*both("CVTTPS2DQ"), *both("CVTPS2DQ"))
def _cvtps2dq(L, ins, b):
    bits = dst_bits(L, ins)
    x = src(L, ins, 1, bits)
    n = bits // 32
    vt, it = vty(bits, 32, True), vty(bits, 32)
    f = tov(L, x, bits, 32, True)
    if ins.mnemonic in (M.CVTPS2DQ, M.VCVTPS2DQ):
        nm = _intr_name(vt)
        L.mod.intrinsics.add(f"declare {vt} @llvm.rint.{nm}({vt})")
        f = L.op(f"call {vt} @llvm.rint.{nm}({vt} {f})")
    lo = "<" + ", ".join([f"float {_fp_lit('float', -2.0 ** 31)}"] * n) + ">"
    hi = "<" + ", ".join([f"float {_fp_lit('float', 2.0 ** 31)}"] * n) + ">"
    ge = L.op(f"fcmp oge {vt} {f}, {lo}")
    lt = L.op(f"fcmp olt {vt} {f}, {hi}")
    ok = L.op(f"and <{n} x i1> {ge}, {lt}")
    safe = L.op(f"select <{n} x i1> {ok}, {vt} {f}, {vt} zeroinitializer")
    v = L.op(f"fptosi {vt} {safe} to {it}")
    ind = "<" + ", ".join(["i32 -2147483648"] * n) + ">"
    r = L.op(f"select <{n} x i1> {ok}, {it} {v}, {it} {ind}")
    wdst(L, ins, fromv(L, r, bits, 32), bits)


@sem(*both("CVTPS2PD"), *both("CVTDQ2PD"))
def _cvtps2pd(L, ins, b):
    bits = dst_bits(L, ins)
    n = bits // 64
    x = src(L, ins, 1, n * 32)
    if ins.mnemonic in (M.CVTPS2PD, M.VCVTPS2PD):
        r = L.op(f"fpext <{n} x float> {L.op(f'bitcast i{n * 32} {x} to <{n} x float>')} to <{n} x double>")
    else:
        r = L.op(f"sitofp <{n} x i32> {L.op(f'bitcast i{n * 32} {x} to <{n} x i32>')} to <{n} x double>")
    wdst(L, ins, fromv(L, r, bits, 64, True), bits)


@sem(*both("CVTPD2PS"))
def _cvtpd2ps(L, ins, b):
    sbits = L.op_bits(ins, 1)
    n = sbits // 64
    x = src(L, ins, 1, sbits)
    r = L.op(f"fptrunc <{n} x double> {tov(L, x, sbits, 64, True)} to <{n} x float>")
    v = L.op(f"bitcast <{n} x float> {r} to i{n * 32}")
    wdst(L, ins, L.resize(v, n * 32, 128), 128)


@sem(*both("ROUNDSS"), *both("ROUNDSD"), *both("ROUNDPS"), *both("ROUNDPD"))
def _round(L, ins, b):
    mn = ins.mnemonic
    e = 64 if mn in (M.ROUNDSD, M.VROUNDSD, M.ROUNDPD, M.VROUNDPD) else 32
    imm = ins.immediate(ins.op_count - 1)
    mode = "rint" if imm & 4 else ("roundeven", "floor", "ceil", "trunc")[imm & 3]
    if mn in (M.ROUNDSS, M.VROUNDSS, M.ROUNDSD, M.VROUNDSD):
        t = ety(e, True)
        f = scalar_src(L, ins, ins.op_count - 2, e)
        nm = _intr_name(t)
        L.mod.intrinsics.add(f"declare {t} @llvm.{mode}.{nm}({t})")
        scalar_merge(L, ins, e, L.op(f"call {t} @llvm.{mode}.{nm}({t} {f})"))
        return
    bits = dst_bits(L, ins)
    vt = vty(bits, e, True)
    nm = _intr_name(vt)
    L.mod.intrinsics.add(f"declare {vt} @llvm.{mode}.{nm}({vt})")
    f = tov(L, src(L, ins, 1, bits), bits, e, True)
    wdst(L, ins, fromv(L, L.op(f"call {vt} @llvm.{mode}.{nm}({vt} {f})"), bits, e, True), bits)


@sem(*both("CVTTPD2DQ"), *both("CVTPD2DQ"))
def _cvtpd2dq(L, ins, b):
    sbits = L.op_bits(ins, 1)
    n = sbits // 64
    x = src(L, ins, 1, sbits)
    vt, it = f"<{n} x double>", f"<{n} x i32>"
    f = tov(L, x, sbits, 64, True)
    if ins.mnemonic in (M.CVTPD2DQ, M.VCVTPD2DQ):
        nm = _intr_name(vt)
        L.mod.intrinsics.add(f"declare {vt} @llvm.rint.{nm}({vt})")
        f = L.op(f"call {vt} @llvm.rint.{nm}({vt} {f})")
    lo = "<" + ", ".join([f"double {_fp_lit('double', -2.0 ** 31)}"] * n) + ">"
    hi = "<" + ", ".join([f"double {_fp_lit('double', 2.0 ** 31)}"] * n) + ">"
    ok = L.op(f"and <{n} x i1> {L.op(f'fcmp oge {vt} {f}, {lo}')}, {L.op(f'fcmp olt {vt} {f}, {hi}')}")
    safe = L.op(f"select <{n} x i1> {ok}, {vt} {f}, {vt} zeroinitializer")
    v = L.op(f"fptosi {vt} {safe} to {it}")
    ind = "<" + ", ".join(["i32 -2147483648"] * n) + ">"
    r = L.op(f"select <{n} x i1> {ok}, {it} {v}, {it} {ind}")
    iv = L.op(f"bitcast {it} {r} to i{n * 32}")
    wdst(L, ins, L.resize(iv, n * 32, 128), 128)  # upper half of the 128-bit destination zeroed


@sem(*both("PHADDW"), *both("PHADDD"), *both("PHSUBW"), *both("PHSUBD"))
def _phadd(L, ins, b):
    bits = dst_bits(L, ins)
    x, y, _ = srcs(L, ins, bits)
    mn = ins.mnemonic
    e = 16 if mn in (M.PHADDW, M.VPHADDW, M.PHSUBW, M.VPHSUBW) else 32
    op = "sub" if mn in (M.PHSUBW, M.VPHSUBW, M.PHSUBD, M.VPHSUBD) else "add"
    n = bits // e
    per = 128 // e
    ev, od = [], []
    for lane in range(bits // 128):
        for src_off in (0, n):
            for i in range(per // 2):
                ev.append(src_off + lane * per + 2 * i)
                od.append(src_off + lane * per + 2 * i + 1)
    vt = vty(bits, e)
    a, c = tov(L, x, bits, e), tov(L, y, bits, e)
    mask = lambda idx: "<" + ", ".join(f"i32 {i}" for i in idx) + ">"
    pe = L.op(f"shufflevector {vt} {a}, {vt} {c}, <{n} x i32> {mask(ev)}")
    po = L.op(f"shufflevector {vt} {a}, {vt} {c}, <{n} x i32> {mask(od)}")
    wdst(L, ins, fromv(L, L.op(f"{op} {vt} {pe}, {po}"), bits, e), bits)


@sem(M.VMASKMOVPS, M.VMASKMOVPD)
def _vmaskmov(L, ins, b):
    """Masked lanes are neither read nor written (and cannot fault): llvm.masked.load/store."""
    e = 32 if ins.mnemonic == M.VMASKMOVPS else 64
    if ins.op_kind(0) == OK.MEMORY:  # store: mem, mask, value
        bits = L.op_bits(ins, 2)
        msk, val = src(L, ins, 1, bits), src(L, ins, 2, bits)
        addr = L.ea(ins)
    else:  # load: reg, mask, mem
        bits = L.op_bits(ins, 0)
        msk = src(L, ins, 1, bits)
        addr = L.ea(ins)
    n = bits // e
    vt = vty(bits, e)
    m = L.op(f"icmp slt {vt} {tov(L, msk, bits, e)}, zeroinitializer")
    p = L.op(f"inttoptr i64 {addr} to ptr")
    s = vsuffix(bits, e)
    if ins.op_kind(0) == OK.MEMORY:
        intrinsic(L, f"declare void @llvm.masked.store.{s}.p0({vt}, ptr, i32, <{n} x i1>)")
        if L.mod.tso and not L.stack_relative(ins):
            L.e("fence release")
        L.e(f"call void @llvm.masked.store.{s}.p0({vt} {tov(L, val, bits, e)}, ptr {p}, i32 1, <{n} x i1> {m})")
    else:
        intrinsic(L, f"declare {vt} @llvm.masked.load.{s}.p0(ptr, i32, <{n} x i1>, {vt})")
        r = L.op(f"call {vt} @llvm.masked.load.{s}.p0(ptr {p}, i32 1, <{n} x i1> {m}, {vt} zeroinitializer)")
        if L.mod.tso and not L.stack_relative(ins):
            L.e("fence acquire")
        wdst(L, ins, fromv(L, r, bits, e), bits)
