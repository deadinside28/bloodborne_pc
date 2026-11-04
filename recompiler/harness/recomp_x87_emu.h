// SPDX-License-Identifier: GPL-2.0-or-later
// An x87 emulator for recompiled code on hosts without x87 (AArch64): the register stack, TOP and
// tags, the control and status words, and every instruction form the game's libc uses, in
// software, bit for bit as the hardware (checked against it by harness/x87_fuzz.c on x86-64).
//
//   uint64_t x87emu_insn(uint64_t enc, uint64_t addr, uint64_t flags): one instruction as
//       recomp/lift.py x87_encoding packs it (opcode, ModRM with any memory operand at [addr]);
//       flags: the guest's EFLAGS (fcmovcc); returns them updated (fcomi/fucomi set ZF PF CF).
//   uint64_t x87emu_env(int op, uint64_t addr): the environment ops of recomp_x87.h's
//       __recomp_x87 (fnstenv, fldenv, fnstcw, fldcw, fnstsw, fnclex, fninit); returns the status word.
//
// Per thread, as the FPU is. Exceptions are taken as masked whatever the control word says (an
// unmasked one would trap at the next x87 instruction: the status word still shows it pending).
// Not emulated (the game does not use them): the transcendentals (fsin, fcos, fsincos, fptan,
// fpatan, f2xm1, fyl2x, fyl2xp1), packed BCD (fbld, fbstp), fnsave / frstor: they stop the program.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned __int128 x87u128;
typedef struct { uint64_t m; uint16_t se; } X87Reg;  // se: sign << 15 | biased exponent
typedef struct { X87Reg r[8]; uint16_t cw, sw, tag; } X87State;  // r: physical registers

static _Thread_local X87State x87e = { .cw = 0x037f, .sw = 0, .tag = 0xffff };

enum { X_IE = 1, X_DE = 2, X_ZE = 4, X_OE = 8, X_UE = 16, X_PE = 32, X_SF = 64, X_ES = 0x80,
       X_C0 = 0x100, X_C1 = 0x200, X_C2 = 0x400, X_C3 = 0x4000, X_B = 0x8000 };
enum { CLS_ZERO, CLS_FIN, CLS_INF, CLS_NAN, CLS_BAD };  // BAD: unsupported formats (unnormals...)

static const X87Reg X_INDEF = { 0xC000000000000000ull, 0xFFFF };

// ---- stack ---------------------------------------------------------------------------------------
static int x87_top(void) { return (x87e.sw >> 11) & 7; }
static void x87_set_top(int t) { x87e.sw = (uint16_t)((x87e.sw & ~0x3800) | ((t & 7) << 11)); }
static int x87_phys(int i) { return (x87_top() + i) & 7; }
static int x87_empty(int i) { return ((x87e.tag >> (2 * x87_phys(i))) & 3) == 3; }

static unsigned x87_tag_of(X87Reg v) {
    const unsigned e = v.se & 0x7fff;
    if (e == 0x7fff) return 2;
    if (e == 0) return v.m ? 2 : 1;
    return (v.m >> 63) ? 0 : 2;  // an unnormal: special
}
static void x87_put(int i, X87Reg v) {
    const int p = x87_phys(i);
    x87e.r[p] = v;
    x87e.tag = (uint16_t)((x87e.tag & ~(3u << (2 * p))) | (x87_tag_of(v) << (2 * p)));
}
static X87Reg x87_get(int i) { return x87e.r[x87_phys(i)]; }
static void x87_free(int i) { x87e.tag |= (uint16_t)(3u << (2 * x87_phys(i))); }
static void x87_raise(unsigned f) {
    x87e.sw |= (uint16_t)f;
    if (x87e.sw & ~x87e.cw & 0x3f) x87e.sw |= X_ES | X_B;
}
static void x87_pop(void) { x87_free(0); x87_set_top(x87_top() + 1); }
static void x87_push(X87Reg v) {
    if (!x87_empty(7)) {  // stack overflow
        x87_raise(X_IE | X_SF);
        x87e.sw |= X_C1;
        v = X_INDEF;
    } else x87e.sw &= ~X_C1;
    x87_set_top(x87_top() - 1);
    x87_put(0, v);
}
// An operand st(i): empty -> stack underflow (IE SF, C1 = 0); returns 0 then.
static int x87_need(int i) {
    if (!x87_empty(i)) return 1;
    x87_raise(X_IE | X_SF);
    x87e.sw &= ~X_C1;
    return 0;
}

// ---- numbers ------------------------------------------------------------------------------------
typedef struct { int sign, cls; int32_t e; uint64_t m; } X87Unp;
// A float memory operand was a denormal (its 80-bit value is not): DE when the operation looks at it.
static _Thread_local int x87_mem_de, x87_de_pending;  // finite: m * 2^(e - 63), m's top bit set

static int x87_clz64(uint64_t v) { return v ? __builtin_clzll(v) : 64; }
static int x87_clz128(x87u128 v) {
    const uint64_t hi = (uint64_t)(v >> 64);
    return hi ? __builtin_clzll(hi) : 64 + x87_clz64((uint64_t)v);
}

static X87Unp x87_unpack(X87Reg r) {
    X87Unp u = { r.se >> 15, CLS_FIN, 0, r.m };
    const int32_t e = r.se & 0x7fff;
    if (e == 0x7fff) {
        if (!(r.m >> 63)) u.cls = CLS_BAD;  // pseudo-infinity, pseudo-NaN
        else u.cls = (r.m << 1) ? CLS_NAN : CLS_INF;
        return u;
    }
    if (e == 0) {
        if (!r.m) { u.cls = CLS_ZERO; return u; }
        const int s = x87_clz64(r.m);  // denormal (or pseudo-denormal: same value)
        u.m = r.m << s;
        u.e = 1 - 16383 - s;
        return u;
    }
    if (!(r.m >> 63)) { u.cls = CLS_BAD; return u; }  // unnormal
    u.e = e - 16383;
    return u;
}
static int x87_is_snan(X87Reg r) {
    return (r.se & 0x7fff) == 0x7fff && (r.m >> 63) && (r.m << 1) && !((r.m >> 62) & 1);
}
static int x87_is_denormal(X87Reg r) { return (r.se & 0x7fff) == 0 && r.m; }
static X87Reg x87_quiet(X87Reg r) { r.m |= 1ull << 62; return r; }
static X87Reg x87_inf(int sign) { return (X87Reg){ 1ull << 63, (uint16_t)(sign << 15 | 0x7fff) }; }
static X87Reg x87_zero(int sign) { return (X87Reg){ 0, (uint16_t)(sign << 15) }; }

