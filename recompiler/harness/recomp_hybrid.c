// SPDX-License-Identifier: GPL-2.0-or-later
// Hybrid execution (see recomp_hybrid.h): per-thread state and entry patches. The calls into
// original code and the entry patches are x86-64's; the per-thread state, entries from C and
// setjmp/longjmp for recompiled code also serve an AArch64 host (the recompiled game alone).
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

#if !defined(__x86_64__) && !defined(__aarch64__)
#error "x86-64 or AArch64 hosts"
#endif

__thread HybridThread *recomp_hybrid_self RECOMP_HYBRID_TLS_MODEL;
int recomp_hybrid_fs_from_gs;
extern void (*recomp_dispatch_jump_fallback)(RecompState *st, uint64_t target);
extern void (*recomp_dispatch_fallback)(RecompState *st, uint64_t target);
#if defined(__x86_64__)
// recomp_hybrid_jump_native's target: addressed fs-relative by the thunk (local-exec: the
// runtime is linked into the executable).
__thread uint64_t recomp_hybrid_jump_target __attribute__((tls_model("local-exec"), used));
void recomp_hybrid_enter(void);  // recomp_hybrid_x86.S
#endif

// MXCSR as recompiled code sees it: the hardware's on x86-64; elsewhere the default (rounding
// and exception masks are not applied to recompiled float code yet: recompiler/README.md).
static uint32_t host_mxcsr(void) {
#if defined(__x86_64__)
    return __builtin_ia32_stmxcsr();
#else
    return 0x1f80;
#endif
}
static void set_host_mxcsr(uint32_t v) {
#if defined(__x86_64__)
    __builtin_ia32_ldmxcsr(v);
#else
    (void)v;
#endif
}

_Static_assert(sizeof(RecompState) == 672, "RecompState layout (recomp_hybrid_x86.S)");
_Static_assert(__builtin_offsetof(HybridThread, native_frame) == 32, "HybridThread layout");
_Static_assert(__builtin_offsetof(HybridThread, entries) == 40, "HybridThread layout");

#define HOST_STACK_SIZE (64ull << 20)  // reserved, not committed: LLVM frames of nested guest calls
#define MAX_DEPTH 16384                // nested original -> recompiled entries per thread

static pthread_mutex_t threads_lock = PTHREAD_MUTEX_INITIALIZER;
static HybridThread *threads;    // live threads' states
static HybridTotals finished;    // counts of threads that ended

