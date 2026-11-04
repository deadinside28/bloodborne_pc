// SPDX-License-Identifier: GPL-2.0-or-later
// Whole-library differential test: the system's libz.so (an unmodified, optimised build) runs as
// the original x86-64 code and as recompiled code on the same inputs; every result, output
// byte and stream field is compared. libz's imports (malloc, free, memcpy, memset, ...) are bound
// to the host libc (recomp_loader.c), as the game's will be bound to bbport's runtime.
//
//   zlib_test /usr/lib/libz.so.1 [-q] [-d]
// Every output the checks look at is hashed into a digest (printed at the end). -d runs only the
// recompiled code (for hosts that cannot run x86-64, e.g. AArch64): its digest must equal the
// one an x86-64 run prints, where the recompiled outputs were compared to the original's.
// zlib.h is used only for z_stream and the constants; this program does not link libz.
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "recomp_loader.h"

typedef uint64_t (*Fn8)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static int checks, failures, quiet, lifted_only;
static uint64_t digest = 0xcbf29ce484222325ull;  // FNV-1a over all checked outputs

static void dg(const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { digest ^= b[i]; digest *= 0x100000001b3ull; }
}
static void dgv(uint64_t v) { dg(&v, 8); }

static uint64_t link_of(const char *name) {
    uint64_t l = recomp_symbol(name);
    if (!l) { fprintf(stderr, "no export %s\n", name); exit(2); }
    return l;
}

static uint64_t lifted(const char *name, const uint64_t *a, int n);

static uint64_t orig(const char *name, const uint64_t *a, int n) {
    if (lifted_only) return lifted(name, a, n);
    uint64_t v[8] = { 0 };
    memcpy(v, a, (size_t)n * 8);
    Fn8 f = (Fn8)(uintptr_t)(__recomp_slide + link_of(name));
    return f(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
}

static uint64_t lifted(const char *name, const uint64_t *a, int n) {
    RecompFn fn = recomp_lookup(link_of(name));
    if (!fn) { fprintf(stderr, "%s was not recompiled\n", name); exit(2); }
    RecompState st;
    char why[200] = "";
    if (recomp_call(fn, a, n, NULL, 0, &st, why)) {
        printf("FAIL %s: %s\n", name, why);
        failures++;
        return 0xdeaddeaddeaddeadull;
    }
    return st.gpr[G_RAX];
}

#define P(x) ((uint64_t)(uintptr_t)(x))

static void check(int ok, const char *what) {
    checks++;
    if (!ok) { failures++; printf("FAIL %s\n", what); }
    else if (!quiet && checks % 500 == 0) { printf("  %d checks\n", checks); fflush(stdout); }
}

// ---- inputs ------------------------------------------------------------------------------------

static uint64_t rng = 0x243f6a8885a308d3ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }

enum { D_RANDOM, D_TEXT, D_ZEROS, D_RUNS, D_MIXED, D_KINDS };
static const char *kind_name[] = { "random", "text", "zeros", "runs", "mixed" };

static void fill(uint8_t *p, size_t n, int kind) {
    static const char *words[] = { "the ", "hunter ", "must ", "hunt ", "blood ", "moon ", "Yharnam ", "dream ",
                                   "\n", "beast ", "old ", "ones ", "insight ", "of ", "a " };
    size_t i = 0;
    switch (kind) {
    case D_RANDOM: for (; i < n; i++) p[i] = (uint8_t)rnd(); break;
    case D_ZEROS: memset(p, 0, n); break;
    case D_TEXT:
        while (i < n) { const char *w = words[rnd() % 15]; for (; *w && i < n; w++) p[i++] = (uint8_t)*w; }
        break;
    case D_RUNS:
        while (i < n) { uint8_t c = (uint8_t)rnd(); size_t r = 1 + rnd() % 300; for (; r-- && i < n;) p[i++] = c; }
        break;
    default:
        while (i < n) { size_t r = 1 + rnd() % 2000; int k = rnd() % 4; fill(p + i, r < n - i ? r : n - i, k == 3 ? D_RUNS : k); i += r; }
    }
}

static const size_t sizes[] = { 0, 1, 2, 7, 64, 255, 1000, 4096, 32768, 65537, 300000 };
#define N_SIZES (sizeof sizes / sizeof sizes[0])

// ---- checksums ----------------------------------------------------------------------------------