// The NaN an operation with NaN operands gives (x87: a QNaN operand as is, an SNaN quieted;
// two NaNs: a QNaN over an SNaN, else the larger significand, the positive one on a tie); IE for an SNaN.
static X87Reg x87_nan2(X87Reg a, X87Reg b, int a_nan, int b_nan) {
    if (x87_is_snan(a) || x87_is_snan(b)) x87_raise(X_IE);
    if (a_nan && b_nan) {
        const int qa = (a.m >> 62) & 1, qb = (b.m >> 62) & 1;
        if (qa != qb) return qa ? a : b;  // a QNaN over an SNaN
        const uint64_t ma = a.m, mb = b.m;
        if (ma > mb) return x87_quiet(a);
        if (mb > ma) return x87_quiet(b);
        return (a.se >> 15) <= (b.se >> 15) ? x87_quiet(a) : x87_quiet(b);  // tie: the positive one
    }
    return x87_quiet(a_nan ? a : b);
}

// Rounds sign * sig * 2^(e - 127) (sig: bit 127 set, or 0 with sticky) to `prec` bits in a format
// (bias, the largest exponent field, an explicit integer bit or not), with rounding control rc.
// Sets PE, UE (tiny and inexact), OE, C1 (rounded away from zero). Returns the significand
// aligned to its low prec bits and the exponent field in *field (0: zero or denormal).
typedef struct { int32_t bias, emax; int prec; } X87Fmt;
static const X87Fmt FMT80 = { 16383, 0x7ffe, 64 }, FMT64 = { 1023, 0x7fe, 53 }, FMT32 = { 127, 0xfe, 24 };

static uint64_t x87_round(int sign, int32_t e, x87u128 sig, int sticky, int prec, X87Fmt f, int *field, int *ovf) {
    const int rc = (x87e.cw >> 10) & 3;
    int32_t E = e + f.bias;
    int drop = 128 - prec;
    int tiny = 0;
    *ovf = 0;
    if (E < 1) {
        tiny = 1;
        drop += 1 - E;
        E = 1;
    }
    x87u128 kept, rem, half;
    if (drop >= 128) {
        kept = 0;
        rem = sig;
        half = drop == 128 ? (x87u128)1 << 127 : 0;
        if (drop > 128) { sticky |= rem != 0; rem = 0; }  // below half
    } else {
        kept = sig >> drop;
        rem = sig & (((x87u128)1 << drop) - 1);
        half = (x87u128)1 << (drop - 1);
    }
    const int inexact = rem != 0 || sticky;
    int up;
    if (!inexact) up = 0;
    else if (rc == 0) up = rem > half || (rem == half && (sticky || (kept & 1)));
    else if (rc == 1) up = sign;
    else if (rc == 2) up = !sign;
    else up = 0;
    if (drop > 128 && rc == 0) up = 0;  // far below half
    if (up) kept++;
    if (!tiny && kept >> prec) { kept >>= 1; E++; }
    *field = tiny ? (int)((kept >> (prec - 1)) & 1) : E;  // a denormal rounded into the smallest normal
    if (inexact) {
        x87_raise(X_PE);
        if (tiny) x87_raise(X_UE);
    }
    if (up) x87e.sw |= X_C1; else x87e.sw &= ~X_C1;
    if (!tiny && E > f.emax) {
        x87_raise(X_OE | X_PE);
        *ovf = 1;
        const int to_inf = rc == 0 || (rc == 1 && sign) || (rc == 2 && !sign);
        if (to_inf) { x87e.sw |= X_C1; *field = f.emax + 1; return 0; }
        x87e.sw &= ~X_C1;
        *field = f.emax;
        return prec == 64 ? ~0ull : ((uint64_t)1 << prec) - 1;  // the largest finite
    }
    return (uint64_t)kept;
}

static int x87_pc_bits(void) {
    switch ((x87e.cw >> 8) & 3) { case 0: return 24; case 2: return 53; default: return 64; }
}

// An arithmetic result sign * sig * 2^(e - 127), rounded by the precision control, as a register.
static X87Reg x87_pack_p(int sign, int32_t e, x87u128 sig, int sticky, int prec) {
    if (!sig && !sticky) return x87_zero(sign);
    int field, ovf;
    uint64_t k = x87_round(sign, e, sig, sticky, prec, FMT80, &field, &ovf);
    if (ovf && field == 0x7fff) return x87_inf(sign);
    if (ovf) return (X87Reg){ ~0ull << (64 - prec), (uint16_t)(sign << 15 | 0x7ffe) };
    uint64_t m = prec == 64 ? k : k << (64 - prec);
    if (field == 0 && (m >> 63)) field = 1;  // rounded up into the smallest normal
    return (X87Reg){ m, (uint16_t)(sign << 15 | field) };
}
static X87Reg x87_pack(int sign, int32_t e, x87u128 sig, int sticky) { return x87_pack_p(sign, e, sig, sticky, x87_pc_bits()); }
static X87Reg x87_pack_unp(X87Unp u) { return x87_pack(u.sign, u.e, (x87u128)u.m << 64, 0); }

// ---- arithmetic --------------------------------------------------------------------------------
// Operand checks shared by the two-operand operations: NaN, unsupported, denormal (DE).
static int x87_prep(X87Reg a, X87Reg b, X87Unp *ua, X87Unp *ub, X87Reg *out) {
    x87e.sw &= ~X_C1;
    *ua = x87_unpack(a);
    *ub = x87_unpack(b);
    if (ua->cls == CLS_BAD || ub->cls == CLS_BAD) { x87_raise(X_IE); *out = X_INDEF; return 1; }
    if (ua->cls == CLS_NAN || ub->cls == CLS_NAN) { *out = x87_nan2(a, b, ua->cls == CLS_NAN, ub->cls == CLS_NAN); return 1; }
    x87_de_pending = x87_is_denormal(a) || x87_is_denormal(b) || x87_mem_de;
    return 0;
}
static void x87_de(void) {  // the operation goes on with its denormal operands
    if (x87_de_pending) x87_raise(X_DE);
    x87_de_pending = 0;
}

