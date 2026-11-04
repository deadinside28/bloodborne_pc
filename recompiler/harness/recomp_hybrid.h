// SPDX-License-Identifier: GPL-2.0-or-later
// Hybrid execution (x86-64 hosts only): an image's original code and its recompiled functions run
// in one process, calling each other in both directions.
//
//   original -> recompiled: the function's entry is patched with a jump to a stub, which enters
//       recomp_hybrid_enter: the guest registers go into a RecompState, the recompiled function
//       runs on a host stack of its own (its LLVM frames must not overwrite the guest stack, which
//       the lifted code addresses at the same addresses), and the registers come back from the
//       state when it returns.
//   recompiled -> original: __recomp_dispatch finds no recompiled function and calls
//       recomp_hybrid_call_native: the guest registers are loaded from the state and the target
//       runs natively on the guest stack; its return comes back to the thunk, which stores the
//       registers again.
//
// Each host thread that runs guest code needs recomp_hybrid_thread_init() first.
#pragma once
#include <stdint.h>

#include "recomp_rt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Per host thread. Offsets are used by recomp_hybrid_x86.S.
typedef struct HybridThread {
    uint64_t host_sp;       // 0: host stack top for the next entry (16-byte aligned)
    uint64_t depth;         // 8: entries in use in states[]
    RecompState *states;    // 16
    uint64_t max_depth;     // 24
    uint64_t native_frame;  // 32: host frame of the innermost recompiled -> original call
    uint64_t entries;       // 40: original -> recompiled entries on this thread
    uint8_t *stack;         // host stack mapping (for thread_fini)
    uint64_t stack_size, states_size;
} HybridThread;

extern __thread HybridThread *recomp_hybrid_self __attribute__((tls_model("initial-exec")));
// The guest's thread pointer is GS (bbport rewrites the game's fs: accesses to gs:), while the
// recompiled code, lifted from the unmodified executable, addresses FS: state.fs_base := GS base.
extern int recomp_hybrid_fs_from_gs;
extern _Atomic uint64_t recomp_hybrid_native_calls;

int recomp_hybrid_thread_init(void);  // 0, or -1 (errno)
void recomp_hybrid_thread_fini(void);
// Sends the original function at host address `entry` to `fn` (a 5-byte jump at the entry).
// 0, or -1 with `why` filled in (at most 160 bytes).
int recomp_hybrid_patch(uint64_t entry, RecompFn fn, char *why);
// The dispatcher's fallback for targets that have no recompiled function.
void recomp_hybrid_call_native(RecompState *st, uint64_t target);
// Installs recomp_hybrid_call_native as the dispatcher's fallback.
void recomp_hybrid_enable(void);

// Tests: calls fn(a, b, c, d) natively with known values in the callee-saved registers; returns
// rax (and xmm0's low 64 bits in *xmm0) and sets *bad when a callee-saved register or rsp changed.
uint64_t recomp_hybrid_checked_call(uint64_t fn, uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                                    uint64_t fa, uint64_t fb, uint64_t *xmm0, int *bad);

#ifdef __cplusplus
}
#endif
