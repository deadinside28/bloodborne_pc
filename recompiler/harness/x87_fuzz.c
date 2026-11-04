// SPDX-License-Identifier: GPL-2.0-or-later
// recomp_x87_emu.h against the hardware (x86-64 only): every instruction form the emulator takes,
// from random states (stack depth, TOP, rounding and precision control, sticky flags, condition
// codes, guest EFLAGS) with random and edge-case operands, run once on the x87 (frstor; the
// instruction; fnsave) and once emulated; the control, status and tag words, the eight registers,
// the memory operand and the flags must come out the same.
//
//   cc -O2 -o x87_fuzz x87_fuzz.c && ./x87_fuzz [iterations per form] [seed] [only: hex enc]
// Exceptions stay masked (the emulator takes them so).
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "recomp_x87_emu.h"

static uint64_t rng = 0x243f6a8885a308d3ull;
static uint64_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}
static unsigned rn(unsigned n) { return (unsigned)(rnd() % n); }

// ---- operands -----------------------------------------------------------------------------------
static X87Reg val80(void) {
    X87Reg v;
    const int sign = rn(2);
    switch (rn(16)) {
    case 0: v = x87_zero(sign); break;
    case 1: v = x87_inf(sign); break;
    case 2: v = (X87Reg){ 1ull << 63 | rnd() >> 2 | 1 << 0, 0x7fff }; v.m |= 1ull << 62; break;  // QNaN
    case 3: v = (X87Reg){ 1ull << 63 | (rnd() >> 3 | 1), 0x7fff }; v.m &= ~(1ull << 62); break;  // SNaN
    case 4: v = (X87Reg){ rnd() >> (1 + rn(63)), 0 }; if (!v.m) v.m = 1; break;  // denormal
    case 5: v = (X87Reg){ rnd() | 1ull << 63, (uint16_t)(1 + rn(80)) }; break;  // tiny
    case 6: v = (X87Reg){ rnd() | 1ull << 63, (uint16_t)(0x7ffe - rn(80)) }; break;  // huge
    case 7: v = x87_from_int((int64_t)(rnd() >> rn(64))); break;  // integers
    case 8: v = (X87Reg){ (rnd() | 1ull << 63) & ~0ull << rn(64), (uint16_t)(16383 + rn(70) - 4) }; break;  // few bits: halfway cases
    case 9: v = (X87Reg){ rnd() | 1ull << 63, (uint16_t)(16383 - 1 + rn(3)) }; break;  // around 1
    case 10: v = (X87Reg){ 1ull << 63 | (rnd() & 0x7ff), (uint16_t)(16383 + rn(64)) }; break;  // near f64 / integer edges
    case 11: v = (X87Reg){ ~0ull - (rnd() & 0xfff), (uint16_t)(16383 + rn(20)) }; break;  // just below powers of two
    default: v = (X87Reg){ rnd() | 1ull << 63, (uint16_t)(16383 - 40 + rn(80)) }; break;  // ordinary
    }
    v.se |= (uint16_t)(sign << 15);
    return v;
}
static uint64_t valf64(void) {
    switch (rn(8)) {
    case 0: return rnd();
    case 1: return (uint64_t)rn(2) << 63 | 0x7ff0000000000000ull | (rn(2) ? 0 : rnd() >> 12);
    case 2: return (uint64_t)rn(2) << 63 | rnd() >> 12;  // denormal
    case 3: return (uint64_t)rn(2) << 63;
    default: return (uint64_t)rn(2) << 63 | (uint64_t)(1023 - 40 + rn(80)) << 52 | rnd() >> 12;
    }
}
static uint32_t valf32(void) {
    switch (rn(8)) {
    case 0: return (uint32_t)rnd();
    case 1: return (uint32_t)rn(2) << 31 | 0x7f800000u | (rn(2) ? 0 : (uint32_t)rnd() >> 9);
    case 2: return (uint32_t)rn(2) << 31 | (uint32_t)rnd() >> 9;
    case 3: return (uint32_t)rn(2) << 31;
    default: return (uint32_t)rn(2) << 31 | (uint32_t)(127 - 20 + rn(40)) << 23 | (uint32_t)rnd() >> 9;
    }
}
static int64_t vali(void) { return (int64_t)rnd() >> rn(64); }