static X87Reg x87_add(X87Reg a, X87Reg b, int negate_b) {
    X87Unp ua, ub;
    X87Reg out;
    if (x87_prep(a, b, &ua, &ub, &out)) return out;
    x87_de();
    ub.sign ^= negate_b;
    const int rc = (x87e.cw >> 10) & 3;
    if (ua.cls == CLS_INF || ub.cls == CLS_INF) {
        if (ua.cls == CLS_INF && ub.cls == CLS_INF && ua.sign != ub.sign) { x87_raise(X_IE); return X_INDEF; }
        return x87_inf(ua.cls == CLS_INF ? ua.sign : ub.sign);
    }
    if (ua.cls == CLS_ZERO && ub.cls == CLS_ZERO)
        return x87_zero(ua.sign == ub.sign ? ua.sign : rc == 1);
    if (ua.cls == CLS_ZERO) return x87_pack_unp(ub);
    if (ub.cls == CLS_ZERO) return x87_pack_unp(ua);
    if (ua.e < ub.e || (ua.e == ub.e && ua.m < ub.m)) { X87Unp t = ua; ua = ub; ub = t; }
    const int d = ua.e - ub.e;
    x87u128 sa = (x87u128)ua.m << 64, sb = (x87u128)ub.m << 64;
    int sticky = 0;
    if (d >= 128) { sticky = 1; sb = 0; }
    else if (d) { sticky = (sb & (((x87u128)1 << d) - 1)) != 0; sb >>= d; }
    int32_t e = ua.e;
    if (ua.sign == ub.sign) {
        x87u128 s = sa + sb;
        if (s < sa) {  // carry out
            sticky |= (int)(s & 1);
            s = (s >> 1) | ((x87u128)1 << 127);
            e++;
        }
        return x87_pack(ua.sign, e, s, sticky);
    }
    x87u128 r = sa - sb - (sticky ? 1 : 0);
    if (!r && !sticky) return x87_zero(rc == 1);
    const int lz = x87_clz128(r);
    return x87_pack(ua.sign, e - lz, r << lz, sticky);
}

static X87Reg x87_mul(X87Reg a, X87Reg b) {
    X87Unp ua, ub;
    X87Reg out;
    if (x87_prep(a, b, &ua, &ub, &out)) return out;
    x87_de();
    const int sign = ua.sign ^ ub.sign;
    if (ua.cls == CLS_INF || ub.cls == CLS_INF) {
        if (ua.cls == CLS_ZERO || ub.cls == CLS_ZERO) { x87_raise(X_IE); return X_INDEF; }
        return x87_inf(sign);
    }
    if (ua.cls == CLS_ZERO || ub.cls == CLS_ZERO) return x87_zero(sign);
    const x87u128 p = (x87u128)ua.m * ub.m;
    const int lz = x87_clz128(p);
    return x87_pack(sign, ua.e + ub.e + 1 - lz, p << lz, 0);
}

static X87Reg x87_div(X87Reg a, X87Reg b) {
    X87Unp ua, ub;
    X87Reg out;
    if (x87_prep(a, b, &ua, &ub, &out)) return out;
    const int sign = ua.sign ^ ub.sign;
    if (ub.cls != CLS_ZERO) x87_de();
    if (ua.cls == CLS_INF) {
        if (ub.cls == CLS_INF) { x87_raise(X_IE); return X_INDEF; }
        return x87_inf(sign);
    }
    if (ub.cls == CLS_INF) return x87_zero(sign);
    if (ub.cls == CLS_ZERO) {
        if (ua.cls == CLS_ZERO) { x87_raise(X_IE); return X_INDEF; }
        x87_raise(X_ZE);
        return x87_inf(sign);
    }
    if (ua.cls == CLS_ZERO) return x87_zero(sign);
    const x87u128 n = (x87u128)ua.m << 64;
    x87u128 q = n / ub.m, r = n % ub.m;
    q = (q << 2) | ((r << 2) / ub.m);
    r = (r << 2) % ub.m;
    const int lz = x87_clz128(q);
    return x87_pack(sign, ua.e - ub.e + 61 - lz, q << lz, r != 0);
}

static X87Reg x87_sqrt(X87Reg a) {
    x87e.sw &= ~X_C1;
    X87Unp u = x87_unpack(a);
    if (u.cls == CLS_BAD) { x87_raise(X_IE); return X_INDEF; }
    if (u.cls == CLS_NAN) return x87_nan2(a, a, 1, 0);
    if (u.cls == CLS_ZERO) return a;
    if (u.sign) { x87_raise(X_IE); return X_INDEF; }
    if (u.cls == CLS_INF) return a;
    if (x87_is_denormal(a)) x87_raise(X_DE);
    int32_t E2 = u.e - 63;
    x87u128 x = u.m;
    int L = 64;
    if (E2 & 1) { x <<= 1; E2--; L = 66; }
    // digit by digit over X = x << (132 - L): a 66-bit root
    x87u128 rem = 0, root = 0;
    for (int j = 0; j < 66; ++j) {
        const int hi = 131 - 2 * j, base = 132 - L;
        unsigned pair = 0;
        for (int k = 0; k < 2; ++k) {
            const int p = hi - k;
            pair = pair << 1 | (unsigned)(p >= base ? (x >> (p - base)) & 1 : 0);
        }
        rem = rem << 2 | pair;
        const x87u128 trial = root << 2 | 1;
        if (rem >= trial) { rem -= trial; root = root << 1 | 1; }
        else root <<= 1;
    }
    const int32_t e = E2 / 2 - (132 - L) / 2 + 65;
    return x87_pack(0, e, root << 62, rem != 0);
}

