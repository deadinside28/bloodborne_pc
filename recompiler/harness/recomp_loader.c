// SPDX-License-Identifier: GPL-2.0-or-later
// Host runtime for recompiled images: maps an x86-64 ELF image (our own mapping, as bbport maps
// the game), applies its relocations, binds its imports to host functions, and provides the
// dispatcher, the import bridge and guest calls.
//
// Imports: a GOT entry holds the host function's real address. Original x86-64 code calls it
// natively; recompiled code reaches it through __recomp_dispatch, which finds it in the import
// table and calls it through a C bridge (on AArch64 that call is the SysV -> AAPCS64 bridge).
#define _GNU_SOURCE
#include "recomp_loader.h"
#include "recomp_x87.h"  // __recomp_x87 (defined there)

#include <dlfcn.h>
#include <signal.h>
#include <ucontext.h>
#include <elf.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

uint64_t __recomp_slide;

RecompImage recomp_image;
_Thread_local sigjmp_buf recomp_trap_jmp;
_Thread_local int recomp_trap_armed;
_Atomic uint64_t recomp_dispatch_misses, recomp_import_calls;

static void die(const char *m) { perror(m); exit(2); }

// ---- imports ---------------------------------------------------------------------------------

typedef struct { uint64_t host; const char *name; RecompBridge bridge; } Import;
static Import imports[1024];
static int n_imports;

// Generic bridge: up to six integer/pointer arguments, integer result (enough for the libc
// functions zlib and similar code import). Functions with float or stack arguments need a
// bridge of their own (recomp_set_bridge), as the game's NID imports will.
static void bridge_int6(RecompState *st, uint64_t host) {
    uint64_t (*f)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) = (void *)(uintptr_t)host;
    st->gpr[G_RAX] = f(st->gpr[G_RDI], st->gpr[G_RSI], st->gpr[G_RDX], st->gpr[G_RCX], st->gpr[G_R8], st->gpr[G_R9]);
}

static void bridge_stack_chk_fail(RecompState *st, uint64_t host) {
    (void)host;
    fprintf(stderr, "guest stack protector: canary overwritten (guest rsp %#" PRIx64 ")\n", st->gpr[G_RSP]);
    if (recomp_trap_armed) siglongjmp(recomp_trap_jmp, 4);
    abort();
}

// Imports bound by name, before dlsym: what a static build (no dynamic symbol lookup, e.g. the
// AArch64 test binaries) needs, and the shape the game's NID import table will take.
static void bridge_unbound(RecompState *st, uint64_t host);
static int unbound_count;
static char unbound_slots[256];  // distinct addresses standing for imports nobody provides

static const struct { const char *name; void *fn; } host_imports[] = {
    { "malloc", (void *)malloc }, { "free", (void *)free }, { "calloc", (void *)calloc },
    { "realloc", (void *)realloc }, { "memcpy", (void *)memcpy }, { "memmove", (void *)memmove },
    { "memset", (void *)memset }, { "memchr", (void *)memchr }, { "memcmp", (void *)memcmp },
    { "strlen", (void *)strlen }, { "strerror", (void *)strerror }, { "read", (void *)read },
    { "write", (void *)write }, { "close", (void *)close }, { "lseek64", (void *)lseek },
    { "lseek", (void *)lseek }, { "__errno_location", (void *)__errno_location },
};

static Import *find_import(uint64_t host) {
    for (int i = 0; i < n_imports; i++)
        if (imports[i].host == host) return &imports[i];
    return NULL;
}

