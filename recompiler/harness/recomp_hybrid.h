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
// Each host thread gets its state on its first entry (or earlier from recomp_hybrid_thread_init);
// recomp_hybrid_thread_fini frees it when the thread ends.
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
    uint64_t native_calls;  // recompiled -> original calls and jumps on this thread (counted per
    uint64_t native_jumps;  //   thread: one shared atomic counter cost every core a cache line)
    uint64_t host_calls;    // of native_calls: into host functions (imports), not guest code
    uint64_t module_calls;  // of native_calls: into the game's linked system modules (libc.prx...)
    uint8_t *stack;         // host stack mapping (for thread_fini)
    uint64_t stack_size, states_size;
    struct HybridThread *next;  // all threads' states (recomp_hybrid_total_entries)
    struct HybridJmp *jmps;     // recompiled setjmps on this thread (recomp_hybrid_setjmp_record)
    uint64_t jmp_next;
} HybridThread;

// A setjmp made by recompiled code (__recomp_setjmp): how to resume it when a longjmp to its
// jmp_buf comes. Offsets used by recomp_hybrid_x86.S.
typedef struct HybridJmp {
    uint64_t host_ret;      // 0: the return address into the recompiled code
    uint64_t host_rsp;      // 8: the host rsp after that return
    uint64_t rbx, rbp, r12, r13, r14, r15;  // 16..56: the host's callee-saved registers
    uint64_t env;           // the guest jmp_buf
    uint64_t guest_rsp, guest_ret;  // what the jmp_buf held then (a later setjmp may reuse it)
    RecompState *st;        // the state the recompiled code runs on
    uint64_t depth, native_frame, host_sp;  // the thread's hybrid bookkeeping then
} HybridJmp;

// The game libc's setjmp, for recompiled callers: void __recomp_setjmp(RecompState *st), returns
// twice (recomp_hybrid_x86.S). jmp_buf layout (libc.prx): [0] return address, [1] rbx, [2] rsp
// (at the return address), [3] rbp, [4..7] r12-r15, +0x40 x87 control word, +0x44 MXCSR,
// +0x48 the signal mask (not saved: bbport delivers no guest signals).
void __recomp_setjmp(RecompState *st);
HybridJmp *recomp_hybrid_setjmp_record(RecompState *st);  // its C half
// Replaces the libc's longjmp at its entry (a RecompFn for recomp_hybrid_patch): a jmp_buf of a
// recompiled setjmp resumes that recompiled code; any other does what libc's longjmp does.
void recomp_hybrid_longjmp(RecompState *st);
__attribute__((noreturn)) void recomp_hybrid_longjmp_resume(const HybridJmp *j);

extern __thread HybridThread *recomp_hybrid_self __attribute__((tls_model("initial-exec")));
// The guest's thread pointer is GS (bbport rewrites the game's fs: accesses to gs:), while the
// recompiled code, lifted from the unmodified executable, addresses FS: state.fs_base := GS base.
extern int recomp_hybrid_fs_from_gs;

int recomp_hybrid_thread_init(void);  // 0, or -1 (errno)
void recomp_hybrid_thread_fini(void);
// Entries into recompiled code, and calls / jumps back to the original code, on all threads so
// far (live threads read without locking).
typedef struct { uint64_t entries, native_calls, native_jumps, max_depth, host_calls, module_calls; } HybridTotals;  // max_depth: open entries, deepest live thread
HybridTotals recomp_hybrid_totals(void);
// Sends the original function at host address `entry` to `fn` (a 5-byte jump at the entry, to a
// stub that enters recomp_hybrid_enter with `fn`; *stub := the stub, when given). 0, or -1 with
// `why` filled in (at most 160 bytes).
int recomp_hybrid_patch(uint64_t entry, RecompFn fn, char *why, uint64_t *stub);
// A stub alone (no entry patched; within reach of `near`): calling it with the guest's ABI enters
// `fn` as a call of the original function would, without running any of the original code.
// The stub's address, or 0 with `why` filled in.
uint64_t recomp_hybrid_stub(uint64_t near, RecompFn fn, char *why);
// The dispatcher's fallback for targets that have no recompiled function.
void recomp_hybrid_call_native(RecompState *st, uint64_t target);
// A call to a host function (SysV ABI, not guest code): argument and result registers only.
void recomp_hybrid_call_host(RecompState *st, uint64_t target);
// __recomp_dispatch_jump's fallback: the original code continues at target for good (the
// thread's innermost entry ends); no return.
__attribute__((noreturn)) void recomp_hybrid_jump_native(RecompState *st, uint64_t target);
// Installs recomp_hybrid_call_native / recomp_hybrid_jump_native as the dispatchers' fallbacks.
void recomp_hybrid_enable(void);

// Tests: calls fn(a, b, c, d) natively with known values in the callee-saved registers; returns
// rax (and xmm0's low 64 bits in *xmm0) and sets *bad when a callee-saved register or rsp changed.
uint64_t recomp_hybrid_checked_call(uint64_t fn, uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                                    uint64_t fa, uint64_t fb, uint64_t *xmm0, int *bad);

#ifdef __cplusplus
}
#endif