// Compare: -1 less, 0 equal, 1 greater, 2 unordered. quiet: fucom (IE for SNaN only).
static int x87_cmp(X87Reg a, X87Reg b, int quiet) {
    X87Unp ua = x87_unpack(a), ub = x87_unpack(b);
    if (ua.cls == CLS_BAD || ub.cls == CLS_BAD) { x87_raise(X_IE); return 2; }
    if (ua.cls == CLS_NAN || ub.cls == CLS_NAN) {
        if (!quiet || x87_is_snan(a) || x87_is_snan(b)) x87_raise(X_IE);
        return 2;
    }
    if (x87_is_denormal(a) || x87_is_denormal(b) || x87_mem_de) x87_raise(X_DE);
    if (ua.cls == CLS_ZERO && ub.cls == CLS_ZERO) return 0;
    // order by sign, then magnitude (class, exponent, significand)
    const int sa = ua.cls == CLS_ZERO ? 0 : ua.sign ? -1 : 1, sb = ub.cls == CLS_ZERO ? 0 : ub.sign ? -1 : 1;
    if (sa != sb) return sa < sb ? -1 : 1;
    int mag;  // |a| vs |b|
    if (ua.cls == CLS_INF || ub.cls == CLS_INF) mag = (ua.cls == CLS_INF) - (ub.cls == CLS_INF);
    else if (ua.e != ub.e) mag = ua.e < ub.e ? -1 : 1;
    else mag = ua.m < ub.m ? -1 : ua.m > ub.m;
    return sa < 0 ? -mag : mag;
}

// ---- conversions --------------------------------------------------------------------------------
static X87Reg x87_from_f64(uint64_t v) {
    const int sign = (int)(v >> 63), e = (int)(v >> 52) & 0x7ff;
    const uint64_t f = v & ((1ull << 52) - 1);
    if (e == 0x7ff) {
        if (!f) return x87_inf(sign);
        return (X87Reg){ 1ull << 63 | f << 11, (uint16_t)(sign << 15 | 0x7fff) };  // an SNaN stays one
    }
    if (e == 0) {
        if (!f) return x87_zero(sign);
        x87_mem_de = 1;
        const int s = x87_clz64(f);
        return (X87Reg){ f << s, (uint16_t)(sign << 15 | (1 - 1023 + 16383 - (s - 11))) };
    }
    return (X87Reg){ 1ull << 63 | f << 11, (uint16_t)(sign << 15 | (e - 1023 + 16383)) };
}
static X87Reg x87_from_f32(uint32_t v) {
    const int sign = (int)(v >> 31), e = (int)(v >> 23) & 0xff;
    const uint64_t f = v & ((1u << 23) - 1);
    if (e == 0xff) {
        if (!f) return x87_inf(sign);
        return (X87Reg){ 1ull << 63 | f << 40, (uint16_t)(sign << 15 | 0x7fff) };
    }
    if (e == 0) {
        if (!f) return x87_zero(sign);
        x87_mem_de = 1;
        const int s = x87_clz64(f);
        return (X87Reg){ f << s, (uint16_t)(sign << 15 | (1 - 127 + 16383 - (s - 40))) };
    }
    return (X87Reg){ 1ull << 63 | f << 40, (uint16_t)(sign << 15 | (e - 127 + 16383)) };
}
static X87Reg x87_from_int(int64_t v) {
    if (!v) return x87_zero(0);
    const int sign = v < 0;
    const uint64_t a = sign ? 0 - (uint64_t)v : (uint64_t)v;
    const int s = x87_clz64(a);
    return (X87Reg){ a << s, (uint16_t)(sign << 15 | (63 - s + 16383)) };
}

// To float (bits 32 or 64), rounding by rc; NaN: quieted, truncated; IE for an SNaN.
static uint64_t x87_to_float(X87Reg a, int bits) {
    const X87Fmt f = bits == 64 ? FMT64 : FMT32;
    const int mbits = f.prec - 1, ebits = bits - 1 - mbits;
    X87Unp u = x87_unpack(a);
    x87e.sw &= ~X_C1;
    const uint64_t sign = (uint64_t)u.sign << (bits - 1);
    const uint64_t emask = ((1ull << ebits) - 1) << mbits;
    if (u.cls == CLS_BAD) { x87_raise(X_IE); return (1ull << (bits - 1)) | emask | (1ull << (mbits - 1)); }
    if (u.cls == CLS_NAN) {
        if (x87_is_snan(a)) x87_raise(X_IE);
        return sign | emask | ((a.m << 1) >> (64 - mbits)) | (1ull << (mbits - 1));
    }
    if (u.cls == CLS_INF) return sign | emask;
    if (u.cls == CLS_ZERO) return sign;
    int field, ovf;
    const uint64_t k = x87_round(u.sign, u.e, (x87u128)u.m << 64, 0, f.prec, f, &field, &ovf);
    if (ovf && field == f.emax + 1) return sign | emask;
    return sign | ((uint64_t)field << mbits) | (k & ((1ull << mbits) - 1));
}

// To integer (bits 16 32 64): rc (fist) or toward zero (fisttp); out of range: the integer
// indefinite and IE.
static int64_t x87_to_int(X87Reg a, int bits, int truncate) {
    const int64_t indef_v = bits == 64 ? INT64_MIN : -((int64_t)1 << (bits - 1));  // the integer indefinite
    X87Unp u = x87_unpack(a);
    x87e.sw &= ~X_C1;
    if (u.cls == CLS_BAD || u.cls == CLS_NAN || u.cls == CLS_INF) { x87_raise(X_IE); return indef_v; }
    if (u.cls == CLS_ZERO) { x87e.sw &= ~X_C1; return 0; }
    const int rc = truncate ? 3 : (x87e.cw >> 10) & 3;
    // the integer part and the rest, from m * 2^(e - 63)
    uint64_t ip;
    int up = 0, inexact = 0;
    if (u.e >= 64) { x87_raise(X_IE); return indef_v; }
    if (u.e >= 63) ip = u.m;
    else if (u.e < -1) { ip = 0; inexact = 1; up = rc == 1 ? u.sign : rc == 2 ? !u.sign : 0; }
    else {
        const int sh = 63 - u.e;  // 1..64
        ip = sh == 64 ? 0 : u.m >> sh;
        const uint64_t rem = sh == 64 ? u.m : u.m & ((1ull << sh) - 1);
        const uint64_t half = 1ull << (sh - 1);
        inexact = rem != 0;
        if (inexact) {
            if (rc == 0) up = rem > half || (rem == half && (ip & 1));
            else if (rc == 1) up = u.sign;
            else if (rc == 2) up = !u.sign;
        }
    }
    if (up) { ip++; if (!ip) { x87_raise(X_IE); return indef_v; } }
    const uint64_t lim = u.sign ? 1ull << (bits - 1) : (1ull << (bits - 1)) - 1;
    if (ip > lim) { x87_raise(X_IE); return indef_v; }
    if (inexact) x87_raise(X_PE);
    if (up) x87e.sw |= X_C1; else x87e.sw &= ~X_C1;
    return u.sign ? (int64_t)(0 - ip) : (int64_t)ip;
}