// ---- the forms ----------------------------------------------------------------------------------
// The memory operand kind of a memory form: 0 none (a store) 1 f32 2 f64 3 f80 4 i16 5 i32 6 i64.
static int mem_kind(int op, int reg) {
    switch (op) {
    case 0xD8: return 1;
    case 0xDA: return 5;
    case 0xDC: return 2;
    case 0xDE: return 4;
    case 0xD9: return reg == 0 ? 1 : 0;
    case 0xDB: return reg == 0 ? 5 : reg == 5 ? 3 : 0;
    case 0xDD: return reg == 0 ? 2 : 0;
    case 0xDF: return reg == 0 ? 4 : reg == 5 ? 6 : 0;
    default: return 0;
    }
}
static int emulated(int op, int modrm) {
    const int reg = (modrm >> 3) & 7;
    if (modrm < 0xc0) {
        switch (op) {
        case 0xD9: return reg == 0 || reg == 2 || reg == 3;
        case 0xDB: return reg != 4 && reg != 6;
        case 0xDD: return reg <= 3 || reg == 7;
        case 0xDF: return reg != 4 && reg != 6;
        default: return 1;
        }
    }
    switch (op) {
    case 0xD8: return 1;
    case 0xD9:
        if (modrm < 0xD0) return 1;
        switch (modrm) {
        case 0xD0: case 0xE0: case 0xE1: case 0xE4: case 0xE5: case 0xE8: case 0xE9: case 0xEA: case 0xEB:
        case 0xEC: case 0xED: case 0xEE: case 0xF4: case 0xF5: case 0xF6: case 0xF7: case 0xF8: case 0xFA:
        case 0xFC: case 0xFD: return 1;
        default: return 0;
        }
    case 0xDA: return modrm < 0xE0 || modrm == 0xE9;
    case 0xDB: return modrm < 0xE0 || (modrm >= 0xE8 && modrm < 0xF8);
    case 0xDC: return modrm < 0xD0 || modrm >= 0xE0;
    case 0xDD: return modrm < 0xC8 || (modrm >= 0xD0 && modrm < 0xF0);
    case 0xDE: return modrm < 0xD0 || modrm == 0xD9 || modrm >= 0xE0;
    case 0xDF: return modrm < 0xC8 || (modrm >= 0xE8 && modrm < 0xF8);
    default: return 0;
    }
}

// ---- the hardware -------------------------------------------------------------------------------
// frstor [rdx]; push rsi; popfq; the instruction; pushfq; pop rax; fnsave [rcx]; ret
typedef uint64_t (*HwFn)(uint64_t addr, uint64_t flags, const void *in, void *out);
static uint8_t *code;
static HwFn hw_routine(int op, int modrm) {
    static HwFn made[8][256];
    if (made[op - 0xD8][modrm]) return made[op - 0xD8][modrm];
    static size_t used;
    uint8_t *c = code + used;
    size_t k = 0;
    c[k++] = 0xDD; c[k++] = 0x22;  // frstor [rdx]
    c[k++] = 0x56; c[k++] = 0x9d;
    c[k++] = (uint8_t)op; c[k++] = (uint8_t)(modrm < 0xc0 ? (modrm & 0x38) | 7 : modrm);
    c[k++] = 0x9c; c[k++] = 0x58;
    c[k++] = 0xDD; c[k++] = 0x31;  // fnsave [rcx]
    c[k++] = 0xc3;
    used += 16;
    return made[op - 0xD8][modrm] = (HwFn)(void *)c;
}

#pragma pack(push, 1)
typedef struct { uint16_t cw, p0, sw, p1, tag, p2; uint32_t ip, cs, dp, ds; uint8_t st[8][10]; } FSave;
#pragma pack(pop)

static void reg_bytes(uint8_t *b, X87Reg r) { memcpy(b, &r.m, 8); memcpy(b + 8, &r.se, 2); }
static X87Reg bytes_reg(const uint8_t *b) { X87Reg r; memcpy(&r.m, b, 8); memcpy(&r.se, b + 8, 2); return r; }

