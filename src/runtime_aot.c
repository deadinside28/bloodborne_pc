// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* Hybrid execution (BB_AOT): some of the game's functions run recompiled (recompiler/, x86-64
 * code from LLVM) while the rest stays the original code, calling each other both ways through
 * the thunks of recompiler/harness/recomp_hybrid_x86.S. A step towards running the game on
 * AArch64: the recompiled code meets the real game here first, function by function.
 *
 *   BB_AOT_DUMP=path   the game's code as it runs (after the port's patches and hooks) at start
 *   BB_AOT=lib.so      the recompiled functions (recompiler/harness/build_aot.sh, built against
 *                      that dump: functions the port changes are never recompiled)
 *   BB_AOT_BISECT=lo:hi only the library's functions lo..hi-1 (by address) are swapped in
 *
 * The library carries, per function, the code bytes it was lifted from (a hash); when the code
 * here differs (other patches or settings, another game version) the game does not start with it:
 * recompiled code calls other recompiled functions directly, so one changed function would
 * silently lose the port's change. */
#define _GNU_SOURCE
#include "runtime.h"
#include "../gpu/shim/bb_env.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) && defined(__x86_64__)
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <ucontext.h>
#include <unistd.h>
#include <x86intrin.h>
#include <cpuid.h>
#include "../recompiler/harness/recomp_hybrid.h"

uint64_t __recomp_slide;  /* the image's link address 0 (exported: the library reads it) */
void (*recomp_dispatch_fallback)(RecompState *st, uint64_t target);
void (*recomp_dispatch_jump_fallback)(RecompState *st, uint64_t target);

static const RecompEntry *table;
static uint64_t table_size;
static uint64_t image_size;  /* the game's image: [__recomp_slide, +image_size) is guest code and data */
static int enabled;

/* Link address -> recompiled function: open addressing, at most half full (every call of
 * recompiled code to a target it does not have directly asks this). */
static RecompEntry *hash;
static uint64_t hash_mask;
static unsigned hash_shift;

static uint64_t slot_of(uint64_t link) { return (link * 0x9e3779b97f4a7c15ull) >> hash_shift; }

static int hash_build(void) {
    unsigned bits = 4;
    while ((1ull << bits) < 2 * table_size) ++bits;
    hash = calloc(1ull << bits, sizeof(*hash));
    if (!hash) return -1;
    hash_mask = (1ull << bits) - 1;
    hash_shift = 64 - bits;
    for (uint64_t i = 0; i < table_size; ++i) {
        uint64_t s = slot_of(table[i].guest);
        while (hash[s].fn) s = (s + 1) & hash_mask;
        hash[s] = table[i];
    }
    return 0;
}

static RecompFn lookup(uint64_t link) {
    for (uint64_t s = slot_of(link);; s = (s + 1) & hash_mask) {
        if (hash[s].guest == link && hash[s].fn) return hash[s].fn;
        if (!hash[s].fn) return NULL;
    }
}

/* The thread's last dispatches into the original code (fault reports): target, the guest rsp and
 * the word there, the recompiled caller, and the rsp when the call came back (0: a jump, no
 * return; a call still running). */
typedef struct { uint64_t target, rsp, slot, caller, kind, done_rsp; } DispatchEvent;
enum { EV_CALL = 1, EV_JUMP = 2 };
static __thread DispatchEvent events[64];
static __thread unsigned event_head;

static DispatchEvent *note(RecompState *st, uint64_t target, uint64_t caller, uint64_t kind) {
    DispatchEvent *e = &events[event_head++ & 63];
    *e = (DispatchEvent){ target, st->gpr[G_RSP], *(const uint64_t *)(uintptr_t)st->gpr[G_RSP], caller, kind, 0 };
    return e;
}

/* Calls of recompiled code (direct ones to functions it does not have, indirect ones), and its
 * tail calls to known function starts: the recompiled function when there is one (even when
 * BB_AOT_BISECT left its entry original), else the original code, which returns here. */
void __recomp_dispatch(RecompState *st, uint64_t target) {
    const RecompFn fn = lookup(target - __recomp_slide);
    if (fn) { fn(st); return; }
    DispatchEvent *e = note(st, target, (uint64_t)(uintptr_t)__builtin_return_address(0), EV_CALL);
    if (target - __recomp_slide >= image_size) recomp_hybrid_self->host_calls++;  /* bbport, not the game */
    else if (runtime_libc_base && target >= runtime_libc_base) recomp_hybrid_self->module_calls++;  /* libc.prx... */
    recomp_dispatch_fallback(st, target);
    if (e->target == target) e->done_rsp = st->gpr[G_RSP];  /* unless the ring went round */
}

/* Jumps the CFG could not resolve: a recompiled function start is called (a tail call); anything
 * else, the middle of a function say, gets control for good: no return slot to borrow there. */
void __recomp_dispatch_jump(RecompState *st, uint64_t target) {
    const RecompFn fn = lookup(target - __recomp_slide);
    if (fn) { fn(st); return; }
    note(st, target, (uint64_t)(uintptr_t)__builtin_return_address(0), EV_JUMP);
    recomp_dispatch_jump_fallback(st, target);
}