static uint64_t bind_import(const char *name, int weak) {
    void *p = NULL;
    RecompBridge b = bridge_int6;
    for (size_t i = 0; i < sizeof host_imports / sizeof host_imports[0]; i++)
        if (!strcmp(host_imports[i].name, name)) p = host_imports[i].fn;
    if (!p && !strcmp(name, "__stack_chk_fail")) { p = (void *)abort; b = bridge_stack_chk_fail; }
    if (!p) p = dlsym(RTLD_DEFAULT, name);
    if (!p) {
        if (weak) return 0;
        // Nobody provides it: a placeholder address whose bridge reports the call. Original
        // code calling it natively would crash, so this is only reached in static builds.
        if (unbound_count >= (int)sizeof unbound_slots) { fprintf(stderr, "too many unbound imports\n"); exit(2); }
        p = &unbound_slots[unbound_count++];
        b = bridge_unbound;
    }
    uint64_t host = (uint64_t)(uintptr_t)p;
    if (!find_import(host) && n_imports < 1024) imports[n_imports++] = (Import){ host, strdup(name), b };
    return host;
}

static void bridge_unbound(RecompState *st, uint64_t host) {
    (void)st;
    Import *im = find_import(host);
    fprintf(stderr, "call to unbound import %s\n", im ? im->name : "?");
    if (recomp_trap_armed) siglongjmp(recomp_trap_jmp, 5);
    abort();
}

void recomp_set_bridge(const char *name, RecompBridge bridge) {
    uint64_t host = (uint64_t)(uintptr_t)dlsym(RTLD_DEFAULT, name);
    Import *im = host ? find_import(host) : NULL;
    if (im) im->bridge = bridge;
}

// ---- image -----------------------------------------------------------------------------------

static const Elf64_Sym *dynsym;
static const char *dynstr;
static size_t n_dynsym;

uint64_t recomp_symbol(const char *name) {
    size_t len = strlen(name);
    for (size_t k = 0; k < n_dynsym; k++) {
        const char *nm = dynstr + dynsym[k].st_name;
        if (dynsym[k].st_shndx == SHN_UNDEF || strncmp(nm, name, len) || (nm[len] && nm[len] != '@')) continue;
        return dynsym[k].st_value;
    }
    return 0;
}

static uint64_t resolve(uint32_t symidx, uint8_t *at, int *weak_missing) {
    const Elf64_Sym *s = &dynsym[symidx];
    const char *nm = dynstr + s->st_name;
    *weak_missing = 0;
    if (s->st_shndx != SHN_UNDEF) return (uint64_t)(uintptr_t)at + s->st_value;  // defined here
    uint64_t host = bind_import(nm, ELF64_ST_BIND(s->st_info) == STB_WEAK);
    if (!host) *weak_missing = 1;
    return host;
}

