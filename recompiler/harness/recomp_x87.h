// SPDX-License-Identifier: GPL-2.0-or-later
// The x87 environment for recompiled code (recomp/lift.py X87_OPS): control and status words,
// no register stack. uint64_t __recomp_x87(RecompState *st, int32_t op, uint64_t addr), op:
// 0 fnstenv m28, 1 fldenv m28, 2 fnstcw m16, 3 fldcw m16, 4 fnstsw m16, 5 fnstsw ax, 6 fnclex,
// 7 fninit; returns the status word. Include in exactly one translation unit of a runtime.
//
// x86-64 hosts run the instruction itself, so original code still running next to the recompiled
// code (x87 in libc) sees the same FPU. Other hosts keep the two words per thread.
#pragma once
#include <stdint.h>
#include <string.h>

#include "recomp_rt.h"

uint64_t __recomp_x87(RecompState *st, int32_t op, uint64_t addr);

uint64_t __recomp_x87(RecompState *st, int32_t op, uint64_t addr) {
    (void)st;
    void *p = (void *)(uintptr_t)addr;
#if defined(__x86_64__)
    uint16_t sw;
    switch (op) {
    case 0: __asm__ volatile("fnstenv (%0)" ::"r"(p) : "memory"); break;
    case 1: __asm__ volatile("fldenv (%0)" ::"r"(p) : "memory"); break;
    case 2: __asm__ volatile("fnstcw (%0)" ::"r"(p) : "memory"); break;
    case 3: __asm__ volatile("fldcw (%0)" ::"r"(p) : "memory"); break;
    case 4: __asm__ volatile("fnstsw (%0)" ::"r"(p) : "memory"); break;
    case 6: __asm__ volatile("fnclex" ::: "memory"); break;
    case 7: __asm__ volatile("fninit" ::: "memory"); break;
    default: break;
    }
    __asm__ volatile("fnstsw %0" : "=a"(sw));
    return sw;
#else
    // The words alone: the tag word reads empty, pointers zero (no x87 code runs here).
    static _Thread_local uint16_t cw = 0x037f, sw;
    uint16_t env[14] = { 0 };
    switch (op) {
    case 0:
        env[0] = cw; env[2] = sw; env[4] = 0xffff;
        memcpy(p, env, 28);
        cw |= 0x3f;  // fnstenv masks every exception after storing
        break;
    case 1: memcpy(env, p, 28); cw = env[0]; sw = env[2]; break;
    case 2: memcpy(p, &cw, 2); break;
    case 3: memcpy(&cw, p, 2); break;
    case 4: memcpy(p, &sw, 2); break;
    case 6: sw &= 0x7f00; break;  // exception flags, ES and B cleared
    case 7: cw = 0x037f; sw = 0; break;
    default: break;
    }
    return sw;
#endif
}
