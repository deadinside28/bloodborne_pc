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
#include "../recompiler/harness/recomp_x87.h"  /* __recomp_x87 (defined there) */
#include "../gpu/shim/bbport_guest_sites.h"

/* The port's guest sites (gpu/shim/bbport_guest_sites.cpp): weak, a CPU-only build has none. */
__attribute__((weak)) size_t bbport_guest_sites(const BbGuestSite **sites);
__attribute__((weak)) const BbGuestSite *bbport_guest_site_at(uint64_t address);
__attribute__((weak)) extern BbGuestCallHook bbport_guest_call_hook;

uint64_t __recomp_slide;  /* the image's link address 0 (exported: the library reads it) */
void (*recomp_dispatch_fallback)(RecompState *st, uint64_t target);
void (*recomp_dispatch_jump_fallback)(RecompState *st, uint64_t target);

static const RecompEntry *table;
static uint64_t table_size;
static uint64_t image_size;  /* the game's image: [__recomp_slide, +image_size) is guest code and data */
/* Calls into the runtime take recomp_hybrid_call_host (argument and result registers only);
 * BB_AOT_HOST_CALLS=full: the whole register file, as calls into the game's own code. */
static int host_calls_light = 1;
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

/* Host code address -> the game function it was recompiled from: the library's functions sorted
 * by host address, each up to the next one's start (they are laid out one after another). For
 * the port's checks that know game code by the faulting instruction's address (the GPU write
 * tracking's dynamic writers, the layer traps' game-or-port): gpu/games/game_profile.cpp,
 * gpu/layer/buffer_cache_layer_internal.h. */
typedef struct { uint64_t host, guest; } HostRange;
static HostRange *host_ranges;
static uint64_t host_range_count, host_code_end;

static int by_host(const void *a, const void *b) {
    const uint64_t x = ((const HostRange *)a)->host, y = ((const HostRange *)b)->host;
    return x < y ? -1 : x > y;
}

static int host_map_build(void) {
    host_ranges = malloc(table_size * sizeof(*host_ranges));
    if (!host_ranges) return -1;
    for (uint64_t i = 0; i < table_size; ++i)
        host_ranges[i] = (HostRange){ (uint64_t)(uintptr_t)table[i].fn, __recomp_slide + table[i].guest };
    qsort(host_ranges, table_size, sizeof(*host_ranges), by_host);
    host_range_count = table_size;
    host_code_end = table_size ? host_ranges[table_size - 1].host + (1u << 20) : 0;  /* the last one: at most 1 MiB */
    return 0;
}

uint64_t runtime_aot_guest_pc(uint64_t host_pc) {
    if (!host_range_count || host_pc < host_ranges[0].host || host_pc >= host_code_end) return 0;
    uint64_t lo = 0, hi = host_range_count;  /* the last start <= host_pc */
    while (hi - lo > 1) {
        const uint64_t mid = (lo + hi) / 2;
        if (host_ranges[mid].host <= host_pc) lo = mid; else hi = mid;
    }
    return host_ranges[lo].guest;
}

/* BB_AOT_TARGETS=1: every 64th call or jump of recompiled code into original game code is
 * counted by target (and its recompiled caller), and the most frequent are printed at exit: what
 * still runs as original code, and from where. */
static int sample_targets;
static __thread unsigned sample_tick;
typedef struct { uint64_t target, caller, count; int jump; } TargetCount;
static TargetCount target_counts[4096];
static pthread_mutex_t target_lock = PTHREAD_MUTEX_INITIALIZER;

static void sample(uint64_t target, uint64_t caller, int jump) {
    if (!sample_targets || (++sample_tick & 63)) return;
    pthread_mutex_lock(&target_lock);
    for (uint64_t s = (target * 0x9e3779b97f4a7c15ull) >> 52, n = 0; n < 4096; s = (s + 1) & 4095, ++n) {
        TargetCount *t = &target_counts[s];
        if (t->count && (t->target != target || t->jump != jump)) continue;
        if (!t->count) *t = (TargetCount){ target, caller, 0, jump };
        t->count++;
        break;
    }
    pthread_mutex_unlock(&target_lock);
}