void recomp_load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) die(path);
    struct stat sb;
    fstat(fd, &sb);
    uint8_t *file = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file == MAP_FAILED) die("mmap file");
    recomp_image.file = file;
    Elf64_Ehdr *eh = (Elf64_Ehdr *)file;
    if (memcmp(eh->e_ident, ELFMAG, 4) || eh->e_machine != EM_X86_64) { fprintf(stderr, "%s: not x86-64 ELF\n", path); exit(2); }
    Elf64_Phdr *ph = (Elf64_Phdr *)(file + eh->e_phoff);
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD) {
            if (ph[i].p_vaddr < lo) lo = ph[i].p_vaddr;
            if (ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
        }
    lo &= ~0xfffull;
    hi = (hi + 0xfff) & ~0xfffull;
    uint8_t *base = mmap(NULL, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) die("mmap image");
    uint8_t *at = base - lo;
    __recomp_slide = (uint64_t)(uintptr_t)at;
    recomp_image.base = at;
    Elf64_Dyn *dyn = NULL;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) memcpy(at + ph[i].p_vaddr, file + ph[i].p_offset, ph[i].p_filesz);
        if (ph[i].p_type == PT_DYNAMIC) dyn = (Elf64_Dyn *)(file + ph[i].p_offset);
    }
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0, symtab = 0, strtab = 0, relr = 0, relrsz = 0;
    for (Elf64_Dyn *d = dyn; d && d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_RELA: rela = d->d_un.d_ptr; break;
        case DT_RELASZ: relasz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = d->d_un.d_ptr; break;
        case DT_PLTRELSZ: pltrelsz = d->d_un.d_val; break;
        case DT_SYMTAB: symtab = d->d_un.d_ptr; break;
        case DT_STRTAB: strtab = d->d_un.d_ptr; break;
#ifndef DT_RELR
#define DT_RELR 36
#define DT_RELRSZ 35
#endif
        case DT_RELR: relr = d->d_un.d_ptr; break;
        case DT_RELRSZ: relrsz = d->d_un.d_val; break;
        }
    }
    // the dynamic symbol table, read from the file (section headers give its size)
    Elf64_Shdr *sh = (Elf64_Shdr *)(file + eh->e_shoff);
    for (int i = 0; i < eh->e_shnum; i++)
        if (sh[i].sh_type == SHT_DYNSYM) {
            dynsym = (const Elf64_Sym *)(file + sh[i].sh_offset);
            n_dynsym = sh[i].sh_size / sizeof(Elf64_Sym);
            dynstr = (const char *)(file + sh[sh[i].sh_link].sh_offset);
        }
    (void)symtab; (void)strtab;
    // DT_RELR (packed relative relocations, -z pack-relative-relocs): an even entry is an address
    // to relocate; an odd entry is a bitmap of the next 63 words after the last address.
    uint64_t next = 0;
    for (const uint64_t *e = (const uint64_t *)(at + relr); relrsz && e < (const uint64_t *)(at + relr + relrsz); e++) {
        if (!(*e & 1)) {
            uint64_t *w = (uint64_t *)(at + *e);
            *w += (uint64_t)(uintptr_t)at;
            next = *e + 8;
        } else {
            for (int bit = 1; bit < 64; bit++)
                if (*e >> bit & 1) *(uint64_t *)(at + next + (uint64_t)(bit - 1) * 8) += (uint64_t)(uintptr_t)at;
            next += 63 * 8;
        }
    }
    struct { uint64_t off, size; } tabs[2] = { { rela, relasz }, { jmprel, pltrelsz } };
    for (int t = 0; t < 2; t++) {
        if (!tabs[t].size) continue;
        // the tables are addressed by link address: read them from the mapped image
        Elf64_Rela *r = (Elf64_Rela *)(at + tabs[t].off);
        Elf64_Rela *end = (Elf64_Rela *)(at + tabs[t].off + tabs[t].size);
        for (; r < end; r++) {
            uint64_t *where = (uint64_t *)(at + r->r_offset);
            int weak_missing;
            switch (ELF64_R_TYPE(r->r_info)) {
            case R_X86_64_NONE: break;
            case R_X86_64_RELATIVE: *where = (uint64_t)(uintptr_t)at + r->r_addend; break;
            case R_X86_64_GLOB_DAT:
            case R_X86_64_JUMP_SLOT: *where = resolve(ELF64_R_SYM(r->r_info), at, &weak_missing); break;
            case R_X86_64_64: *where = resolve(ELF64_R_SYM(r->r_info), at, &weak_missing) + r->r_addend; break;
            default:
                fprintf(stderr, "unsupported relocation type %lu\n", (unsigned long)ELF64_R_TYPE(r->r_info));
                exit(2);
            }
        }
    }
#if defined(__x86_64__)
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & PF_X)) {
            uint64_t s = ph[i].p_vaddr & ~0xfffull, e = (ph[i].p_vaddr + ph[i].p_memsz + 0xfff) & ~0xfffull;
            if (mprotect(at + s, e - s, PROT_READ | PROT_EXEC)) die("mprotect");
        }
#endif
    recomp_image.eh = eh;
}

// The guest's time stamp counter (rdtsc). bbport will return the PS4 TSC here; the harness uses
// the host's counter (rdtsc on x86-64, the virtual counter on AArch64).
uint64_t __recomp_rdtsc(void) {
#if defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
#error "no counter for this host"
#endif
}