int main(int argc, char **argv) {
    const long iters = argc > 1 ? atol(argv[1]) : 20000;
    if (argc > 2) rng = strtoull(argv[2], NULL, 0) | 1;
    const long only = argc > 3 ? strtol(argv[3], NULL, 16) : -1;
    code = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) return 1;
    long forms = 0, bad_forms = 0, runs = 0;
    for (int op = 0xD8; op <= 0xDF; ++op)
        for (int modrm = 0; modrm < 256; ++modrm) {
            if (modrm < 0xc0 && (modrm & 0xc7) != 0x07) continue;  // one memory form per reg: [rdi]
            if (!emulated(op, modrm)) continue;
            const unsigned enc = (unsigned)(op | modrm << 8);
            if (only >= 0 && enc != (unsigned)only) continue;
            const HwFn hw = hw_routine(op, modrm);
            const int kind = modrm < 0xc0 ? mem_kind(op, (modrm >> 3) & 7) : 0;
            ++forms;
            int shown = 0;
            long fails = 0;
            for (long it = 0; it < iters; ++it) {
                // the state
                FSave in;
                memset(&in, 0, sizeof in);
                const int pc = (int[]){ 0, 2, 3 }[rn(3)], rc = rn(4);
                in.cw = (uint16_t)(0x7f | pc << 8 | rc << 10);
                const int top = rn(8), depth = rn(9);
                in.sw = (uint16_t)((rnd() & 0x3f & (rn(2) ? 0 : 0x3f)) | (rnd() & (X_C0 | X_C1 | X_C2 | X_C3)) | top << 11);
                X87Reg phys[8];
                uint16_t tag = 0;
                for (int p = 0; p < 8; ++p) {
                    phys[p] = val80();
                    const int i = (p - top) & 7;  // st(i)
                    tag |= (uint16_t)((i < depth ? x87_tag_of(phys[p]) : 3u) << (2 * p));
                }
                // a few equal operands: compares, subtractions
                if (depth >= 2 && rn(8) == 0) phys[(top + 1) & 7] = phys[top];
                tag = 0;
                for (int p = 0; p < 8; ++p) tag |= (uint16_t)((((p - top) & 7) < depth ? x87_tag_of(phys[p]) : 3u) << (2 * p));
                in.tag = tag;
                for (int i = 0; i < 8; ++i) reg_bytes(in.st[i], phys[(top + i) & 7]);
                uint8_t mem[16] __attribute__((aligned(16)));
                for (int b = 0; b < 16; ++b) mem[b] = (uint8_t)rnd();
                switch (kind) {
                case 1: { uint32_t v = valf32(); memcpy(mem, &v, 4); break; }
                case 2: { uint64_t v = valf64(); memcpy(mem, &v, 8); break; }
                case 3: reg_bytes(mem, val80()); break;
                case 4: { int16_t v = (int16_t)vali(); memcpy(mem, &v, 2); break; }
                case 5: { int32_t v = (int32_t)vali(); memcpy(mem, &v, 4); break; }
                case 6: { int64_t v = vali(); memcpy(mem, &v, 8); break; }
                default: break;
                }
                const uint64_t flags = (rnd() & 0x8d5) | 2;
                // the hardware
                FSave out;
                uint8_t hmem[16] __attribute__((aligned(16)));
                memcpy(hmem, mem, 16);
                const uint64_t hflags = hw((uint64_t)(uintptr_t)hmem, flags, &in, &out) & 0x8d5;
                // the emulator
                x87e.cw = in.cw; x87e.sw = in.sw; x87e.tag = in.tag;
                for (int p = 0; p < 8; ++p) x87e.r[p] = phys[p];
                uint8_t emem[16] __attribute__((aligned(16)));
                memcpy(emem, mem, 16);
                const uint64_t eflags = x87emu_insn(enc | 2ull << 56, (uint64_t)(uintptr_t)emem, flags) & 0x8d5;
                // compare
                const int etop = x87_top();
                int diff = out.cw != x87e.cw || out.sw != x87e.sw || out.tag != x87e.tag || hflags != eflags || memcmp(hmem, emem, 16);
                for (int i = 0; i < 8 && !diff; ++i) {
                    const X87Reg e = x87e.r[(etop + i) & 7];
                    uint8_t b[10];
                    reg_bytes(b, e);
                    diff = memcmp(b, out.st[i], 10) != 0;
                }
                ++runs;
                if (!diff) continue;
                ++fails;
                if (shown++ >= 3) continue;
                printf("%02X %02X: cw %04x sw %04x tag %04x flags %03" PRIx64 "\n", op, modrm, in.cw, in.sw, in.tag, flags);
                for (int i = 0; i < 2 && i < 8; ++i) {
                    const X87Reg r = phys[(top + i) & 7];
                    printf("    in  st%d %04x %016" PRIx64 "%s\n", i, r.se, r.m, i < depth ? "" : " (empty)");
                }
                if (kind) { printf("    in  mem "); for (int b = 9; b >= 0; --b) printf("%02x", mem[b]); printf("\n"); }
                printf("    hw  sw %04x tag %04x flags %03" PRIx64 " mem ", out.sw, out.tag, hflags);
                for (int b = 9; b >= 0; --b) printf("%02x", hmem[b]);
                printf("\n    emu sw %04x tag %04x flags %03" PRIx64 " mem ", x87e.sw, x87e.tag, eflags);
                for (int b = 9; b >= 0; --b) printf("%02x", emem[b]);
                printf("\n");
                for (int i = 0; i < 2; ++i) {
                    const X87Reg h = bytes_reg(out.st[i]), e = x87e.r[(etop + i) & 7];
                    printf("    st%d hw %04x %016" PRIx64 "  emu %04x %016" PRIx64 "%s\n", i, h.se, h.m, e.se, e.m,
                           h.se != e.se || h.m != e.m ? "  <--" : "");
                }
            }
            if (fails) {
                ++bad_forms;
                printf("FAIL %02X %02X: %ld of %ld\n", op, modrm, fails, iters);
            }
        }
    printf("%ld forms, %ld runs, %ld forms differ\n", forms, runs, bad_forms);
    return bad_forms != 0;
}
