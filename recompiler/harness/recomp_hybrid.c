// SPDX-License-Identifier: GPL-2.0-or-later
// Hybrid execution (see recomp_hybrid.h): per-thread state and entry patches. x86-64 only.
#define _GNU_SOURCE
#include "recomp_hybrid.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if !defined(__x86_64__)
#error "hybrid execution runs original x86-64 code: x86-64 hosts only"
#endif

__thread HybridThread *recomp_hybrid_self __attribute__((tls_model("initial-exec")));
// recomp_hybrid_jump_native's target: addressed fs-relative by the thunk (local-exec: the
// runtime is linked into the executable).
__thread uint64_t recomp_hybrid_jump_target __attribute__((tls_model("local-exec"), used));
_Atomic uint64_t recomp_hybrid_native_jumps;
extern void (*recomp_dispatch_jump_fallback)(RecompState *st, uint64_t target);
int recomp_hybrid_fs_from_gs;
_Atomic uint64_t recomp_hybrid_native_calls;

void recomp_hybrid_enter(void);  // recomp_hybrid_x86.S
extern void (*recomp_dispatch_fallback)(RecompState *st, uint64_t target);

_Static_assert(sizeof(RecompState) == 672, "RecompState layout (recomp_hybrid_x86.S)");
_Static_assert(__builtin_offsetof(HybridThread, native_frame) == 32, "HybridThread layout");
_Static_assert(__builtin_offsetof(HybridThread, entries) == 40, "HybridThread layout");

#define HOST_STACK_SIZE (64ull << 20)  // reserved, not committed: LLVM frames of nested guest calls
#define MAX_DEPTH 16384                // nested original -> recompiled entries per thread

static pthread_mutex_t threads_lock = PTHREAD_MUTEX_INITIALIZER;
static HybridThread *threads;      // live threads' states
static uint64_t finished_entries;  // entries of threads that ended

uint64_t recomp_hybrid_total_entries(void) {
    pthread_mutex_lock(&threads_lock);
    uint64_t n = finished_entries;
    for (HybridThread *t = threads; t; t = t->next) n += __atomic_load_n(&t->entries, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&threads_lock);
    return n;
}