static void test_checksums(void) {
    static uint8_t buf[300001];
    char what[160];
    for (int r = 0; r < 60; r++) {
        size_t n = r < 30 ? sizes[r % N_SIZES] + (size_t)(r / N_SIZES) : rnd() % 300000;
        fill(buf, n, r % D_KINDS);
        uint64_t seed = r & 1 ? rnd() : (r & 2 ? 1 : 0);
        for (int k = 0; k < 3; k++) {
            const char *f = (const char *[]){ "adler32", "crc32", "crc32_z" }[k];
            const uint64_t a[3] = { seed, P(buf + (r & 3)), n };
            uint64_t o = orig(f, a, 3) & 0xffffffff, l = lifted(f, a, 3) & 0xffffffff;
            dgv(l);
            snprintf(what, sizeof what, "%s(len %zu): %#" PRIx64 " recompiled, %#" PRIx64 " original", f, n, l, o);
            check(o == l, what);
        }
        const uint64_t c[3] = { rnd(), rnd(), rnd() % 100000 };
        for (int k = 0; k < 2; k++) {
            const char *f = k ? "crc32_combine" : "adler32_combine";
            uint64_t o = orig(f, c, 3), l = lifted(f, c, 3);
            dgv(l);
            snprintf(what, sizeof what, "%s: %#" PRIx64 " vs %#" PRIx64, f, l, o);
            check(o == l, what);
        }
    }
    const uint64_t one[1] = { 123456 };
    uint64_t cb = lifted("compressBound", one, 1);
    dgv(cb);
    check(orig("compressBound", one, 1) == cb, "compressBound");
}

// ---- one-shot compress / uncompress ----------------------------------------------------------------

static void test_compress(void) {
    size_t cap = 400000;
    uint8_t *src = malloc(cap), *o = malloc(cap), *l = malloc(cap), *u1 = malloc(cap), *u2 = malloc(cap);
    char what[200];
    for (int kind = 0; kind < D_KINDS; kind++)
        for (size_t si = 0; si < N_SIZES; si++) {
            size_t n = sizes[si];
            fill(src, n, kind);
            for (int level = -1; level <= 9; level += (n > 40000 ? 3 : 1)) {
                uLongf ol = cap, ll = cap;
                memset(o, 0xcc, cap / 4); memset(l, 0xcc, cap / 4);
                const uint64_t ao[5] = { P(o), P(&ol), P(src), n, (uint64_t)(int64_t)level };
                const uint64_t al[5] = { P(l), P(&ll), P(src), n, (uint64_t)(int64_t)level };
                int ro = (int)orig("compress2", ao, 5), rl = (int)lifted("compress2", al, 5);
                dgv((uint64_t)rl); dgv(ll); dg(l, ll < cap ? ll : 0);
                snprintf(what, sizeof what, "compress2(%s, %zu bytes, level %d): ret %d/%d, len %lu/%lu",
                         kind_name[kind], n, level, rl, ro, (unsigned long)ll, (unsigned long)ol);
                check(ro == rl && ol == ll && !memcmp(o, l, ol), what);
                // and back
                uLongf uo = cap, ul = cap;
                const uint64_t bo[4] = { P(u1), P(&uo), P(o), ol }, bl[4] = { P(u2), P(&ul), P(o), ol };
                int qo = (int)orig("uncompress", bo, 4), ql = (int)lifted("uncompress", bl, 4);
                dgv((uint64_t)ql); dgv(ul);
                snprintf(what, sizeof what, "uncompress(%s, %zu, level %d): ret %d/%d", kind_name[kind], n, level, ql, qo);
                check(qo == ql && qo == Z_OK && uo == n && ul == n && !memcmp(u1, src, n) && !memcmp(u2, src, n), what);
            }
        }
    free(src); free(o); free(l); free(u1); free(u2);
}

// ---- streaming deflate / inflate ---------------------------------------------------------------------

typedef uint64_t (*Caller)(const char *, const uint64_t *, int);

// deflate src in chunks; returns output length (or -1) and fills out. Flush mode varies per chunk.
static long stream_deflate(Caller call, const uint8_t *src, size_t n, uint8_t *out, size_t cap,
                           int level, int wbits, int mem, int strategy, unsigned chunk, int *err) {
    z_stream z;
    memset(&z, 0, sizeof z);
    const uint64_t init[8] = { P(&z), (uint64_t)(int64_t)level, Z_DEFLATED, (uint64_t)(int64_t)wbits,
                               (uint64_t)mem, (uint64_t)strategy, P(ZLIB_VERSION), sizeof z };
    *err = (int)call("deflateInit2_", init, 8);
    if (*err != Z_OK) return -1;
    size_t pos = 0;
    z.next_out = out; z.avail_out = (uInt)cap;
    int ret = Z_OK, i = 0;
    while (ret != Z_STREAM_END) {
        size_t take = n - pos < chunk ? n - pos : chunk;
        z.next_in = (Bytef *)src + pos; z.avail_in = (uInt)take;
        pos += take;
        int flush = pos == n ? Z_FINISH : (i % 7 == 3 ? Z_SYNC_FLUSH : i % 11 == 5 ? Z_FULL_FLUSH : Z_NO_FLUSH);
        const uint64_t a[2] = { P(&z), (uint64_t)flush };
        ret = (int)call("deflate", a, 2);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) break;
        if (++i > 100000) break;
    }
    const uint64_t e[1] = { P(&z) };
    call("deflateEnd", e, 1);
    *err = ret;
    return ret == Z_STREAM_END ? (long)z.total_out : -1;
}