/* From the fault handler (probe.c): the thread's hybrid state, to tell a fault the recompiled
 * code caused. dladdr names the recompiled function (F_<link address>) that made the call. */
void runtime_aot_describe_fault(void *context) {
    const HybridThread *t = recomp_hybrid_self;
    if (!enabled || !t) return;
    char line[512];
    Dl_info where;
    snprintf(line, sizeof(line), "AOT: this thread: %" PRIu64 " recompiled entries open\n", t->depth);
    ssize_t written = write(2, line, strlen(line));
    const ucontext_t *uc = context;
    const uint64_t rsp = (uint64_t)uc->uc_mcontext.gregs[REG_RSP];
    const uint64_t *s = (const uint64_t *)(uintptr_t)rsp;
    snprintf(line, sizeof(line), "AOT: fault rsp %#" PRIx64 " [-16] %#" PRIx64 " [-8] %#" PRIx64 " [0] %#" PRIx64
             " [8] %#" PRIx64 "; host_sp %#" PRIx64 " native_frame %#" PRIx64 "\n",
             rsp, s[-2], s[-1], s[0], s[1], t->host_sp, t->native_frame);
    written = write(2, line, strlen(line));
    for (uint64_t d = 0; d < t->depth && d < 4; ++d) {
        const RecompState *st = &t->states[t->depth - 1 - d];
        const uint64_t *g = (const uint64_t *)(uintptr_t)st->gpr[G_RSP];
        snprintf(line, sizeof(line), "AOT: open entry %" PRIu64 ": entered from %#" PRIx64 ", state rsp %#" PRIx64
                 " [-8] %#" PRIx64 " [0] %#" PRIx64 " rax %#" PRIx64 " rbx %#" PRIx64 " rbp %#" PRIx64 "\n",
                 t->depth - 1 - d, st->rip, st->gpr[G_RSP], g[-1], g[0], st->gpr[G_RAX], st->gpr[G_RBX], st->gpr[G_RBP]);
        written = write(2, line, strlen(line));
    }
    for (unsigned k = 0; k < 24; ++k) {  /* the last 24 calls and jumps into the original code, oldest first */
        const DispatchEvent *e = &events[(event_head - 24 + k) & 63];
        if (!e->target) continue;
        const char *n = dladdr((void *)(uintptr_t)e->caller, &where) && where.dli_sname ? where.dli_sname : "?";
        snprintf(line, sizeof(line), "AOT: %s %#" PRIx64 " from %s+%#" PRIx64 ": rsp %#" PRIx64 " [rsp] %#" PRIx64
                 " -> rsp after %#" PRIx64 "\n", e->kind == EV_JUMP ? "jump" : "call", e->target, n,
                 n[0] != '?' ? e->caller - (uint64_t)(uintptr_t)where.dli_saddr : e->caller, e->rsp, e->slot, e->done_rsp);
        written = write(2, line, strlen(line));
    }
    const uint64_t *m = (const uint64_t *)(uintptr_t)(rsp - 0x80);
    for (int r = 0; r < 12; ++r) {
        snprintf(line, sizeof(line), "AOT: stack %#" PRIx64 ": %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 "\n",
                 rsp - 0x80 + (uint64_t)r * 32, m[4 * r], m[4 * r + 1], m[4 * r + 2], m[4 * r + 3]);
        written = write(2, line, strlen(line));
    }
    (void)written;
}

void __recomp_trap(RecompState *st, uint64_t addr, int32_t kind) {
    (void)st;
    fprintf(stderr, "STOP: recompiled code trapped (kind %d) at guest offset 0x%" PRIx64 "\n", kind,
            addr - __recomp_slide);
    abort();
}

uint64_t __recomp_rdtsc(void) { return __rdtsc(); }  /* as the original's rdtsc reads it */

void __recomp_cpuid(uint32_t leaf, uint32_t sub, uint32_t out[4]) {
    __cpuid_count(leaf, sub, out[0], out[1], out[2], out[3]);
}

/* recompiler/recomp/game.py code_hash: FNV-1a, 0x65 (gs:) read as 0x64 (fs:). */
static uint64_t code_hash(const unsigned char *p, uint64_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i < n; ++i) h = (h ^ (p[i] == 0x65 ? 0x64 : p[i])) * 0x100000001b3ull;
    return h;
}

/* The image page by page: the gaps between its segments are not readable (zeros in the file);
 * write() reports them (EFAULT) instead of faulting. */
static void dump_image(const char *path, const unsigned char *image, uint64_t size) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    static const unsigned char zeros[4096];
    uint64_t unreadable = 0, at = 0;
    while (fd >= 0 && at < size) {
        const size_t n = size - at < 4096 ? (size_t)(size - at) : 4096;
        ssize_t w = write(fd, image + at, n);
        if (w < 0 && errno == EFAULT) { w = write(fd, zeros, n); unreadable += n; }
        if (w != (ssize_t)n) break;
        at += n;
    }
    if (fd < 0 || at < size) printf("AOT: cannot write the game's code to %s (%s)\n", path, strerror(errno));
    else printf("AOT: the game's code as it runs written to %s (%" PRIu64 " bytes, %" PRIu64 " unreadable)\n",
                path, size, unreadable);
    if (fd >= 0) close(fd);
}