static X87Reg x87_rndint(X87Reg a) {
    x87e.sw &= ~X_C1;
    X87Unp u = x87_unpack(a);
    if (u.cls == CLS_BAD) { x87_raise(X_IE); return X_INDEF; }
    if (u.cls == CLS_NAN) return x87_nan2(a, a, 1, 0);
    if (u.cls != CLS_FIN || u.e >= 63) return a;
    if (x87_is_denormal(a)) x87_raise(X_DE);
    const int rc = (x87e.cw >> 10) & 3;
    uint64_t ip;
    int up = 0, inexact;
    if (u.e < -1) { ip = 0; inexact = 1; up = rc == 1 ? u.sign : rc == 2 ? !u.sign : 0; }
    else {
        const int sh = 63 - u.e;
        ip = sh == 64 ? 0 : u.m >> sh;
        const uint64_t rem = sh == 64 ? u.m : u.m & ((1ull << sh) - 1), half = 1ull << (sh - 1);
        inexact = rem != 0;
        if (inexact) up = rc == 0 ? (rem > half || (rem == half && (ip & 1))) : rc == 1 ? u.sign : rc == 2 ? !u.sign : 0;
    }
    if (inexact) x87_raise(X_PE);
    if (up) { ip++; x87e.sw |= X_C1; } else x87e.sw &= ~X_C1;
    if (!ip) return x87_zero(u.sign);
    X87Reg r = x87_from_int((int64_t)ip);  // ip < 2^63 here
    r.se |= (uint16_t)(u.sign << 15);
    return r;
}

static X87Reg x87_scale(X87Reg a, X87Reg b) {
    X87Unp ua, ub;
    X87Reg out;
    if (x87_prep(a, b, &ua, &ub, &out)) return out;
    x87_de();
    if (ub.cls == CLS_INF) {
        if (!ub.sign) {
            if (ua.cls == CLS_ZERO) { x87_raise(X_IE); return X_INDEF; }
            return ua.cls == CLS_FIN ? x87_inf(ua.sign) : a;
        }
        if (ua.cls == CLS_INF) { x87_raise(X_IE); return X_INDEF; }
        return ua.cls == CLS_FIN ? x87_zero(ua.sign) : a;
    }
    if (ua.cls != CLS_FIN) return a;
    int32_t n = 0;  // trunc(b), clamped
    if (ub.cls == CLS_FIN && ub.e >= 0) n = ub.e > 30 ? (1 << 30) : (int32_t)(ub.m >> (63 - ub.e));
    if (ub.sign) n = -n;
    return x87_pack_p(ua.sign, ua.e + n, (x87u128)ua.m << 64, 0, 64);  // not by the precision control
}

// fprem (toward zero) / fprem1 (to nearest), exact: when the exponents differ by 64 or more, a
// partial remainder (C2 set) that brings the difference down to a multiple of 32 (AMD: by
// N = 32 + d mod 32 quotient bits); else the remainder, with C0 C3 C1 = the quotient's low bits.
static X87Reg x87_prem(X87Reg a, X87Reg b, int nearest) {
    X87Unp ua, ub;
    X87Reg out;
    x87e.sw &= ~X_C2;  // NaN and invalid operands keep C0 C3
    if (x87_prep(a, b, &ua, &ub, &out)) return out;
    if (ua.cls == CLS_INF || ub.cls == CLS_ZERO) { x87_raise(X_IE); return X_INDEF; }
    x87_de();
    x87e.sw &= ~(X_C0 | X_C3);
    if (ua.cls == CLS_ZERO || ub.cls == CLS_INF) return a;
    const int32_t d = ua.e - ub.e;
    if (d >= 64) {
        const int N = 32 + (d & 31), sh = d - N;
        const x87u128 r = ((x87u128)ua.m << N) % ub.m;  // in units of 2^(eb + sh - 63)
        X87Reg res = x87_zero(ua.sign);
        if (r) {
            const int lz = x87_clz128(r);
            res = x87_pack_p(ua.sign, ub.e + sh + 64 - lz, r << lz, 0, 64);
        }
        x87e.sw = (uint16_t)((x87e.sw & ~(X_C0 | X_C1 | X_C3)) | X_C2);
        return res;
    }
    if (d < -1 || (d == -1 && !nearest)) return a;  // |a| < |b| / 2 (or < |b|): quotient 0
    // a and b in units of 2^(eb - 64): a's significand shifted by d + 1 (0..64)
    const x87u128 n = (x87u128)ua.m << (d + 1), mb = (x87u128)ub.m << 1;
    x87u128 q = n / mb, r = n % mb;
    int sign = ua.sign;
    if (nearest && (r << 1 > mb || (r << 1 == mb && (q & 1)))) { r = mb - r; q++; sign ^= 1; }
    X87Reg res = x87_zero(ua.sign);
    if (r) {
        const int lz = x87_clz128(r);
        res = x87_pack_p(sign, ub.e + 63 - lz, r << lz, 0, 64);
    }
    x87e.sw &= ~(X_C0 | X_C1 | X_C3);
    if (q & 4) x87e.sw |= X_C0;
    if (q & 2) x87e.sw |= X_C3;
    if (q & 1) x87e.sw |= X_C1;
    return res;
}

