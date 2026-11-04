// SPDX-License-Identifier: GPL-2.0-or-later
// The recompiled code's view of the guest CPU. Must match STATE_TYPE in recomp/lift.py:
//   %State = type { [16 x i64], i64, i64, i64, i32, i32, [16 x i128], [16 x i128] }
#pragma once
#include <stdint.h>

enum { G_RAX, G_RCX, G_RDX, G_RBX, G_RSP, G_RBP, G_RSI, G_RDI,
       G_R8, G_R9, G_R10, G_R11, G_R12, G_R13, G_R14, G_R15 };

typedef struct RecompState {
    uint64_t gpr[16];
    uint64_t rip;
    uint64_t fs_base, gs_base;
    uint32_t mxcsr, pad;
    unsigned __int128 xmm[16];   // bits 0..127 of xmm/ymm
    unsigned __int128 ymmh[16];  // bits 128..255 of ymm
} RecompState;

_Static_assert(__builtin_offsetof(RecompState, xmm) == 160, "layout");
_Static_assert(__builtin_offsetof(RecompState, ymmh) == 416, "layout");

typedef void (*RecompFn)(RecompState *);
typedef struct { uint64_t guest; RecompFn fn; } RecompEntry;

// Emitted by the lifter.
extern const RecompEntry __recomp_table[];
extern const uint64_t __recomp_table_size;
// Provided by the runtime.
extern uint64_t __recomp_slide;
void __recomp_dispatch(RecompState *st, uint64_t target);
void __recomp_trap(RecompState *st, uint64_t addr, int32_t kind);