// inflate with a small output window; returns total output (or the error code < 0)
static long stream_inflate(Caller call, const uint8_t *src, size_t n, uint8_t *out, size_t cap, int wbits,
                           unsigned window, uint64_t *msg) {
    z_stream z;
    memset(&z, 0, sizeof z);
    const uint64_t init[4] = { P(&z), (uint64_t)(int64_t)wbits, P(ZLIB_VERSION), sizeof z };
    int ret = (int)call("inflateInit2_", init, 4);
    if (ret != Z_OK) return -100 + ret;
    z.next_in = (Bytef *)src; z.avail_in = (uInt)n;
    size_t got = 0;
    for (int i = 0; ret == Z_OK && i < 1000000; i++) {
        z.next_out = out + got;
        z.avail_out = (uInt)(cap - got < window ? cap - got : window);
        const uint64_t a[2] = { P(&z), Z_NO_FLUSH };
        ret = (int)call("inflate", a, 2);
        got = z.total_out;
        if (ret == Z_BUF_ERROR && z.avail_in == 0) break;
    }
    *msg = P(z.msg);
    const uint64_t e[1] = { P(&z) };
    call("inflateEnd", e, 1);
    return ret == Z_STREAM_END ? (long)got : (ret < 0 ? ret : -50 - ret);
}

static void test_streams(void) {
    size_t cap = 700000;
    uint8_t *src = malloc(cap), *o = malloc(cap), *l = malloc(cap), *io = malloc(cap), *il = malloc(cap);
    char what[240];
    static const int wbits[] = { 9, 10, 12, 15, -9, -15, 15 + 16, 9 + 16 };
    for (int t = 0; t < 70; t++) {
        size_t n = t < 10 ? sizes[t] : 1000 + rnd() % 250000;
        int kind = t % D_KINDS;
        fill(src, n, kind);
        int level = (int)(rnd() % 11) - 1, wb = wbits[rnd() % 8], mem = 1 + rnd() % 9, strategy = rnd() % 5;
        unsigned chunk = 1 + rnd() % (t & 1 ? 97 : 20000);
        int eo, el;
        long no = stream_deflate(orig, src, n, o, cap, level, wb, mem, strategy, chunk, &eo);
        long nl = stream_deflate(lifted, src, n, l, cap, level, wb, mem, strategy, chunk, &el);
        dgv((uint64_t)nl); dgv((uint64_t)el); if (nl > 0) dg(l, (size_t)nl);
        snprintf(what, sizeof what, "deflate(%s %zu B, level %d, wbits %d, mem %d, strategy %d, chunk %u): %ld/%ld bytes, ret %d/%d",
                 kind_name[kind], n, level, wb, mem, strategy, chunk, nl, no, el, eo);
        check(no == nl && eo == el && (no < 0 || !memcmp(o, l, (size_t)no)), what);
        if (no < 0) continue;
        int iwb = wb < 0 ? wb : (wb > 15 ? 15 + 32 : 15);  // 15+32: detect zlib/gzip header
        unsigned window = 1 + rnd() % (t & 2 ? 31 : 70000);
        for (int corrupt = 0; corrupt < 2; corrupt++) {
            if (corrupt && no > 4) { size_t at = 2 + rnd() % (size_t)(no - 2); o[at] ^= (uint8_t)(1 + rnd() % 255); }
            uint64_t mo, ml;
            long ro = stream_inflate(orig, o, (size_t)no, io, cap, iwb, window, &mo);
            long rl = stream_inflate(lifted, o, (size_t)no, il, cap, iwb, window, &ml);
            dgv((uint64_t)rl); if (rl > 0) dg(il, (size_t)rl);
            if (ml) dg((const char *)(uintptr_t)ml, strlen((const char *)(uintptr_t)ml));  // text, not address
            size_t cmp = ro > 0 ? (size_t)ro : 0;
            snprintf(what, sizeof what, "inflate(%s %zu B, window %u%s): %ld/%ld, msg %s/%s", kind_name[kind], n, window,
                     corrupt ? ", corrupted" : "", rl, ro, ml ? (char *)(uintptr_t)ml : "-", mo ? (char *)(uintptr_t)mo : "-");
            check(ro == rl && mo == ml && !memcmp(io, il, cmp) && (corrupt || (ro == (long)n && !memcmp(io, src, n))), what);
        }
    }
    free(src); free(o); free(l); free(io); free(il);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: zlib_test LIBZ.so [-q]\n"); return 2; }
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-q")) quiet = 1;
        if (!strcmp(argv[i], "-d")) lifted_only = 1;
    }
    recomp_load(argv[1]);
    const uint64_t none[1] = { 0 };
    printf("zlibVersion: original %s, recompiled %s\n", (char *)(uintptr_t)orig("zlibVersion", none, 0),
           (char *)(uintptr_t)lifted("zlibVersion", none, 0));
    test_checksums();
    printf("checksums: %d checks, %d failed\n", checks, failures);
    test_compress();
    printf("compress2/uncompress: %d checks, %d failed\n", checks, failures);
    test_streams();
    printf("streams: %d checks total, %d failed; import calls %" PRIu64 ", dispatch misses %" PRIu64 "\n",
           checks, failures, (uint64_t)recomp_import_calls, (uint64_t)recomp_dispatch_misses);
    printf("digest: %016" PRIx64 "%s\n", digest, lifted_only ? " (recompiled code only)" : "");
    return failures ? 1 : 0;
}