// ---- the instruction ---------------------------------------------------------------------------
static void x87_set_cc(int r) {  // fcom family: C3 C2 C0
    x87e.sw &= ~(X_C0 | X_C2 | X_C3 | X_C1);
    if (r == 2) x87e.sw |= X_C0 | X_C2 | X_C3;
    else if (r == -1) x87e.sw |= X_C0;
    else if (r == 0) x87e.sw |= X_C3;
}
static uint64_t x87_set_eflags(uint64_t flags, int r) {  // fcomi family: ZF PF CF
    flags &= ~(uint64_t)(0x1 | 0x4 | 0x40 | 0x10 | 0x80 | 0x800);
    if (r == 2) flags |= 0x1 | 0x4 | 0x40;
    else if (r == -1) flags |= 0x1;
    else if (r == 0) flags |= 0x40;
    return flags;  // C1 kept (AMD, as the PS4's)
}

static int x87_full(void) {  // a push would overflow: the operand is not looked at
    if (x87_empty(7)) return 0;
    x87_push(X_INDEF);
    return 1;
}
static X87Reg x87_load_mem(int kind, uint64_t addr);
static X87Reg x87_fld_mem(int kind, uint64_t addr) {  // fld m32 / m64: DE for a denormal
    X87Reg v = x87_load_mem(kind, addr);
    if (x87_mem_de) x87_raise(X_DE);
    if (x87_is_snan(v)) { x87_raise(X_IE); v = x87_quiet(v); }
    return v;
}
static X87Reg x87_load_mem(int kind, uint64_t addr) {  // 0 m32fp 1 m32int 2 m64fp 3 m16int
    const void *p = (const void *)(uintptr_t)addr;
    switch (kind) {
    case 0: { uint32_t v; memcpy(&v, p, 4); return x87_from_f32(v); }
    case 1: { int32_t v; memcpy(&v, p, 4); return x87_from_int(v); }
    case 2: { uint64_t v; memcpy(&v, p, 8); return x87_from_f64(v); }
    default: { int16_t v; memcpy(&v, p, 2); return x87_from_int(v); }
    }
}

// The arithmetic group: op 0 add 1 mul 2 com 3 comp 4 sub 5 subr 6 div 7 divr, dst = st(0) or st(i).
static void x87_arith(int op, int dst, X87Reg a, X87Reg b) {
    X87Reg r;
    switch (op) {
    case 0: r = x87_add(a, b, 0); break;
    case 1: r = x87_mul(a, b); break;
    case 4: r = x87_add(a, b, 1); break;
    case 5: r = x87_add(b, a, 1); break;
    case 6: r = x87_div(a, b); break;
    case 7: r = x87_div(b, a); break;
    default: { x87_set_cc(x87_cmp(a, b, 0)); if (op == 3) x87_pop(); return; }
    }
    x87_put(dst, r);
}

static void x87_unemulated(uint64_t enc) {
    fprintf(stderr, "x87 instruction %#llx: not emulated (recomp_x87_emu.h)\n", (unsigned long long)enc);
    abort();
}

