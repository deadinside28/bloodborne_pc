// SPDX-License-Identifier: GPL-2.0-or-later
// The x87 environment for recompiled code (recomp/lift.py X87_OPS): control and status words,
// no register stack. uint64_t __recomp_x87(RecompState *st, int32_t op, uint64_t addr), op:
// 0 fnstenv m28, 1 fldenv m28, 2 fnstcw m16, 3 fldcw m16, 4 fnstsw m16, 5 fnstsw ax, 6 fnclex,
// 7 fninit, 8 fxsave's x87 part (FCW, FSW, abridged tags, ST0-7, MXCSR_MASK; FOP/FIP/FDP as 0;
// the caller stores MXCSR and the XMM registers); returns the status word. Include in exactly one translation unit of a runtime.
//
// x86-64 hosts run the instruction itself, so original code still running next to the recompiled
// code (x87 in libc) sees the same FPU. Other hosts (or -DRECOMP_X87_EMU) emulate the x87 per
// thread: recomp_x87_emu.h, checked bit for bit against the hardware by x87_fuzz.c.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_rt.h"

#if !defined(__x86_64__) && !defined(RECOMP_X87_EMU)
#define RECOMP_X87_EMU 1
#endif
#if defined(RECOMP_X87_EMU)
#include "recomp_x87_emu.h"
#endif

uint64_t __recomp_x87(RecompState *st, int32_t op, uint64_t addr);
uint64_t __recomp_x87_insn(RecompState *st, uint64_t enc, uint64_t addr, uint64_t flags);

// Any other x87 instruction (recomp/lift.py x87_encoding): enc packs its opcode and ModRM with
// the memory operand at [rdi] (bytes little-endian, the length in the top byte). x86-64 hosts
// run it as itself: a small routine per encoding, made once (push rsi; popfq; the instruction;
// pushfq; pop rax; ret), called with rdi = the address and rsi = the guest's flags; the flags
// after it come back (fcomi/fucomi). Other hosts: the emulator.
#if !defined(RECOMP_X87_EMU)
#include <pthread.h>
#include <sys/mman.h>
typedef uint64_t (*X87Routine)(uint64_t addr, uint64_t flags);
static struct { _Atomic uint64_t enc; X87Routine fn; } x87_routines[1024];
static uint8_t *x87_code;
static size_t x87_code_used;
static pthread_mutex_t x87_lock = PTHREAD_MUTEX_INITIALIZER;

static X87Routine x87_routine(uint64_t enc) {
    uint64_t s = (enc * 0x9e3779b97f4a7c15ull) >> 54;
    for (unsigned n = 0; n < 1024; ++n, s = (s + 1) & 1023) {
        const uint64_t e = __atomic_load_n(&x87_routines[s].enc, __ATOMIC_ACQUIRE);
        if (e == enc) return x87_routines[s].fn;
        if (e) continue;
        pthread_mutex_lock(&x87_lock);
        const uint64_t now = __atomic_load_n(&x87_routines[s].enc, __ATOMIC_RELAXED);
        if (now) {  // taken meanwhile: this encoding, or the probe goes on
            X87Routine fn = now == enc ? x87_routines[s].fn : NULL;
            pthread_mutex_unlock(&x87_lock);
            if (fn) return fn;
            continue;
        }
        if (!x87_code) {
            void *p = mmap(NULL, 1 << 16, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            x87_code = p == MAP_FAILED ? NULL : p;
        }
        const unsigned len = (unsigned)(enc >> 56);
        if (!x87_code || x87_code_used + len + 6 > (1 << 16) || len > 4) {
            pthread_mutex_unlock(&x87_lock);
            return NULL;
        }
        uint8_t *c = x87_code + x87_code_used;
        size_t k = 0;
        c[k++] = 0x56;                                         // push rsi
        c[k++] = 0x9d;                                         // popfq: the guest's flags
        for (unsigned i = 0; i < len; ++i) c[k++] = (uint8_t)(enc >> (8 * i));
        c[k++] = 0x9c;                                         // pushfq
        c[k++] = 0x58;                                         // pop rax: the flags after it
        c[k++] = 0xc3;                                         // ret
        x87_code_used += (k + 15) & ~(size_t)15;
        x87_routines[s].fn = (X87Routine)(void *)c;
        __atomic_store_n(&x87_routines[s].enc, enc, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&x87_lock);
        return (X87Routine)(void *)c;
    }
    return NULL;
}
#endif

uint64_t __recomp_x87_insn(RecompState *st, uint64_t enc, uint64_t addr, uint64_t flags) {
    (void)st;
#if defined(RECOMP_X87_EMU)
    return x87emu_insn(enc, addr, flags);
#else
    const X87Routine fn = x87_routine(enc);
    if (fn) return fn(addr, flags);
    fprintf(stderr, "x87 instruction %#llx: no room for its routine\n", (unsigned long long)enc);
    abort();
#endif
}

uint64_t __recomp_x87(RecompState *st, int32_t op, uint64_t addr) {
    (void)st;
#if defined(RECOMP_X87_EMU)
    return x87emu_env(op, addr);
#else
    void *p = (void *)(uintptr_t)addr;
    uint16_t sw;
    switch (op) {
    case 0: __asm__ volatile("fnstenv (%0)" ::"r"(p) : "memory"); break;
    case 1: __asm__ volatile("fldenv (%0)" ::"r"(p) : "memory"); break;
    case 2: __asm__ volatile("fnstcw (%0)" ::"r"(p) : "memory"); break;
    case 3: __asm__ volatile("fldcw (%0)" ::"r"(p) : "memory"); break;
    case 4: __asm__ volatile("fnstsw (%0)" ::"r"(p) : "memory"); break;
    case 6: __asm__ volatile("fnclex" ::: "memory"); break;
    case 7: __asm__ volatile("fninit" ::: "memory"); break;
    case 8: {
        uint8_t area[512] __attribute__((aligned(16)));
        __asm__ volatile("fxsave64 %0" : "=m"(area));
        memset(area + 6, 0, 18);  // FOP, FIP, FDP: they would name host code
        memcpy(p, area, 24);
        memcpy((char *)p + 28, area + 28, 160 - 28);
        break;
    }
    default: break;
    }
    __asm__ volatile("fnstsw %0" : "=a"(sw));
    return sw;
#endif
}