HybridTotals recomp_hybrid_totals(void) {
    pthread_mutex_lock(&threads_lock);
    HybridTotals n = finished;
    n.max_depth = 0;
    for (HybridThread *t = threads; t; t = t->next) {
        n.entries += __atomic_load_n(&t->entries, __ATOMIC_RELAXED);
        n.native_calls += __atomic_load_n(&t->native_calls, __ATOMIC_RELAXED);
        n.native_jumps += __atomic_load_n(&t->native_jumps, __ATOMIC_RELAXED);
        n.host_calls += __atomic_load_n(&t->host_calls, __ATOMIC_RELAXED);
        n.module_calls += __atomic_load_n(&t->module_calls, __ATOMIC_RELAXED);
        const uint64_t d = __atomic_load_n(&t->depth, __ATOMIC_RELAXED);
        if (d > n.max_depth) n.max_depth = d;
    }
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

// ---- Entering recompiled code from C (no stub: what a host without the x86 code does) ----

// fn(st) with the stack pointer at `top` (16-byte aligned): the recompiled function's LLVM frames
// go on the thread's host stack, not on the guest's, which the lifted code addresses itself.
void recomp_call_on_stack(RecompFn fn, RecompState *st, uint64_t top);
#if defined(__x86_64__)
__asm__(".text\n.globl recomp_call_on_stack\n.type recomp_call_on_stack,@function\nrecomp_call_on_stack:\n"
        "  push %rbp\n  mov %rsp, %rbp\n  mov %rdx, %rsp\n  and $-16, %rsp\n"
        "  mov %rdi, %rax\n  mov %rsi, %rdi\n  call *%rax\n"
        "  mov %rbp, %rsp\n  pop %rbp\n  ret\n.size recomp_call_on_stack, .-recomp_call_on_stack\n");
#elif defined(__aarch64__)
__asm__(".text\n.globl recomp_call_on_stack\n.type recomp_call_on_stack,%function\nrecomp_call_on_stack:\n"
        "  stp x29, x30, [sp, #-16]!\n  mov x29, sp\n  and x2, x2, #~15\n  mov sp, x2\n"
        "  mov x16, x0\n  mov x0, x1\n  blr x16\n"
        "  mov sp, x29\n  ldp x29, x30, [sp], #16\n  ret\n.size recomp_call_on_stack, .-recomp_call_on_stack\n");
#endif

uint64_t recomp_hybrid_call_recompiled(RecompFn fn, const uint64_t args[6], const uint64_t *xmm,
                                       const uint64_t *stack, unsigned stack_words, uint64_t fs_base,
                                       uint64_t *xmm0_out) {
    if (!recomp_hybrid_self && recomp_hybrid_thread_init()) {
        fprintf(stderr, "STOP: no hybrid state for this thread\n");
        abort();
    }
    HybridThread *const t = recomp_hybrid_self;
    if (t->depth >= t->max_depth) {
        fprintf(stderr, "STOP: recompiled entries nested too deep\n");
        abort();
    }
    // The guest's stack: below the innermost entry's (a callback from a host function called
    // by recompiled code), else below this C frame (a thread entry, the game's entry: their
    // stacks are the guest's).
    uint64_t sp = t->depth ? t->states[t->depth - 1].gpr[G_RSP] - 256
                           : (uint64_t)(uintptr_t)__builtin_frame_address(0) - 1024;
    sp -= 8 * (uint64_t)stack_words;
    sp &= ~(uint64_t)15;
    for (unsigned i = 0; i < stack_words; ++i) ((uint64_t *)(uintptr_t)sp)[i] = stack[i];
    sp -= 8;
    *(uint64_t *)(uintptr_t)sp = 0;  // the return slot (stack arguments past it): the lifted `ret` pops it
    RecompState *const st = &t->states[t->depth];
    memset(st, 0, sizeof *st);
    static const unsigned regs[6] = { G_RDI, G_RSI, G_RDX, G_RCX, G_R8, G_R9 };
    for (int i = 0; i < 6; ++i) st->gpr[regs[i]] = args[i];
    for (int i = 0; xmm && i < 8; ++i) st->xmm[i] = xmm[i];
    st->gpr[G_RSP] = sp;
    st->fs_base = fs_base;
    st->gs_base = fs_base;  // as the stub path: code lifted from bbport's patched bytes reads gs:
    st->mxcsr = 0x1f80;
    t->depth++;
    t->entries++;
    // The function's frames: at the thread's host_sp, or below this C frame when it is itself on
    // the host stack (a host function called by recompiled code entering the game again; the
    // host-call bridge lowers host_sp too, but any C caller is covered here).
    const uint64_t outer_sp = t->host_sp;
    uint64_t top = t->host_sp;
    const uint64_t here = (uint64_t)(uintptr_t)__builtin_frame_address(0);
    if (here > (uint64_t)(uintptr_t)t->stack && here <= (uint64_t)(uintptr_t)t->stack + t->stack_size &&
        here - 512 < top)
        top = (here - 512) & ~(uint64_t)15;
    t->host_sp = top;
    recomp_call_on_stack(fn, st, top);
    t->host_sp = outer_sp;
    t->depth--;
    if (xmm0_out) *xmm0_out = (uint64_t)st->xmm[0];
    return st->gpr[G_RAX];
}

void recomp_hybrid_thread_fini(void) {
    // When the thread ends: entries may still be open (a guest thread left through longjmp).
    HybridThread *t = recomp_hybrid_self;
    if (!t) return;
    pthread_mutex_lock(&threads_lock);
    for (HybridThread **p = &threads; *p; p = &(*p)->next)
        if (*p == t) { *p = t->next; break; }
    finished.entries += t->entries;
    finished.native_calls += t->native_calls;
    finished.native_jumps += t->native_jumps;
    finished.host_calls += t->host_calls;
    finished.module_calls += t->module_calls;
    pthread_mutex_unlock(&threads_lock);
    recomp_hybrid_self = NULL;
    free(t->jmps);
    munmap(t->stack, t->stack_size);
    munmap(t->states, t->states_size);
    free(t);
}

#if defined(__x86_64__)
// Recompiled code runs only inside an entry, so the thread's state exists here.
static void call_native(RecompState *st, uint64_t target) {
    recomp_hybrid_self->native_calls++;
    recomp_hybrid_call_native(st, target);
}

static void jump_native(RecompState *st, uint64_t target) {
    recomp_hybrid_self->native_jumps++;
    recomp_hybrid_jump_native(st, target);
}
#endif

// ---- setjmp / longjmp ------------------------------------------------------------------------

#define JMP_RECORDS 64

HybridJmp *recomp_hybrid_setjmp_record(RecompState *st) {
    HybridThread *t = recomp_hybrid_self;
    uint64_t *env = (uint64_t *)(uintptr_t)st->gpr[G_RDI];
    const uint64_t rsp = st->gpr[G_RSP];  // at the return address the guest's call pushed
    env[0] = *(const uint64_t *)(uintptr_t)rsp;
    env[1] = st->gpr[G_RBX];
    env[2] = rsp;
    env[3] = st->gpr[G_RBP];
    env[4] = st->gpr[G_R12];
    env[5] = st->gpr[G_R13];
    env[6] = st->gpr[G_R14];
    env[7] = st->gpr[G_R15];
    ((uint32_t *)env)[16] = 0x037f;  // x87 control word: the default (only libc uses x87)
    ((uint32_t *)env)[17] = host_mxcsr();
    st->gpr[G_RSP] = rsp + 8;  // setjmp's ret
    if (!t->jmps) t->jmps = calloc(JMP_RECORDS, sizeof(HybridJmp));
    HybridJmp *j = NULL;
    for (int i = 0; t->jmps && i < JMP_RECORDS && !j; ++i)
        if (t->jmps[i].env == (uint64_t)(uintptr_t)env) j = &t->jmps[i];
    if (!j && t->jmps) j = &t->jmps[t->jmp_next++ % JMP_RECORDS];  // the oldest goes
    static HybridJmp no_room;  // no memory: this setjmp is not resumable (a longjmp takes libc's path)
    if (!j) j = &no_room;
    *j = (HybridJmp){ .env = (uint64_t)(uintptr_t)env, .guest_rsp = rsp, .guest_ret = env[0], .st = st,
                      .depth = t->depth, .native_frame = t->native_frame, .host_sp = t->host_sp };
    return j;
}

void recomp_hybrid_longjmp(RecompState *st) {
    HybridThread *t = recomp_hybrid_self;
    const uint64_t *env = (const uint64_t *)(uintptr_t)st->gpr[G_RDI];
    const uint64_t val = st->gpr[G_RSI] ? st->gpr[G_RSI] : 1;
    const uint32_t mxcsr = (((const uint32_t *)env)[17] & ~0x3fu) | (host_mxcsr() & 0x3fu);
    for (int i = 0; t->jmps && i < JMP_RECORDS; ++i) {
        const HybridJmp *j = &t->jmps[i];
        if (j->env != (uint64_t)(uintptr_t)env || j->guest_rsp != env[2] || j->guest_ret != env[0]) continue;
        // A recompiled setjmp: its state gets the jmp_buf's registers, the thread's bookkeeping
        // goes back to then (entries and calls since are dropped), and its code resumes.
        RecompState *s = j->st;
        s->gpr[G_RBX] = env[1];
        s->gpr[G_RBP] = env[3];
        s->gpr[G_R12] = env[4];
        s->gpr[G_R13] = env[5];
        s->gpr[G_R14] = env[6];
        s->gpr[G_R15] = env[7];
        s->gpr[G_RSP] = env[2] + 8;
        s->gpr[G_RAX] = val;
        s->mxcsr = mxcsr;
        set_host_mxcsr(mxcsr);
        t->depth = j->depth;
        t->native_frame = j->native_frame;
        t->host_sp = j->host_sp;
        recomp_hybrid_longjmp_resume(j);
    }
    // A setjmp of the original code: what libc's longjmp does, on this entry's state (the entry
    // thunk returns through [rsp - 8] = the jmp_buf's return address).
    st->gpr[G_RBX] = env[1];
    st->gpr[G_RBP] = env[3];
    st->gpr[G_R12] = env[4];
    st->gpr[G_R13] = env[5];
    st->gpr[G_R14] = env[6];
    st->gpr[G_R15] = env[7];
    st->gpr[G_RAX] = val;
    st->mxcsr = mxcsr;
    *(uint64_t *)(uintptr_t)env[2] = env[0];
    st->gpr[G_RSP] = env[2] + 8;
}

#if defined(__aarch64__)
// __recomp_setjmp (returns twice) and its resume: the host's callee-saved registers (x19-x28,
// x29, d8-d15) in HybridJmp.saved, as recomp_hybrid_x86.S keeps rbx, rbp, r12-r15 there.
__asm__(".text\n.globl __recomp_setjmp\n.type __recomp_setjmp,%function\n__recomp_setjmp:\n"
        "  stp x29, x30, [sp, #-32]!\n  str x0, [sp, #16]\n  mov x29, sp\n"
        "  bl recomp_hybrid_setjmp_record\n"
        "  ldr x1, [sp, #16]\n  ldp x29, x30, [sp], #32\n"
        "  str x30, [x0]\n  mov x2, sp\n  str x2, [x0, #8]\n"
        "  stp x19, x20, [x0, #16]\n  stp x21, x22, [x0, #32]\n  stp x23, x24, [x0, #48]\n"
        "  stp x25, x26, [x0, #64]\n  stp x27, x28, [x0, #80]\n  str x29, [x0, #96]\n"
        "  stp d8, d9, [x0, #104]\n  stp d10, d11, [x0, #120]\n  stp d12, d13, [x0, #136]\n"
        "  stp d14, d15, [x0, #152]\n"
        "  str xzr, [x1]\n"  // the guest's rax: setjmp returns 0
        "  ret\n.size __recomp_setjmp, .-__recomp_setjmp\n"
        ".globl recomp_hybrid_longjmp_resume\n.type recomp_hybrid_longjmp_resume,%function\n"
        "recomp_hybrid_longjmp_resume:\n"
        "  ldp x19, x20, [x0, #16]\n  ldp x21, x22, [x0, #32]\n  ldp x23, x24, [x0, #48]\n"
        "  ldp x25, x26, [x0, #64]\n  ldp x27, x28, [x0, #80]\n  ldr x29, [x0, #96]\n"
        "  ldp d8, d9, [x0, #104]\n  ldp d10, d11, [x0, #120]\n  ldp d12, d13, [x0, #136]\n"
        "  ldp d14, d15, [x0, #152]\n"
        "  ldr x1, [x0, #8]\n  mov sp, x1\n  ldr x30, [x0]\n  ret\n"
        ".size recomp_hybrid_longjmp_resume, .-recomp_hybrid_longjmp_resume\n");
_Static_assert(__builtin_offsetof(HybridJmp, saved) == 16 && sizeof(((HybridJmp *)0)->saved) >= 160,
               "HybridJmp layout (the asm above)");
#else
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

static uint8_t *make_stub(uint64_t entry, RecompFn fn, char *why) {
    uint8_t *page = stub_near(entry);
    if (!page) { snprintf(why, 160, "no stub space within reach of %#" PRIx64, entry); return NULL; }
    StubPage *sp = NULL;
    for (int i = 0; i < n_stub_pages; i++) if (stub_pages[i].base == page) sp = &stub_pages[i];
    uint8_t *s = page + sp->used;
    if (mprotect(page, STUB_PAGE, PROT_READ | PROT_WRITE)) { snprintf(why, 160, "stub page: %s", strerror(errno)); return NULL; }
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
    if (mprotect(page, STUB_PAGE, PROT_READ | PROT_EXEC)) { snprintf(why, 160, "stub page: %s", strerror(errno)); return NULL; }
    return s;
}

uint64_t recomp_hybrid_stub(uint64_t near, RecompFn fn, char *why) {
    pthread_mutex_lock(&patch_lock);
    uint8_t *s = make_stub(near, fn, why);
    pthread_mutex_unlock(&patch_lock);
    return (uint64_t)(uintptr_t)s;
}

int recomp_hybrid_patch(uint64_t entry, RecompFn fn, char *why, uint64_t *stub) {
    pthread_mutex_lock(&patch_lock);
    int rc = -1;
    uint8_t *s = make_stub(entry, fn, why);
    if (!s) goto out;
    if (stub) *stub = (uint64_t)(uintptr_t)s;
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
#endif