static void report(void) {
    const HybridTotals n = recomp_hybrid_totals();
    printf("AOT: recompiled code entered %" PRIu64 "x from the original code; %" PRIu64 " calls back (%" PRIu64
           " into bbport, %" PRIu64 " into the game's system modules), %" PRIu64 " jumps back for good; deepest open "
           "entries %" PRIu64 "\n", n.entries, n.native_calls, n.host_calls, n.module_calls, n.native_jumps, n.max_depth);
    fflush(stdout);
}

static void *report_loop(void *arg) {
    (void)arg;
    for (;;) {
        sleep(30);
        report();
    }
    return NULL;
}

int runtime_aot_install(unsigned char *image, uint64_t size) {
    const char *dump = bb_env("BB_AOT_DUMP");
    if (dump && *dump) dump_image(dump, image, size);
    const char *path = bb_env("BB_AOT");
    if (!path || !*path) return 0;
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("AOT: %s\n", dlerror()); return -1; }
    table = dlsym(lib, "__recomp_table");
    const uint64_t *count = dlsym(lib, "__recomp_table_size");
    const uint64_t (*check)[3] = dlsym(lib, "__recomp_check");
    if (!table || !count || !check) {
        printf("AOT: %s is not a hybrid build (no __recomp_table / __recomp_check)\n", path);
        return -1;
    }
    table_size = *count;
    if (hash_build()) { printf("AOT: no memory for the lookup table\n"); return -1; }
    __recomp_slide = (uint64_t)(uintptr_t)image;
    image_size = size;
    uint64_t changed = 0;
    for (uint64_t i = 0; i < table_size; ++i) {
        if (check[i][0] != table[i].guest || check[i][1] > size || check[i][1] <= check[i][0]) {
            printf("AOT: %s: bad check entry %" PRIu64 "\n", path, i);
            return -1;
        }
        if (code_hash(image + check[i][0], check[i][1] - check[i][0]) != check[i][2] && changed++ < 5)
            printf("AOT: guest 0x%" PRIx64 " differs from the code the library was built from\n", check[i][0]);
    }
    if (changed) {
        printf("AOT: %" PRIu64 " of %" PRIu64 " functions differ: rebuild the library from a BB_AOT_DUMP of this "
               "configuration\n", changed, table_size);
        return -1;
    }
    uint64_t lo = 0, hi = table_size;
    const char *bisect = bb_env("BB_AOT_BISECT");
    if (bisect && sscanf(bisect, "%" SCNu64 ":%" SCNu64, &lo, &hi) == 2) {
        if (hi > table_size) hi = table_size;
        if (lo > hi) lo = hi;
    }
    /* Functions whose entry stays (too short for the jump, or jumped into): sorted addresses. */
    const uint64_t *nopatch = dlsym(lib, "__recomp_nopatch");
    const uint64_t *nopatch_count = dlsym(lib, "__recomp_nopatch_size");
    const uint64_t n_nopatch = nopatch && nopatch_count ? *nopatch_count : 0;
    recomp_hybrid_fs_from_gs = 1;
    recomp_hybrid_enable();
    uint64_t patched = 0, k = 0;
    for (uint64_t i = lo; i < hi; ++i) {
        while (k < n_nopatch && nopatch[k] < table[i].guest) ++k;
        if (k < n_nopatch && nopatch[k] == table[i].guest) continue;  /* recompiled callers only */
        char why[160];
        if (recomp_hybrid_patch(__recomp_slide + table[i].guest, table[i].fn, why)) {
            printf("AOT: guest 0x%" PRIx64 ": %s\n", table[i].guest, why);
            return -1;
        }
        ++patched;
    }
    enabled = 1;
    atexit(report);
    pthread_t reporter;
    if (!pthread_create(&reporter, NULL, report_loop, NULL)) pthread_detach(reporter);
    printf("AOT: %" PRIu64 " of the library's %" PRIu64 " recompiled functions swapped in, %" PRIu64
           " more for recompiled callers only (%s)\n", patched, table_size, n_nopatch, path);
    return 0;
}

void runtime_aot_thread_detach(void) {
    if (enabled) recomp_hybrid_thread_fini();
}

#else /* hybrid execution needs an x86-64 Linux host */

int runtime_aot_install(unsigned char *image, uint64_t size) {
    (void)image; (void)size;
    if (bb_env("BB_AOT") || bb_env("BB_AOT_DUMP")) printf("AOT: hybrid execution needs x86-64 Linux\n");
    return 0;
}
void runtime_aot_thread_detach(void) {}
void runtime_aot_describe_fault(void *context) { (void)context; }

#endif
