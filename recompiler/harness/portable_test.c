// SPDX-License-Identifier: GPL-2.0-or-later
// The parts of the hybrid runtime a host without the game's x86 code uses (recomp_hybrid.c):
// entering a recompiled function from C (recomp_hybrid_call_recompiled, its frames on the
// thread's host stack) and setjmp / longjmp for recompiled code (__recomp_setjmp returns twice;
// recomp_hybrid_longjmp resumes it). The "recompiled" functions here are written in C the way the
// lifter's output behaves: a call pushes the guest return address, setjmp is __recomp_setjmp on
// the state. Built for x86-64 and AArch64 (run.sh runs it natively and under QEMU).
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_hybrid.h"

uint64_t __recomp_slide;
void (*recomp_dispatch_fallback)(RecompState *st, uint64_t target);
void (*recomp_dispatch_jump_fallback)(RecompState *st, uint64_t target);

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL " __VA_ARGS__); printf("\n"); ++failures; } } while (0)

static uint64_t guest_jmp_buf[16];  // libc.prx's jmp_buf, in "guest memory"
static volatile int resumed;

// int f(int depth): setjmp(buf); the first time, longjmp(buf, depth + 41) from a nested call
// (depth > 0: from a further entry made from C, as a host callback would); returns setjmp's value.
static void inner_longjmp(RecompState *st) {
    // longjmp(buf, v): the guest's arguments, then the runtime's replacement of libc's longjmp
    st->gpr[G_RDI] = (uint64_t)(uintptr_t)guest_jmp_buf;
    recomp_hybrid_longjmp(st);
    fprintf(stderr, "STOP: longjmp returned\n");
    abort();
}

__attribute__((noinline)) static void through_c_entry(RecompState *st) {
    const uint64_t args[6] = { 0, st->gpr[G_RSI], 0, 0, 0, 0 };  // longjmp's value in the inner entry's rsi
    recomp_hybrid_call_recompiled(inner_longjmp, args, NULL, NULL, 0, 0, NULL);
}

static void f(RecompState *st) {
    const uint64_t depth = st->gpr[G_RDI];
    volatile uint64_t local = 0x5eed0000 + depth;  // must survive the second return
    // call setjmp: push the guest return address, the jmp_buf in rdi
    st->gpr[G_RSP] -= 8;
    *(uint64_t *)(uintptr_t)st->gpr[G_RSP] = 0x1000 + depth;
    st->gpr[G_RDI] = (uint64_t)(uintptr_t)guest_jmp_buf;
    __recomp_setjmp(st);
    if (st->gpr[G_RAX] == 0) {
        CHECK(guest_jmp_buf[0] == 0x1000 + depth, "jmp_buf return address %#" PRIx64, guest_jmp_buf[0]);
        st->gpr[G_RSI] = depth + 41;
        // call longjmp's caller: push its return address
        st->gpr[G_RSP] -= 8;
        *(uint64_t *)(uintptr_t)st->gpr[G_RSP] = 0x2000;
        if (depth) through_c_entry(st);
        else inner_longjmp(st);
        CHECK(0, "longjmp came back");
        return;
    }
    resumed++;
    CHECK(local == 0x5eed0000 + depth, "local lost across the second return");
    // the lifted `ret`: pop the return address the entry pushed
    st->gpr[G_RSP] += 8;
}

int main(void) {
    for (uint64_t depth = 0; depth < 2; ++depth) {
        resumed = 0;
        const uint64_t args[6] = { depth, 0, 0, 0, 0, 0 };
        const uint64_t r = recomp_hybrid_call_recompiled(f, args, NULL, NULL, 0, 0, NULL);
        CHECK(r == depth + 41, "depth %" PRIu64 ": setjmp's second return gave %" PRIu64, depth, r);
        CHECK(resumed == 1, "depth %" PRIu64 ": resumed %d times", depth, resumed);
        CHECK(recomp_hybrid_self && recomp_hybrid_self->depth == 0, "entry depth %" PRIu64 " after",
              recomp_hybrid_self ? recomp_hybrid_self->depth : 0);
    }
    printf("portable runtime: %s\n", failures ? "FAILED" : "setjmp/longjmp and entries from C OK");
    return failures != 0;
}