int recomp_hybrid_thread_init(void) {
    if (recomp_hybrid_self) return 0;
    HybridThread *t = calloc(1, sizeof *t);
    if (!t) return -1;
    t->stack_size = HOST_STACK_SIZE;
    t->stack = mmap(NULL, t->stack_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    if (t->stack == MAP_FAILED) { free(t); return -1; }
    mprotect(t->stack, 4096, PROT_NONE);  // guard page
    t->states_size = (uint64_t)MAX_DEPTH * sizeof(RecompState);
    void *states = mmap(NULL, t->states_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (states == MAP_FAILED) { munmap(t->stack, t->stack_size); free(t); return -1; }
    t->states = states;
    t->max_depth = MAX_DEPTH;
    t->host_sp = ((uint64_t)(uintptr_t)(t->stack + t->stack_size)) & ~15ull;
    pthread_mutex_lock(&threads_lock);
    t->next = threads;
    threads = t;
    pthread_mutex_unlock(&threads_lock);
    recomp_hybrid_self = t;
    return 0;
}

void recomp_hybrid_thread_fini(void) {
    // When the thread ends: entries may still be open (a guest thread left through longjmp).
    HybridThread *t = recomp_hybrid_self;
    if (!t) return;
    pthread_mutex_lock(&threads_lock);
    for (HybridThread **p = &threads; *p; p = &(*p)->next)
        if (*p == t) { *p = t->next; break; }
    finished_entries += t->entries;
    pthread_mutex_unlock(&threads_lock);
    recomp_hybrid_self = NULL;
    munmap(t->stack, t->stack_size);
    munmap(t->states, t->states_size);
    free(t);
}

static void call_native(RecompState *st, uint64_t target) {
    recomp_hybrid_native_calls++;
    recomp_hybrid_call_native(st, target);
}

static void jump_native(RecompState *st, uint64_t target) {
    recomp_hybrid_native_jumps++;
    recomp_hybrid_jump_native(st, target);
}

void recomp_hybrid_enable(void) {
    recomp_dispatch_fallback = call_native;
    recomp_dispatch_jump_fallback = jump_native;
}

// ---- entry patches ---------------------------------------------------------------------------
// Each patched function gets a 48-byte stub within rel32 reach of its entry:
//   mov [rsp-0x10], rax; movabs rax, fn; mov [rsp-0x18], rax; mov rax, [rsp-0x10];
//   jmp [rip+0]; .quad recomp_hybrid_enter
// and its entry a `jmp rel32` to the stub.

#define STUB_SIZE 48
#define STUB_PAGE (64u << 10)
typedef struct { uint8_t *base; uint32_t used; } StubPage;
static StubPage stub_pages[256];
static int n_stub_pages;
static pthread_mutex_t patch_lock = PTHREAD_MUTEX_INITIALIZER;

static int in_reach(uint64_t from_next, uint64_t to) {
    int64_t d = (int64_t)(to - from_next);
    return d == (int32_t)d;
}

static uint8_t *stub_near(uint64_t entry) {
    for (int i = 0; i < n_stub_pages; i++) {
        StubPage *p = &stub_pages[i];
        uint64_t s = (uint64_t)(uintptr_t)(p->base + p->used);
        if (p->used + STUB_SIZE <= STUB_PAGE && in_reach(entry + 5, s) && in_reach(entry + 5, s + STUB_SIZE))
            return p->base;
    }
    if (n_stub_pages == (int)(sizeof stub_pages / sizeof stub_pages[0])) return NULL;
    // A free range near the entry: try addresses below it, then above, in 16 MiB steps.
    for (int64_t k = 1; k < 120; k++) {
        for (int dir = -1; dir <= 1; dir += 2) {
            uint64_t want = ((entry & ~(uint64_t)(STUB_PAGE - 1)) + (uint64_t)(dir * k * (16ll << 20)));
            if (!in_reach(entry + 5, want) || !in_reach(entry + 5, want + STUB_PAGE)) continue;
            void *p = mmap((void *)(uintptr_t)want, STUB_PAGE, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p == MAP_FAILED) continue;
            stub_pages[n_stub_pages] = (StubPage){ p, 0 };
            return stub_pages[n_stub_pages++].base;
        }
    }
    return NULL;
}

int recomp_hybrid_patch(uint64_t entry, RecompFn fn, char *why) {
    pthread_mutex_lock(&patch_lock);
    int rc = -1;
    uint8_t *page = stub_near(entry);
    if (!page) { snprintf(why, 160, "no stub space within reach of %#" PRIx64, entry); goto out; }
    StubPage *sp = NULL;
    for (int i = 0; i < n_stub_pages; i++) if (stub_pages[i].base == page) sp = &stub_pages[i];
    uint8_t *s = page + sp->used;
    if (mprotect(page, STUB_PAGE, PROT_READ | PROT_WRITE)) { snprintf(why, 160, "stub page: %s", strerror(errno)); goto out; }
    static const uint8_t head[] = { 0x48, 0x89, 0x44, 0x24, 0xf0, 0x48, 0xb8 };           // mov [rsp-0x10], rax; movabs rax,
    static const uint8_t tail[] = { 0x48, 0x89, 0x44, 0x24, 0xe8, 0x48, 0x8b, 0x44, 0x24, 0xf0,  // mov [rsp-0x18], rax; mov rax, [rsp-0x10]
                                    0xff, 0x25, 0, 0, 0, 0 };                                 // jmp [rip+0]
    uint64_t f = (uint64_t)(uintptr_t)fn, enter = (uint64_t)(uintptr_t)recomp_hybrid_enter;
    size_t n = 0;
    memcpy(s + n, head, sizeof head); n += sizeof head;
    memcpy(s + n, &f, 8); n += 8;
    memcpy(s + n, tail, sizeof tail); n += sizeof tail;
    memcpy(s + n, &enter, 8); n += 8;
    memset(s + n, 0xcc, STUB_SIZE - n);
    sp->used += STUB_SIZE;
    if (mprotect(page, STUB_PAGE, PROT_READ | PROT_EXEC)) { snprintf(why, 160, "stub page: %s", strerror(errno)); goto out; }
    // The entry: jmp rel32. Its page is the image's code (read + execute while not being patched).
    uint8_t jmp[5] = { 0xe9 };
    int32_t rel = (int32_t)((uint64_t)(uintptr_t)s - (entry + 5));
    memcpy(jmp + 1, &rel, 4);
    uint64_t pg = entry & ~4095ull, pg_end = (entry + 5 + 4095) & ~4095ull;
    if (mprotect((void *)(uintptr_t)pg, pg_end - pg, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        snprintf(why, 160, "code page %#" PRIx64 ": %s", pg, strerror(errno));
        goto out;
    }
    memcpy((void *)(uintptr_t)entry, jmp, 5);
    mprotect((void *)(uintptr_t)pg, pg_end - pg, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)(uintptr_t)entry, (char *)(uintptr_t)entry + 5);
    rc = 0;
out:
    pthread_mutex_unlock(&patch_lock);
    return rc;
}
