// SPDX-License-Identifier: GPL-2.0-or-later
// x87: long double, which x86-64 compilers keep on the x87 stack (as the game's libc does for its
// long double functions): arithmetic, compares (fucomi, fcmov), conversions (fild, fistp /
// fisttp), fsqrt and fabs, 80-bit stores, and long doubles passed between functions (in memory)
// and returned in st(0). txf_* (signature as fp_simd.c's tf_*): checked on x86-64 hosts only,
// where the runtime runs x87 instructions as themselves (recomp_x87.h); recorded everywhere.
#include <stdint.h>

#define TXF(name) __attribute__((noinline, used, visibility("default"))) double txf_##name( \
    double x, double y, uint64_t a, uint8_t *buf)
#define NI static __attribute__((noinline))

NI long double ld_mix(long double p, long double q) { return p * q + p / (q == 0 ? 3 : q) - q; }
NI long double ld_poly(long double t) { return ((t * 0.5L + 1.25L) * t - 3.0L) * t + 0.125L; }

TXF(arith) {
    long double p = x, q = y;
    long double r = ld_mix(p, q) * 1.0000001L + ld_poly(p - q);
    return (double)r;
}

TXF(compare) {
    long double p = x, q = y;
    double r = (p < q) + 2 * (p == q) + 4 * (p > q) + 8 * (p != p);
    return r + (double)(p <= q ? p : q);
}

TXF(convert) {
    long double p = x;
    const int64_t i = (int64_t)(a & 0xffffff) - 0x800000;
    long double r = p + (long double)i;
    const int ok = r == r && r < 1e18L && r > -1e18L;
    ((int64_t *)buf)[0] = ok ? (int64_t)r : 0;
    ((int32_t *)buf)[3] = ok && r < 2e9L && r > -2e9L ? (int32_t)r : 0;
    return (double)(r / 3.0L);
}

TXF(sqrt_abs) {
    long double p = x;
    return (double)__builtin_sqrtl(__builtin_fabsl(p)) - (double)(-p * 0.75L);
}

TXF(store80) {
    long double v = (long double)x * (long double)y + (long double)(a & 1023);
    *(long double *)(buf + 16) = v;   // fstp m80: 10 bytes (the padding stays as it was)
    long double w = *(long double *)(buf + 16) / 7.0L;
    return (double)w;
}
