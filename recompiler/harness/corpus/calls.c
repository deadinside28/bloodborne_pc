// SPDX-License-Identifier: GPL-2.0-or-later
// Calls across function boundaries, for the hybrid tests (runner -H), where any function may be
// original code or recompiled code and every call between them crosses the thunks: deep mutual
// recursion, wide vector arguments and results, floating point arguments on the stack, large
// struct results, callbacks and values kept in callee-saved registers across calls.
// Signatures as in int_basic.c (t_*) and fp_simd.c (tf_*).
#include <stdint.h>

#define T(name) __attribute__((noinline, used, visibility("default"))) uint64_t t_##name( \
    uint64_t a, uint64_t b, uint64_t c, uint8_t *buf)
#define TF(name) __attribute__((noinline, used, visibility("default"))) double tf_##name( \
    double x, double y, uint64_t a, uint8_t *buf)
#define NI static __attribute__((noinline))

// Mutual recursion: neighbouring functions in address order, so a split by index parity puts
// them on different sides; depth up to 300 crossings.
NI uint64_t ping(uint64_t n, uint64_t acc);
NI uint64_t pong(uint64_t n, uint64_t acc) { return n ? ping(n - 1, acc * 31 + n) ^ (acc >> 3) : acc; }
NI uint64_t ping(uint64_t n, uint64_t acc) { return n ? pong(n - 1, acc + (n << 7)) + 1 : acc ^ 0x55; }
T(pingpong) { return ping((a & 255) + 45, b) + c; }

// Values that live across calls in callee-saved registers on both sides.
NI uint64_t mix3(uint64_t x, uint64_t y, uint64_t z) { return (x * 0x9e3779b97f4a7c15ull) ^ (y + z) ^ (z << 17); }
T(callee_saved) {
    uint64_t r1 = a ^ 1, r2 = b ^ 2, r3 = c ^ 3, r4 = a + b, r5 = b + c, r6 = a * c;
    for (int i = 0; i < 8; i++) {
        uint64_t t = mix3(r1, r2, r3);
        r1 += t; r2 ^= mix3(r4, r5, t); r3 -= mix3(r6, t, r1);
        r4 = r4 * 3 + r1; r5 ^= r2; r6 += r3;
    }
    buf[0] = (uint8_t)r4;
    return r1 ^ r2 ^ r3 ^ r4 ^ r5 ^ r6;
}

// Floating point arguments beyond xmm0-7 go on the stack, mixed with integer stack arguments.
NI double fsum10(double d0, double d1, double d2, double d3, double d4, double d5, double d6,
                 double d7, double d8, double d9) {
    return d0 + 2 * d1 - 3 * d2 + 4 * d3 - 5 * d4 + 6 * d5 - 7 * d6 + 8 * d7 - 9 * d8 + 10 * d9;
}
NI double mixed_args(uint64_t i0, double d0, uint64_t i1, float f1, uint64_t i2, uint64_t i3,
                     uint64_t i4, uint64_t i5, uint64_t i6, double d1, uint64_t i7, double d2,
                     double d3, double d4, double d5, double d6, double d7, double d8, float f9) {
    return (double)(int64_t)(i0 + i1 * 3 + i2 * 5 + i3 * 7 + i4 * 11 + i5 * 13 + i6 * 17 + i7 * 19)
           + d0 * f1 - d1 + d2 * d3 - d4 / (d5 == 0 ? 1 : d5) + d6 - d7 * d8 + f9;
}
TF(float_stack_args) {
    double s = fsum10(x, y, x * y, x - y, 1.5, -2.25, x * 0.5, y * 4, (double)(a & 1023), x + 1);
    return s + mixed_args(a, x, a >> 3, (float)y, a * 5, 7, a ^ 9, 11, a + 13, y * 2, 17, x, y, 3, 5, x, y, 7, (float)x);
}

// Wide vectors through calls: with AVX (the Jaguar builds) in ymm registers; without, in memory.
typedef double v4d __attribute__((vector_size(32)));
typedef float v8f __attribute__((vector_size(32)));
NI v4d v4d_madd(v4d p, v4d q, v4d r) { return p * q + r; }
NI v8f v8f_blend(v8f p, v8f q, float k) { return p * k - q + (v8f){ 1, 2, 3, 4, 5, 6, 7, 8 }; }
TF(wide_vectors) {
    v4d p = { x, y, x * y, x - y }, q = { y, 2.0, -x, 0.25 }, r = { 1, (double)(a & 15), y, x };
    for (int i = 0; i < 4; i++) p = v4d_madd(p, q, r);
    v8f f = { (float)x, (float)y, 1, 2, 3, (float)(a & 7), -1, 0.5f }, g = { 2, 2, 2, 2, (float)y, 1, 1, 1 };
    for (int i = 0; i < 3; i++) f = v8f_blend(f, g, 0.75f);
    for (int i = 0; i < 4; i++) ((double *)buf)[i] = p[i];
    for (int i = 0; i < 8; i++) ((float *)(buf + 64))[i] = f[i];
    return p[0] + p[1] + p[2] + p[3] + f[0] + f[7];
}

// A struct returned through the hidden result pointer, filled by the callee.
typedef struct { uint64_t w[9]; double d; } Big;
NI Big make_big(uint64_t s, double d) {
    Big r;
    for (int i = 0; i < 9; i++) r.w[i] = s * (uint64_t)(i + 1) ^ (s >> i);
    r.d = d * 3;
    return r;
}
T(struct_return) {
    Big p = make_big(a, (double)(int64_t)b), q = make_big(c, 1.5);
    uint64_t s = 0;
    for (int i = 0; i < 9; i++) { s = s * 3 + p.w[i] - q.w[i]; buf[i] = (uint8_t)p.w[i]; }
    return s + (uint64_t)(int64_t)(p.d + q.d);
}

// Callbacks: a function given function pointers calls back into its caller's neighbours.
NI uint64_t cb_add(uint64_t v, uint64_t k) { return v + k; }
NI uint64_t cb_rot(uint64_t v, uint64_t k) { return v << (k & 63) | v >> ((64 - (k & 63)) & 63); }
NI uint64_t cb_mul(uint64_t v, uint64_t k) { return v * (k | 1); }
NI uint64_t apply_all(uint64_t (*const *fs)(uint64_t, uint64_t), int n, uint64_t v, uint64_t k) {
    for (int i = 0; i < n; i++) v = fs[i](v, k + (uint64_t)i);
    return v;
}
T(callbacks) {
    uint64_t (*const fs[6])(uint64_t, uint64_t) = { cb_add, cb_rot, cb_mul, cb_rot, cb_add, cb_mul };
    return apply_all(fs, 6, a, b) ^ apply_all(fs + (c & 3), 3, b, a);
}

// Deep plain recursion through a function that calls itself and a neighbour.
NI uint64_t walk_leaf(uint64_t v) { return v * 5 + 1; }
NI uint64_t walk(uint64_t n, uint64_t v) { return n ? walk(n - 1, walk_leaf(v) ^ n) : v; }
T(deep_walk) { return walk((a & 1023) + 200, b); }