static inline uint64_t x87emu_insn(uint64_t enc, uint64_t addr, uint64_t flags) {
    const int op = (int)(enc & 0xff), modrm = (int)((enc >> 8) & 0xff);
    const int reg = (modrm >> 3) & 7, i = modrm & 7;
    void *p = (void *)(uintptr_t)addr;
    x87_mem_de = x87_de_pending = 0;
    if (modrm < 0xc0) {  // memory forms
        switch (op) {
        case 0xD8: case 0xDC: case 0xDA: case 0xDE: {  // arithmetic with m32fp / m64fp / m32int / m16int
            if (!x87_need(0)) { if (reg != 2 && reg != 3) x87_put(0, X_INDEF); else { x87_set_cc(2); if (reg == 3) x87_pop(); } return flags; }
            const X87Reg m = x87_load_mem(op == 0xD8 ? 0 : op == 0xDC ? 2 : op == 0xDA ? 1 : 3, addr);
            x87_arith(reg, 0, x87_get(0), m);
            return flags;
        }
        case 0xD9:
            switch (reg) {
            case 0: if (!x87_full()) x87_push(x87_fld_mem(0, addr)); return flags;
            case 2: case 3: {
                if (!x87_need(0)) { const uint32_t v = 0xffc00000u; memcpy(p, &v, 4); if (reg == 3) x87_pop(); return flags; }
                const uint32_t v = (uint32_t)x87_to_float(x87_get(0), 32);
                memcpy(p, &v, 4);
                if (reg == 3) x87_pop();
                return flags;
            }
            default: break;  // fldenv fldcw fnstenv fnstcw: __recomp_x87 (x87emu_env)
            }
            break;
        case 0xDB:
            switch (reg) {
            case 0: if (!x87_full()) x87_push(x87_load_mem(1, addr)); return flags;
            case 1: case 2: case 3: {
                int32_t v = INT32_MIN;
                if (x87_need(0)) v = (int32_t)x87_to_int(x87_get(0), 32, reg == 1);
                memcpy(p, &v, 4);
                if (reg != 2) x87_pop();
                return flags;
            }
            case 5: { X87Reg v; memcpy(&v.m, p, 8); memcpy(&v.se, (char *)p + 8, 2); x87_push(v); return flags; }
            case 7: {
                const X87Reg v = x87_need(0) ? x87_get(0) : X_INDEF;
                memcpy(p, &v.m, 8);
                memcpy((char *)p + 8, &v.se, 2);
                x87_pop();
                x87e.sw &= ~X_C1;
                return flags;
            }
            default: break;
            }
            break;
        case 0xDD:
            switch (reg) {
            case 0: if (!x87_full()) x87_push(x87_fld_mem(2, addr)); return flags;
            case 1: {
                int64_t v = INT64_MIN;
                if (x87_need(0)) v = x87_to_int(x87_get(0), 64, 1);
                memcpy(p, &v, 8);
                x87_pop();
                return flags;
            }
            case 2: case 3: {
                if (!x87_need(0)) { const uint64_t v = 0xfff8000000000000ull; memcpy(p, &v, 8); if (reg == 3) x87_pop(); return flags; }
                const uint64_t v = x87_to_float(x87_get(0), 64);
                memcpy(p, &v, 8);
                if (reg == 3) x87_pop();
                return flags;
            }
            case 7: memcpy(p, &x87e.sw, 2); return flags;
            default: break;
            }
            break;
        case 0xDF:
            switch (reg) {
            case 0: if (!x87_full()) x87_push(x87_load_mem(3, addr)); return flags;
            case 1: case 2: case 3: {
                int16_t v = INT16_MIN;
                if (x87_need(0)) v = (int16_t)x87_to_int(x87_get(0), 16, reg == 1);
                memcpy(p, &v, 2);
                if (reg != 2) x87_pop();
                return flags;
            }
            case 5: { int64_t v; memcpy(&v, p, 8); x87_push(x87_from_int(v)); return flags; }
            case 7: {
                int64_t v = INT64_MIN;
                if (x87_need(0)) v = x87_to_int(x87_get(0), 64, 0);
                memcpy(p, &v, 8);
                x87_pop();
                return flags;
            }
            default: break;
            }
            break;
        default: break;
        }
        x87_unemulated(enc);
    }
    // register forms
    switch (op) {
    case 0xD8:  // st0 = st0 op st(i)
        if (!x87_need(0) || !x87_need(i)) { if (reg != 2 && reg != 3) x87_put(0, X_INDEF); else { x87_set_cc(2); if (reg == 3) x87_pop(); } return flags; }
        x87_arith(reg, 0, x87_get(0), x87_get(i));
        return flags;
    case 0xDC: case 0xDE: {  // st(i) = st(i) op st0 (DC: sub/subr and div/divr named the other way)
        static const int map[8] = { 0, 1, 2, 3, 5, 4, 7, 6 };
        if (op == 0xDE && reg == 3 && i == 1) {  // fcompp
            const int r = x87_need(0) && x87_need(1) ? x87_cmp(x87_get(0), x87_get(1), 0) : 2;
            x87_set_cc(r);
            x87_pop(); x87_pop();
            return flags;
        }
        if (!x87_need(0) || !x87_need(i)) { x87_put(i, X_INDEF); if (op == 0xDE) x87_pop(); return flags; }
        if (reg == 2 || reg == 3) {  // DC D0/D8: fcom/fcomp (alias forms)
            x87_set_cc(x87_cmp(x87_get(0), x87_get(i), 0));
            if (reg == 3) x87_pop();
            return flags;
        }
        x87_arith(map[reg], i, x87_get(i), x87_get(0));
        if (op == 0xDE) x87_pop();
        return flags;
    }
    case 0xD9:
        switch (modrm) {
        case 0xD0: return flags;  // fnop
        case 0xE0: if (x87_need(0)) { X87Reg v = x87_get(0); v.se ^= 0x8000; x87_put(0, v); x87e.sw &= ~X_C1; } else x87_put(0, X_INDEF); return flags;
        case 0xE1: if (x87_need(0)) { X87Reg v = x87_get(0); v.se &= 0x7fff; x87_put(0, v); x87e.sw &= ~X_C1; } else x87_put(0, X_INDEF); return flags;
        case 0xE4: x87_set_cc(x87_need(0) ? x87_cmp(x87_get(0), x87_zero(0), 0) : 2); return flags;  // ftst
        case 0xE5: {  // fxam
            x87e.sw &= ~(X_C0 | X_C1 | X_C2 | X_C3);
            const X87Reg v = x87_get(0);
            if (v.se >> 15) x87e.sw |= X_C1;
            if (x87_empty(0)) { x87e.sw |= X_C3 | X_C0; return flags; }
            const X87Unp u = x87_unpack(v);
            if (u.cls == CLS_BAD) return flags;
            if (u.cls == CLS_NAN) x87e.sw |= X_C0;
            else if (u.cls == CLS_INF) x87e.sw |= X_C2 | X_C0;
            else if (u.cls == CLS_ZERO) x87e.sw |= X_C3;
            else if (x87_is_denormal(v)) x87e.sw |= X_C3 | X_C2;
            else x87e.sw |= X_C2;
            return flags;
        }
        case 0xE8: x87_push((X87Reg){ 1ull << 63, 16383 }); return flags;  // fld1
        case 0xEE: x87_push(x87_zero(0)); return flags;  // fldz
        case 0xE9: case 0xEA: case 0xEB: case 0xEC: case 0xED: {  // fldl2t fldl2e fldpi fldlg2 fldln2
            static const X87Reg k[5] = { { 0xD49A784BCD1B8AFEull, 0x4000 }, { 0xB8AA3B295C17F0BCull, 0x3FFF },
                                         { 0xC90FDAA22168C235ull, 0x4000 }, { 0x9A209A84FBCFF799ull, 0x3FFD },
                                         { 0xB17217F7D1CF79ACull, 0x3FFE } };
            // the next bit past the 64 (rounding the constant down or up by rc):
            static const int next_up[5] = { 0, 1, 1, 1, 1 };
            X87Reg v = k[modrm - 0xE9];
            const int rc = (x87e.cw >> 10) & 3;
            if (next_up[modrm - 0xE9] ? (rc == 1 || rc == 3) : rc == 2) v.m += next_up[modrm - 0xE9] ? (uint64_t)-1 : 1;
            x87_push(v);
            return flags;
        }
        case 0xF4: {  // fxtract
            if (!x87_need(0) || !x87_empty(7)) { x87_put(0, X_INDEF); x87_push(X_INDEF); return flags; }  // underflow, overflow
            const X87Reg v = x87_get(0);
            x87e.sw &= ~X_C1;
            X87Unp u = x87_unpack(v);
            if (u.cls == CLS_BAD) { x87_raise(X_IE); x87_put(0, X_INDEF); x87_push(X_INDEF); return flags; }
            if (u.cls == CLS_NAN) { const X87Reg q = x87_nan2(v, v, 1, 0); x87_put(0, q); x87_push(q); return flags; }
            if (u.cls == CLS_ZERO) { x87_raise(X_ZE); x87_put(0, x87_inf(1)); x87_push(v); return flags; }
            if (u.cls == CLS_INF) { x87_put(0, x87_inf(0)); x87_push(v); return flags; }
            if (x87_is_denormal(v)) x87_raise(X_DE);
            x87_put(0, x87_from_int(u.e));
            x87_push((X87Reg){ u.m, (uint16_t)(u.sign << 15 | 16383) });
            return flags;
        }
        case 0xF5: case 0xF8:  // fprem1 fprem
            if (!x87_need(0) || !x87_need(1)) { x87e.sw &= ~X_C2; x87_put(0, X_INDEF); return flags; }
            x87_put(0, x87_prem(x87_get(0), x87_get(1), modrm == 0xF5));
            return flags;
        case 0xF6: x87_set_top(x87_top() - 1); x87e.sw &= ~X_C1; return flags;  // fdecstp
        case 0xF7: x87_set_top(x87_top() + 1); x87e.sw &= ~X_C1; return flags;  // fincstp
        case 0xFA: if (x87_need(0)) x87_put(0, x87_sqrt(x87_get(0))); else x87_put(0, X_INDEF); return flags;
        case 0xFC: if (x87_need(0)) x87_put(0, x87_rndint(x87_get(0))); else x87_put(0, X_INDEF); return flags;
        case 0xFD:
            if (!x87_need(0) || !x87_need(1)) { x87_put(0, X_INDEF); return flags; }
            x87_put(0, x87_scale(x87_get(0), x87_get(1)));
            return flags;
        default: break;
        }
        if (modrm >= 0xC0 && modrm < 0xC8) {  // fld st(i)
            if (!x87_need(i)) { x87_push(X_INDEF); return flags; }
            const X87Reg v = x87_get(i);
            x87_push(v);
            return flags;
        }
        if (modrm >= 0xC8 && modrm < 0xD0) {  // fxch st(i)
            const int e0 = x87_empty(0), ei = x87_empty(i);
            if (e0 || ei) x87_raise(X_IE | X_SF);
            const X87Reg a = e0 ? X_INDEF : x87_get(0), b = ei ? X_INDEF : x87_get(i);
            x87_put(0, b);
            x87_put(i, a);
            x87e.sw &= ~X_C1;
            return flags;
        }
        break;
    case 0xDA: case 0xDB: {
        if (op == 0xDA && modrm == 0xE9) {  // fucompp
            const int r = x87_need(0) && x87_need(1) ? x87_cmp(x87_get(0), x87_get(1), 1) : 2;
            x87_set_cc(r);
            x87_pop(); x87_pop();
            return flags;
        }
        if (op == 0xDB && (modrm == 0xE2 || modrm == 0xE3)) {  // fnclex / fninit: x87emu_env
            break;
        }
        if (op == 0xDB && modrm >= 0xE8 && modrm < 0xF8) {  // fucomi / fcomi
            const int r = x87_need(0) && x87_need(i) ? x87_cmp(x87_get(0), x87_get(i), modrm < 0xF0) : 2;
            return x87_set_eflags(flags, r);
        }
        if (modrm < 0xE0) {  // fcmovcc
            const int cf = flags & 1, zf = (flags >> 6) & 1, pf = (flags >> 2) & 1;
            int c;
            switch (reg & 3) { case 0: c = cf; break; case 1: c = zf; break; case 2: c = cf | zf; break; default: c = pf; }
            if (op == 0xDB) c = !c;
            if (!x87_need(0) || !x87_need(i)) { x87_put(0, X_INDEF); return flags; }
            if (c) x87_put(0, x87_get(i));
            return flags;
        }
        break;
    }
    case 0xDD:
        if (modrm >= 0xC0 && modrm < 0xC8) { x87_free(i); x87e.sw &= ~X_C1; return flags; }  // ffree
        if (modrm >= 0xD0 && modrm < 0xE0) {  // fst / fstp st(i)
            const X87Reg v = x87_need(0) ? x87_get(0) : X_INDEF;
            x87_put(i, v);
            if (modrm >= 0xD8) x87_pop();
            x87e.sw &= ~X_C1;
            return flags;
        }
        if (modrm >= 0xE0 && modrm < 0xF0) {  // fucom / fucomp
            x87_set_cc(x87_need(0) && x87_need(i) ? x87_cmp(x87_get(0), x87_get(i), 1) : 2);
            if (modrm >= 0xE8) x87_pop();
            return flags;
        }
        break;
    case 0xDF:
        if (modrm >= 0xE8 && modrm < 0xF8) {  // fucomip / fcomip
            const int r = x87_need(0) && x87_need(i) ? x87_cmp(x87_get(0), x87_get(i), modrm < 0xF0) : 2;
            flags = x87_set_eflags(flags, r);
            x87_pop();
            return flags;
        }
        if (modrm >= 0xC0 && modrm < 0xC8) { x87_free(i); x87_pop(); x87e.sw &= ~X_C1; return flags; }  // ffreep
        break;
    default: break;
    }
    x87_unemulated(enc);
    return flags;
}