// cpuid for recompiled code: the host's answer (bbport will report the PS4's Jaguar).
void __recomp_cpuid(uint32_t leaf, uint32_t sub, uint32_t out[4]) {
#if defined(__x86_64__)
    __asm__ volatile("cpuid" : "=a"(out[0]), "=b"(out[1]), "=c"(out[2]), "=d"(out[3]) : "a"(leaf), "c"(sub));
#else
    (void)sub;
    memset(out, 0, 16);
    if (leaf == 0) { out[0] = 1; memcpy(&out[1], "Auth", 4); memcpy(&out[3], "enti", 4); memcpy(&out[2], "cAMD", 4); }
#endif
}


// ---- AArch64: misaligned acquire loads / release stores (--tso-rcpc) ------------------------------
// ldapr/ldapur/stlr/stlur assume alignment. LSE2 CPUs accept misalignment inside a 16-byte granule
// and raise an alignment fault (SIGBUS) across one; x86 code does such accesses legitimately. The
// handler performs the faulting access byte-wise with the barrier it stands for, and continues at
// the next instruction. recomp_alignment_faults counts them (each costs a signal).
_Atomic uint64_t recomp_alignment_faults;
#if defined(__aarch64__)
static void alignment_fault(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    uint64_t *x = (uint64_t *)uc->uc_mcontext.regs;
    uint32_t in = *(const uint32_t *)(uintptr_t)uc->uc_mcontext.pc;
    unsigned size = 1u << (in >> 30), rt = in & 31, rn = (in >> 5) & 31;
    uint64_t base = rn == 31 ? uc->uc_mcontext.sp : x[rn];
    int load = -1, sign_to = 0;
    int64_t off = 0;
    if ((in & 0x3ffffc00) == 0x38bfc000) load = 1;                 // ldapr{b,h,}
    else if ((in & 0x3ffffc00) == 0x089ffc00) load = 0;            // stlr{b,h,}
    else if ((in & 0x3f200c00) == 0x19000000) {                     // stlur / ldapur / ldapurs (LRCPC2)
        unsigned opc = (in >> 22) & 3;
        off = ((int64_t)(in << 11)) >> 23;                          // imm9, bits 20..12
        load = opc != 0;
        sign_to = opc == 2 ? 64 : opc == 3 ? 32 : 0;
    }
    if (load < 0) { signal(sig, SIG_DFL); return; }                  // not ours: default action
    uint8_t *p = (uint8_t *)(uintptr_t)(base + (uint64_t)off);
    if (load) {
        uint64_t v = 0;
        memcpy(&v, p, size);
        __asm__ volatile("dmb ishld" ::: "memory");
        if (sign_to) {
            int64_t sv = (int64_t)(v << (64 - 8 * size)) >> (64 - 8 * size);
            v = sign_to == 32 ? (uint32_t)sv : (uint64_t)sv;
        }
        if (rt != 31) x[rt] = v;
    } else {
        uint64_t v = rt == 31 ? 0 : x[rt];
        __asm__ volatile("dmb ish" ::: "memory");
        memcpy(p, &v, size);
    }
    (void)si;
    recomp_alignment_faults++;
    uc->uc_mcontext.pc += 4;
}

__attribute__((constructor)) static void install_alignment_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = alignment_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGBUS, &sa, NULL);
}
#endif

// ---- dispatch, traps, guest calls ----------------------------------------------------------------

