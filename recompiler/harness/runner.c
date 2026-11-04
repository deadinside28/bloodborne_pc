// SPDX-License-Identifier: GPL-2.0-or-later
// Differential test runner: maps an x86-64 ELF image (as bbport's loader maps the game: our own
// mapping, relocations applied), then for every exported t_* function compares the original
// x86-64 code against the recompiled code on the same inputs.
//
//   runner IMAGE.so [-n VECTORS] [-f FILTER]
//
// The original is called only on x86-64 hosts; on AArch64 the expected results come from a
// record file written on x86-64 (-r FILE to record, -c FILE to check).
#define _GNU_SOURCE
#include <elf.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_loader.h"

// Exported t_* / tf_* functions (link address), from the image's symbol table.
typedef struct { const char *name; uint64_t link; int fp; } Export;
static Export exports[4096];
static int n_exports;

static void find_exports(void) {
    Elf64_Ehdr *eh = recomp_image.eh;
    uint8_t *file = recomp_image.file;
    Elf64_Shdr *sh = (Elf64_Shdr *)(file + eh->e_shoff);
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_DYNSYM && sh[i].sh_type != SHT_SYMTAB) continue;
        Elf64_Sym *sym = (Elf64_Sym *)(file + sh[i].sh_offset);
        const char *str = (const char *)(file + sh[sh[i].sh_link].sh_offset);
        for (size_t k = 0; k < sh[i].sh_size / sizeof(Elf64_Sym); k++) {
            const char *nm = str + sym[k].st_name;
            int fp = !strncmp(nm, "tf_", 3);
            if (ELF64_ST_TYPE(sym[k].st_info) != STT_FUNC || (strncmp(nm, "t_", 2) && !fp) || strchr(nm, '.')) continue;
            int dup = 0;
            for (int j = 0; j < n_exports; j++) dup |= exports[j].link == sym[k].st_value;
            if (!dup && n_exports < 4096) exports[n_exports++] = (Export){ strdup(nm), sym[k].st_value, fp };
        }
    }
}

typedef uint64_t (*IntFn)(uint64_t, uint64_t, uint64_t, uint8_t *);
typedef double (*FpFn)(double, double, uint64_t, uint8_t *);

static double as_f64(uint64_t v) { double d; memcpy(&d, &v, 8); return d; }
static uint64_t f64_bits(double d) { uint64_t v; memcpy(&v, &d, 8); return v; }
// FP inputs: signed zeros, infinities, NaN, denormals, integer-range edges, ordinary values.
static const double fp_edges[] = { 0.0, -0.0, 1.0, -1.0, 0.5, -1.5, 2.5, 3.0, 1e-310, -1e-310, 1e-40,
    3.4e38, 1e39, -1e39, 2147483647.0, 2147483648.0, -2147483648.0, -2147483649.0,
    9.2233720368547758e18, -9.2233720368547758e18, 1e19, 4294967295.0, 255.5,
    __builtin_inf(), -__builtin_inf(), __builtin_nan("") };
#define N_FP_EDGES (sizeof fp_edges / sizeof fp_edges[0])

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t next_rand(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static const uint64_t edges[] = { 0, 1, 2, 3, 7, 8, 15, 16, 31, 32, 63, 64, 127, 128, 255, 256,
    0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000, 0xffffffff, 0x100000000,
    0x7fffffffffffffff, 0x8000000000000000, 0xffffffffffffffff, 0xfffffffffffffffe };
#define N_EDGES (sizeof edges / sizeof edges[0])

// NaN sign and payload are not preserved (see recompiler/README.md): x86 returns the first
// operand's NaN and a negative default NaN, AArch64 and LLVM's operand commutation differ. For
// tf_* functions any NaN matches any NaN, as 32-bit floats and as 64-bit doubles.
static int is_nan64(uint64_t v) { return ((v >> 52) & 0x7ff) == 0x7ff && (v & 0xfffffffffffffull); }
static int is_nan32(uint32_t v) { return ((v >> 23) & 0xff) == 0xff && (v & 0x7fffff); }
static int first_difference(const uint8_t *want, const uint8_t *got, int fp_tolerant) {
    for (int k = 0; k < 256; k++) {
        if (want[k] == got[k]) continue;
        if (fp_tolerant) {
            uint32_t w4, g4; uint64_t w8, g8;
            memcpy(&w4, want + (k & ~3), 4); memcpy(&g4, got + (k & ~3), 4);
            memcpy(&w8, want + (k & ~7), 8); memcpy(&g8, got + (k & ~7), 8);
            if ((is_nan32(w4) && is_nan32(g4)) || (is_nan64(w8) && is_nan64(g8))) continue;
        }
        return k;
    }
    return -1;
}

static int run_lifted(RecompFn fn, int fp, uint64_t a, uint64_t b, uint64_t c, uint8_t *buf, uint64_t *ret, char *why) {
    RecompState st;
    uint64_t p = (uint64_t)(uintptr_t)buf;
    if (fp) {  // double tf_x(double x, double y, uint64_t a, uint8_t *buf)
        const uint64_t args[2] = { c, p }, fargs[2] = { a, b };
        if (recomp_call(fn, args, 2, fargs, 2, &st, why)) return -1;
        *ret = (uint64_t)st.xmm[0];
    } else {   // uint64_t t_x(uint64_t a, uint64_t b, uint64_t c, uint8_t *buf)
        const uint64_t args[4] = { a, b, c, p };
        if (recomp_call(fn, args, 4, NULL, 0, &st, why)) return -1;
        *ret = st.gpr[G_RAX];
    }
    return 0;
}

static void die(const char *m) { perror(m); exit(2); }

int main(int argc, char **argv) {
    const char *image = NULL, *filter = NULL, *rec = NULL, *chk = NULL;
    int n = 300, verbose = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) filter = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) rec = argv[++i];
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) chk = argv[++i];
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else image = argv[i];
    }
    if (!image) { fprintf(stderr, "usage: runner IMAGE [-n N] [-f FILTER] [-r REC | -c REC] [-v]\n"); return 2; }
    recomp_load(image);
    find_exports();