static int by_count(const void *a, const void *b) {
    const uint64_t x = ((const TargetCount *)a)->count, y = ((const TargetCount *)b)->count;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void report_targets(void) {
    if (!sample_targets) return;
    static TargetCount sorted[4096];
    pthread_mutex_lock(&target_lock);
    memcpy(sorted, target_counts, sizeof(sorted));
    pthread_mutex_unlock(&target_lock);
    qsort(sorted, 4096, sizeof(*sorted), by_count);
    printf("AOT: most frequent targets in the original game code (every 64th call or jump, by count):\n");
    for (int i = 0; i < 40 && sorted[i].count; ++i) {
        Dl_info where;
        const char *n = dladdr((void *)(uintptr_t)sorted[i].caller, &where) && where.dli_sname ? where.dli_sname : "?";
        printf("AOT:   %8" PRIu64 " %s guest 0x%" PRIx64 " (e.g. from %s)\n", sorted[i].count,
               sorted[i].jump ? "jump to" : "call to", sorted[i].target - __recomp_slide, n);
    }
    fflush(stdout);
}

/* Calls of recompiled code (direct ones to functions it does not have, indirect ones), and its
 * tail calls to known function starts: the recompiled function when there is one (even when
 * BB_AOT_BISECT left its entry original), else the original code, which returns here. */
void __recomp_dispatch(RecompState *st, uint64_t target) {
    const RecompFn fn = lookup(target - __recomp_slide);
    if (fn) { fn(st); return; }
    DispatchEvent *e = note(st, target, (uint64_t)(uintptr_t)__builtin_return_address(0), EV_CALL);
    if (target - __recomp_slide >= image_size) {  /* bbport's runtime (an import), not the game */
        recomp_hybrid_self->host_calls++;
        if (host_calls_light) {
            recomp_hybrid_call_host(st, target);
            if (e->target == target) e->done_rsp = st->gpr[G_RSP];
            return;
        }
    } else if (runtime_libc_base && target >= runtime_libc_base) recomp_hybrid_self->module_calls++;  /* libc.prx... */
    if (target - __recomp_slide < image_size) sample(target, e->caller, 0);
    recomp_dispatch_fallback(st, target);
    if (e->target == target) e->done_rsp = st->gpr[G_RSP];  /* unless the ring went round */
}

/* Jumps the CFG could not resolve: a recompiled function start is called (a tail call); a host
 * function too (only a tail call reaches the runtime by a jump: [rsp] is the return address);
 * anything else, the middle of a function say, gets control for good: no return slot to borrow. */
void __recomp_dispatch_jump(RecompState *st, uint64_t target) {
    const RecompFn fn = lookup(target - __recomp_slide);
    if (fn) { fn(st); return; }
    if (target - __recomp_slide >= image_size && host_calls_light) {
        recomp_hybrid_self->host_calls++;
        recomp_hybrid_call_host(st, target);
        return;
    }
    note(st, target, (uint64_t)(uintptr_t)__builtin_return_address(0), EV_JUMP);
    sample(target, (uint64_t)(uintptr_t)__builtin_return_address(0), 1);
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

/* A guest site in recompiled code (recomp/lift.py): before the instruction at `link`, the port's
 * handler for it, if one is registered in this configuration, runs with the guest's registers
 * (it may change them); returns where to go on (0: the instruction at the site) and the flags. */
typedef struct { uint64_t next, flags; } RecompSiteResult;
static uint64_t site_calls;
static __thread RecompState *site_state;  /* the recompiled code's state while a handler runs */
static __thread uint64_t site_bypass;     /* a site whose handler is skipped once: a call of the function it replaced */

RecompSiteResult __recomp_guest_site(RecompState *st, uint64_t link, uint64_t flags) {
    if (site_bypass == __recomp_slide + link) {
        site_bypass = 0;
        return (RecompSiteResult){ 0, flags };
    }
    const BbGuestSite *s = bbport_guest_site_at ? bbport_guest_site_at(__recomp_slide + link) : NULL;
    if (!s || !s->handler) return (RecompSiteResult){ 0, flags };  /* none, or a variant (lifted both ways) */
    __atomic_add_fetch(&site_calls, 1, __ATOMIC_RELAXED);
    static const unsigned order[16] = { G_RAX, G_RBX, G_RCX, G_RDX, G_RSI, G_RDI, G_RBP, G_RSP,
                                        G_R8, G_R9, G_R10, G_R11, G_R12, G_R13, G_R14, G_R15 };
    BbGuestRegisters r;
    uint64_t *g = &r.rax;
    for (int i = 0; i < 16; ++i) g[i] = st->gpr[order[i]];
    r.rflags = flags;
    r.mxcsr = _mm_getcsr();  /* the hardware's: recompiled x86-64 code keeps MXCSR there */
    for (int i = 0; i < 16; ++i) {
        memcpy(r.ymm[i], &st->xmm[i], 16);
        memcpy(r.ymm[i] + 16, &st->ymmh[i], 16);
    }
    RecompState *const outer = site_state;
    site_state = st;
    const uint64_t next = s->handler(s->tag, &r);
    site_state = outer;
    for (int i = 0; i < 16; ++i) st->gpr[order[i]] = g[i];
    for (int i = 0; i < 16; ++i) {
        memcpy(&st->xmm[i], r.ymm[i], 16);
        memcpy(&st->ymmh[i], r.ymm[i] + 16, 16);
    }
    return (RecompSiteResult){ next, r.rflags };
}

/* bbport_guest_call from a site's handler in recompiled code: the game's function as recompiled
 * code calls it (__recomp_dispatch: recompiled, or the original through the hybrid call path),
 * on the guest's stack below the frame the site is in. A native call there would run on the host
 * stack the recompiled frames are on, and the game code it reaches would enter recompiled code
 * again at the thread's host stack top: over those frames (the GPU heap's grow hook calling the
 * game's allocator lost a callee-saved register that way). Elsewhere: the native call. */
static uint64_t native_guest_call(BbGuestCallArgs *c) {
    typedef struct { uint64_t rax; double xmm0; } Result;  /* rax and xmm0 */
    typedef uint64_t U;
    typedef Result (*Fn)(U, U, U, U, U, U, double, double, double, double, double, double, double, double,
                         U, U, U, U, U, U, U, U);
    double x[8];
    memcpy(x, c->xmm, sizeof(x));
    const U *a = c->args, *s = c->stack;
    const Result r = ((Fn)(uintptr_t)(c->original ? c->original : c->address))(
        a[0], a[1], a[2], a[3], a[4], a[5], x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7],
        s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
    memcpy(&c->xmm0_result, &r.xmm0, 8);
    return r.rax;
}

static uint64_t aot_guest_call(BbGuestCallArgs *c) {
    RecompState *const cur = site_state;
    if (!cur) return native_guest_call(c);
    RecompState s = *cur;
    uint64_t sp = (cur->gpr[G_RSP] - 256) & ~(uint64_t)15;  /* past the red zone */
    sp -= 8 * BB_GUEST_CALL_STACK;                          /* the stack arguments (16-byte aligned) */
    memcpy((void *)(uintptr_t)sp, c->stack, 8 * BB_GUEST_CALL_STACK);
    sp -= 8;
    *(uint64_t *)(uintptr_t)sp = 0;                         /* the return slot */
    s.gpr[G_RSP] = sp;
    static const unsigned regs[6] = { G_RDI, G_RSI, G_RDX, G_RCX, G_R8, G_R9 };
    for (int i = 0; i < 6; ++i) s.gpr[regs[i]] = c->args[i];
    for (int i = 0; i < 8; ++i) {
        s.xmm[i] = 0;
        memcpy(&s.xmm[i], &c->xmm[i], 8);
        s.ymmh[i] = 0;
    }
    uint64_t target = c->address;
    if (c->original) {
        if (lookup(c->address - __recomp_slide)) site_bypass = c->address;  /* its entry site skipped once */
        else target = c->original;                                         /* native: the trampoline */
    }
    site_state = NULL;
    __recomp_dispatch(&s, target);
    site_state = cur;
    site_bypass = 0;
    memcpy(&c->xmm0_result, &s.xmm[0], 8);
    return s.gpr[G_RAX];
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

/* A function's code as the library was lifted from it: the port's guest sites (whose native
 * patches are jumps to its stubs) read as their original bytes. */
static uint64_t function_hash(const unsigned char *image, uint64_t start, uint64_t end) {
    const BbGuestSite *sites = NULL;
    const size_t n = bbport_guest_sites ? bbport_guest_sites(&sites) : 0;
    const uint64_t base = (uint64_t)(uintptr_t)image;
    int touched = 0;
    for (size_t i = 0; i < n && !touched; ++i)
        touched = sites[i].patch < base + end && sites[i].patch + sites[i].length > base + start;
    if (!touched) return code_hash(image + start, end - start);
    unsigned char *copy = malloc(end - start);
    if (!copy) return 0;
    memcpy(copy, image + start, end - start);
    for (size_t i = 0; i < n; ++i)
        for (uint32_t k = 0; k < sites[i].length; ++k) {
            const uint64_t at = sites[i].patch + k - base;
            if (at >= start && at < end) copy[at - start] = sites[i].original[k];
        }
    const uint64_t h = code_hash(copy, end - start);
    free(copy);
    return h;
}

/* BB_AOT_DUMP's `<dump>.sites`: the guest sites registered now, for the recompiler (one per line:
 * the patch's link address, its length, the original bytes in hex, where the handler runs ("at" the
 * patch, or "after" it) or "variant" and the replacement bytes in hex, name). */
static void dump_sites(const char *path, uint64_t base) {
    const BbGuestSite *sites = NULL;
    const size_t n = bbport_guest_sites ? bbport_guest_sites(&sites) : 0;
    char name[4096];
    snprintf(name, sizeof(name), "%s.sites", path);
    FILE *f = fopen(name, "w");
    if (!f) { printf("AOT: cannot write %s\n", name); return; }
    for (size_t i = 0; i < n; ++i) {
        fprintf(f, "0x%" PRIx64 " %u ", sites[i].patch - base, sites[i].length);
        for (uint32_t k = 0; k < sites[i].length; ++k) fprintf(f, "%02x", sites[i].original[k]);
        if (!sites[i].handler) {  /* a variant: its replacement bytes */
            fprintf(f, " variant ");
            for (uint32_t k = 0; k < sites[i].length; ++k) fprintf(f, "%02x", sites[i].replacement[k]);
            fprintf(f, " %s\n", sites[i].name ? sites[i].name : "?");
            continue;
        }
        fprintf(f, " %s %s\n", sites[i].address == sites[i].patch ? "at" : "after", sites[i].name ? sites[i].name : "?");
    }
    fclose(f);
    printf("AOT: %zu guest sites written to %s\n", n, name);
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
           "entries %" PRIu64 "; %" PRIu64 " guest site handler calls\n", n.entries, n.native_calls, n.host_calls,
           n.module_calls, n.native_jumps, n.max_depth, __atomic_load_n(&site_calls, __ATOMIC_RELAXED));
    fflush(stdout);
}

/* Every 30 s (the game ends with _exit: no atexit handlers run then); the sampled targets every
 * 60 s. */
static void *report_loop(void *arg) {
    (void)arg;
    for (unsigned n = 1;; ++n) {
        sleep(30);
        report();
        if (!(n & 1)) report_targets();
    }
    return NULL;
}

int runtime_aot_install(unsigned char *image, uint64_t size) {
    const char *dump = bb_env("BB_AOT_DUMP");
    if (dump && *dump) {
        dump_image(dump, image, size);
        dump_sites(dump, (uint64_t)(uintptr_t)image);
    }
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
    if (host_map_build()) { printf("AOT: no memory for the host code map\n"); return -1; }
    uint64_t changed = 0;
    for (uint64_t i = 0; i < table_size; ++i) {
        if (check[i][0] != table[i].guest || check[i][1] > size || check[i][1] <= check[i][0]) {
            printf("AOT: %s: bad check entry %" PRIu64 "\n", path, i);
            return -1;
        }
        if (function_hash(image, check[i][0], check[i][1]) != check[i][2] && changed++ < 5)
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
    const char *host_calls = bb_env("BB_AOT_HOST_CALLS");
    host_calls_light = !(host_calls && !strcmp(host_calls, "full"));
    sample_targets = bb_env_on("BB_AOT_TARGETS");
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
    /* The linked libc's longjmp, replaced: a longjmp to a recompiled setjmp (__recomp_setjmp)
     * resumes that recompiled code (recomp_hybrid.c). */
    const uint64_t *longjmps = dlsym(lib, "__recomp_longjmp");
    const uint64_t *longjmp_count = dlsym(lib, "__recomp_longjmp_size");
    for (uint64_t i = 0; longjmps && longjmp_count && i < *longjmp_count; ++i) {
        char why[160];
        if (recomp_hybrid_patch(__recomp_slide + longjmps[i], recomp_hybrid_longjmp, why)) {
            printf("AOT: longjmp at guest 0x%" PRIx64 ": %s\n", longjmps[i], why);
            return -1;
        }
    }
    if (&bbport_guest_call_hook) bbport_guest_call_hook = aot_guest_call;
    enabled = 1;
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
uint64_t runtime_aot_guest_pc(uint64_t host_pc) { (void)host_pc; return 0; }

#endif