RecompFn recomp_lookup(uint64_t link) {
    size_t lo = 0, hi = __recomp_table_size;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (__recomp_table[mid].guest == link) return __recomp_table[mid].fn;
        if (__recomp_table[mid].guest < link) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

void (*recomp_dispatch_fallback)(RecompState *st, uint64_t target);
void (*recomp_dispatch_jump_fallback)(RecompState *st, uint64_t target);

void __recomp_dispatch(RecompState *st, uint64_t target);

void __recomp_dispatch_jump(RecompState *st, uint64_t target) {
    RecompFn fn = recomp_lookup(target - __recomp_slide);
    if (fn) { fn(st); return; }
    if (recomp_dispatch_jump_fallback) { recomp_dispatch_jump_fallback(st, target); return; }  // hybrid: no return
    __recomp_dispatch(st, target);
}

void __recomp_dispatch(RecompState *st, uint64_t target) {
    RecompFn fn = recomp_lookup(target - __recomp_slide);
    if (fn) { fn(st); return; }
    if (recomp_dispatch_fallback) { recomp_dispatch_fallback(st, target); return; }  // hybrid: native
    Import *im = find_import(target);
    if (im) {
        recomp_import_calls++;
        // Reached by `call [got]` (the guest pushed a return address) or `jmp [got]` (a tail call:
        // the address on the stack is the caller's). Either way the host function's return is
        // the guest `ret`: pop it.
        im->bridge(st, target);
        st->gpr[G_RSP] += 8;
        return;
    }
    recomp_dispatch_misses++;
    fprintf(stderr, "dispatch miss: guest %#" PRIx64 " (link %#" PRIx64 ")\n", target, target - __recomp_slide);
    if (recomp_trap_armed) siglongjmp(recomp_trap_jmp, 1);
    abort();
}

// Guest sites (corpus/sites.S; in bbport: src/runtime_aot.c): the handler does the instruction at
// the site itself and goes on past it, as the original code, which runs the instruction, does.
typedef struct { uint64_t next, flags; } RecompSiteResult;
_Atomic uint64_t recomp_site_calls;

static uint64_t parity_flag(uint64_t v) { return __builtin_parity((unsigned)(v & 0xff)) ? 0 : 4; }

RecompSiteResult __recomp_guest_site(RecompState *st, uint64_t link, uint64_t flags) {
    static uint64_t add1, vec1, ret1, jmp1, jmp1_to;
    if (!add1) {
        add1 = recomp_symbol("gsite_add_1"); vec1 = recomp_symbol("gsite_vec_1");
        ret1 = recomp_symbol("gsite_ret_1"); jmp1 = recomp_symbol("gsite_jmp_1");
        jmp1_to = recomp_symbol("gtarget_jmp_1");
    }
    if (link == jmp1) return (RecompSiteResult){ __recomp_slide + jmp1_to, flags };  // the jmp's target
    if (link == ret1) {  // xor rax, 0x55; ret
        st->gpr[G_RAX] ^= 0x55;
        const uint64_t back = *(const uint64_t *)(uintptr_t)st->gpr[G_RSP];
        st->gpr[G_RSP] += 8;
        return (RecompSiteResult){ back, flags };
    }
    recomp_site_calls++;
    if (link == add1) {  // add rax, rdi
        const uint64_t a = st->gpr[G_RAX], b = st->gpr[G_RDI], r = a + b;
        flags &= ~(uint64_t)0x8d5;
        flags |= (r < a) | parity_flag(r) | (((a ^ b ^ r) >> 4 & 1) << 4) | (uint64_t)(r == 0) << 6 |
                 (r >> 63) << 7 | ((~(a ^ b) & (a ^ r)) >> 63) << 11;
        st->gpr[G_RAX] = r;
        return (RecompSiteResult){ __recomp_slide + link + 3, flags };
    }
    if (link == vec1) {  // vpaddd xmm0, xmm0, xmm1
        uint32_t x[4], y[4];
        memcpy(x, &st->xmm[0], 16);
        memcpy(y, &st->xmm[1], 16);
        for (int i = 0; i < 4; ++i) x[i] += y[i];
        memcpy(&st->xmm[0], x, 16);
        st->ymmh[0] = 0;
        return (RecompSiteResult){ __recomp_slide + link + 4, flags };
    }
    return (RecompSiteResult){ 0, flags };  // no handler here: the instruction runs
}

void __recomp_trap(RecompState *st, uint64_t addr, int32_t kind) {
    (void)st;
    static const char *kinds[] = { "?", "unsupported instruction", "int3/ud2", "divide error", "?", "?", "no-return function returned" };
    if (kind >= 0x100) fprintf(stderr, "trap: int %#x at link %#" PRIx64 "\n", kind - 0x100, addr - __recomp_slide);
    else if (kind == 8) fprintf(stderr, "trap: privileged instruction or syscall at link %#" PRIx64 "\n", addr - __recomp_slide);
    else fprintf(stderr, "trap: %s at link %#" PRIx64 "\n", kind >= 0 && kind < 7 ? kinds[kind] : "?", addr - __recomp_slide);
    if (recomp_trap_armed) siglongjmp(recomp_trap_jmp, 2);
    abort();
}

// Per host thread: the guest thread block FS points at (the stack protector reads its canary at
// fs:[0x28]) and an 8 MiB guest stack.
static _Thread_local uint8_t guest_tcb[4096] __attribute__((aligned(64)));
static _Thread_local uint8_t *guest_stack;
#define GUEST_STACK_SIZE (8u << 20)

int recomp_call(RecompFn fn, const uint64_t *args, int nargs, const uint64_t *fargs, int nfargs,
                RecompState *st, char *why) {
    memset(st, 0, sizeof *st);
    st->mxcsr = 0x1f80;  // power-on default: all exceptions masked, round to nearest
    for (int i = 0; i < nfargs && i < 8; i++) st->xmm[i] = fargs[i];
    *(uint64_t *)(guest_tcb + 0x28) = 0x2f8a63c1d94e7b05ull;
    *(uint64_t *)guest_tcb = (uint64_t)(uintptr_t)guest_tcb;  // TCB self pointer, as on x86-64 Linux
    st->fs_base = (uint64_t)(uintptr_t)guest_tcb;
    static const int argreg[6] = { G_RDI, G_RSI, G_RDX, G_RCX, G_R8, G_R9 };
    for (int i = 0; i < nargs && i < 6; i++) st->gpr[argreg[i]] = args[i];
    static const int saved[] = { G_RBX, G_RBP, G_R12, G_R13, G_R14, G_R15 };
    uint64_t saved_v[6];
    for (int i = 0; i < 6; i++) st->gpr[saved[i]] = saved_v[i] = 0x5a5a000000000000ull ^ ((uint64_t)i << 40) ^ (uint64_t)(uintptr_t)fn;
    if (!guest_stack) {
        guest_stack = mmap(NULL, GUEST_STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (guest_stack == MAP_FAILED) die("guest stack");
    }
    uint64_t top = ((uint64_t)(uintptr_t)(guest_stack + GUEST_STACK_SIZE - 4096)) & ~15ull;
    int nstack = nargs > 6 ? nargs - 6 : 0;
    top -= (uint64_t)((nstack + 1) & ~1) * 8;  // keep rsp + 8 16-byte aligned at entry
    for (int i = 0; i < nstack; i++) ((uint64_t *)(uintptr_t)top)[i] = args[6 + i];
    st->gpr[G_RSP] = top - 8;
    *(uint64_t *)(uintptr_t)(top - 8) = 0x5e57111e5e57111eull;  // return address sentinel
    recomp_trap_armed = 1;
    if (sigsetjmp(recomp_trap_jmp, 0)) { recomp_trap_armed = 0; strcpy(why, "trapped"); return -1; }
    fn(st);
    recomp_trap_armed = 0;
    if (st->gpr[G_RSP] != top) {
        sprintf(why, "rsp %#" PRIx64 " after return, expected %#" PRIx64, st->gpr[G_RSP], top);
        return -1;
    }
    for (int i = 0; i < 6; i++)
        if (st->gpr[saved[i]] != saved_v[i]) { sprintf(why, "callee-saved register %d clobbered", saved[i]); return -1; }
    return 0;
}