// The environment ops (__recomp_x87's op numbers).
static inline uint64_t x87emu_env(int op, uint64_t addr) {
    void *p = (void *)(uintptr_t)addr;
    uint16_t env[14] = { 0 };
    switch (op) {
    case 0:  // fnstenv: then every exception masked
        env[0] = x87e.cw; env[1] = 0xffff; env[2] = x87e.sw; env[3] = 0xffff; env[4] = x87e.tag; env[5] = 0xffff;
        memcpy(p, env, 28);
        x87e.cw |= 0x3f;
        break;
    case 1:  // the tags: empty or not as given, the rest from the registers (as the hardware)
        memcpy(env, p, 28);
        x87e.cw = env[0];
        x87e.sw = env[2];
        x87e.tag = 0;
        for (int r = 0; r < 8; ++r)
            x87e.tag |= (uint16_t)((((env[4] >> (2 * r)) & 3) == 3 ? 3u : x87_tag_of(x87e.r[r])) << (2 * r));
        break;
    case 2: memcpy(p, &x87e.cw, 2); break;
    case 3: memcpy(&x87e.cw, p, 2); break;
    case 4: memcpy(p, &x87e.sw, 2); break;
    case 6: x87e.sw &= 0x7f00; break;  // fnclex: exception flags, SF, ES, B
    case 7: x87e.cw = 0x037f; x87e.sw = 0; x87e.tag = 0xffff; break;  // fninit
    default: break;
    }
    return x87e.sw;
}