#if !defined(__x86_64__)
    if (!chk) { fprintf(stderr, "non-x86 host: expected results come from a record file (-c)\n"); return 2; }
#endif
    FILE *rf = rec ? fopen(rec, "wb") : NULL, *cf = chk ? fopen(chk, "rb") : NULL;
    if ((rec && !rf) || (chk && !cf)) die("record file");
    int tests = 0, failed = 0, missing = 0;
    static uint8_t buf_init[256], buf_a[256], buf_b[256];
    for (int e = 0; e < n_exports; e++) {
        Export *x = &exports[e];
        if (filter && !strstr(x->name, filter)) continue;
        RecompFn fn = recomp_lookup(x->link);
        if (!fn) { printf("MISSING %s (not recompiled)\n", x->name); missing++; continue; }
        IntFn orig = (IntFn)(uintptr_t)(__recomp_slide + x->link);
        (void)orig;
        int bad = 0, n_edge = x->fp ? (int)(N_FP_EDGES * N_FP_EDGES) : (int)(N_EDGES * N_EDGES);
        int total = n + n_edge;
        rng = 0x9e3779b97f4a7c15ull ^ x->link;
        for (int v = 0; v < total && bad < 3; v++) {
            uint64_t a, b, c;
            if (x->fp && v < n_edge) { a = f64_bits(fp_edges[v / N_FP_EDGES]); b = f64_bits(fp_edges[v % N_FP_EDGES]); c = edges[v % N_EDGES]; }
            else if (x->fp) {  // magnitudes from tiny to huge, both signs
                a = f64_bits((double)(int64_t)next_rand() / (double)(1ull << (next_rand() % 63)));
                b = f64_bits((double)(int32_t)next_rand() / 65536.0); c = next_rand();
            }
            else if (v < n_edge) { a = edges[v / N_EDGES]; b = edges[v % N_EDGES]; c = edges[(v * 7) % N_EDGES]; }
            else { a = next_rand(); b = next_rand(); c = next_rand(); if (v & 1) { a &= 0xffff; b &= 0xff; } }
            for (int i = 0; i < 256; i++) buf_init[i] = (uint8_t)next_rand();
            uint64_t want, got;
            memcpy(buf_a, buf_init, 256);
            if (cf) {
                if (fread(&want, 8, 1, cf) != 1 || fread(buf_a, 256, 1, cf) != 1) { fprintf(stderr, "record file short\n"); return 2; }
            } else {
#if defined(__x86_64__)
                want = x->fp ? f64_bits(((FpFn)(uintptr_t)orig)(as_f64(a), as_f64(b), c, buf_a)) : orig(a, b, c, buf_a);
#endif
            }
            if (rf) { fwrite(&want, 8, 1, rf); fwrite(buf_a, 256, 1, rf); }
            memcpy(buf_b, buf_init, 256);
            char why[160] = "";
            int rc = run_lifted(fn, x->fp, a, b, c, buf_b, &got, why);
            if (rc == 0 && got != want && !(x->fp && is_nan64(got) && is_nan64(want)))
                sprintf(why, "returned %#" PRIx64 ", expected %#" PRIx64, got, want);
            else if (rc == 0) {
                int k = first_difference(buf_a, buf_b, x->fp);
                if (k >= 0) sprintf(why, "buf[%d] = %#x, expected %#x", k, buf_b[k], buf_a[k]);
            }
            if (why[0]) {
                if (!bad) printf("FAIL %s\n", x->name);
                printf("    a=%#" PRIx64 " b=%#" PRIx64 " c=%#" PRIx64 ": %s\n", a, b, c, why);
                bad++;
            }
        }
        tests++;
        if (bad) failed++;
        else if (verbose) printf("ok   %s\n", x->name);
    }
    printf("%d functions tested, %d failed, %d not recompiled; dispatch misses: %" PRIu64 "\n",
           tests, failed, missing, recomp_dispatch_misses);
    return failed || missing ? 1 : 0;
}
