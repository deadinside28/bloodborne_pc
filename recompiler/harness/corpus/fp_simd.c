// SPDX-License-Identifier: GPL-2.0-or-later
// Floating point and SIMD corpus. tf_* functions have the signature
//   double tf_x(double x, double y, uint64_t a, uint8_t *buf)   (buf: 256 bytes)
// and are compared bit for bit (return value and buf). Game-like code: vector math, matrices,
// conversions with x86's out-of-range results, loops the compiler vectorises.
#include <stdint.h>

#define TF(name) __attribute__((noinline, used, visibility("default"))) double tf_##name( \
    double x, double y, uint64_t a, uint8_t *buf)

typedef float v4 __attribute__((vector_size(16)));
typedef float v8 __attribute__((vector_size(32)));
typedef int32_t v4i __attribute__((vector_size(16)));

TF(arith) { return (x + y) * (x - y) / (y == 0 ? 3.0 : y); }
TF(arith_f) { float a1 = (float)x, b1 = (float)y; return (double)(a1 * b1 + a1 / (b1 + 1.5f) - b1); }
TF(sqrt) { return __builtin_sqrt(x < 0 ? -x : x) + (double)__builtin_sqrtf((float)(y < 0 ? -y : y)); }
TF(minmax) { double m = x < y ? x : y; double n = x > y ? x : y; float p = (float)x < (float)y ? (float)x : (float)y; return m * 3 + n + p; }
TF(cmp) { return (double)((x < y) + 2 * (x <= y) + 4 * (x == y) + 8 * (x != y) + 16 * (x > y) + 32 * __builtin_isnan(x)); }
TF(to_int) { return (double)(int64_t)x + (double)(int32_t)y + (double)(int32_t)(float)x; }
TF(to_uint) { return (double)(uint32_t)(x < 0 ? 0 : x > 4e9 ? 4e9 : x) + (double)(uint64_t)(y < 0 ? 0 : y > 1e18 ? 1e18 : y); }
TF(from_int) { return (double)(int64_t)a + (double)(float)(int32_t)a + (double)(uint64_t)a + (double)(float)(uint32_t)(a >> 7); }
#if defined(__SSE4_1__)  // without SSE4.1 these are libm calls
TF(round_modes) { return __builtin_floor(x) + 2 * __builtin_ceil(y) + 3 * __builtin_trunc(x * 0.5) + 4 * __builtin_nearbyint(y * 3) + __builtin_roundf((float)x); }
#endif
TF(fabs_neg) { return __builtin_fabs(x) - (-y) + __builtin_copysign(x, y) + (double)__builtin_fabsf((float)y); }
TF(select) { double r = 0; if (x > 0) r += y; if (y < x) r -= x; if (x != x) r = 42; return r; }
TF(poly) { double r = 0; for (int i = 0; i < 8; i++) r = r * x + (double)(i + 1) * y; return r; }

// vectors (SSE/AVX)
TF(vec4_dot) {
    v4 p = { (float)x, (float)y, 1.5f, -2.0f }, q = { (float)y, 3.0f, (float)x, 0.25f };
    v4 r = p * q + p;
    return (double)(r[0] + r[1] + r[2] + r[3]);
}
TF(vec4_buf) {
    v4 *v = (v4 *)buf;
    for (int i = 0; i < 8; i++) { v4 t = v[i]; t = t * (float)x + v[(i + 1) & 7]; v4i ok = t == t; v[i] = (v4)((v4i)t & ok); }
    return (double)v[a & 7][a >> 3 & 3];
}
TF(vec8) {
    v8 s = { 0 }, k = { (float)x, (float)y, 1, 2, 3, 4, 5, 6 };
    v8 *v = (v8 *)buf;
    for (int i = 0; i < 4; i++) { s += v[i] * k; v[i] = v[i] - k; }
    return (double)(s[0] - s[7] + s[3]);
}
TF(vec_int) {
    v4i *v = (v4i *)buf; v4i acc = { 0 };
    for (int i = 0; i < 16; i++) { acc += v[i] * (int32_t)a; acc ^= v[i] >> 3; v[i] = acc; }
    return (double)(acc[0] + acc[3]);
}
TF(matrix) {
    float *m = (float *)buf, out[16];
    for (int i = 0; i < 16; i++) { float v = m[i]; m[i] = v != v || v > 1e9f || v < -1e9f ? 1.0f : v; }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += m[r * 4 + k] * m[k * 4 + c];
            out[r * 4 + c] = s * (float)x;
        }
    for (int i = 0; i < 16; i++) m[i + 16] = out[i];
    return (double)(out[0] + out[5] + out[10] + out[15]);
}
TF(saxpy) {
    float *p = (float *)buf;
    for (int i = 0; i < 32; i++) p[i + 32] = (float)x * p[i] + p[i + 32];
    return (double)p[33 + (a & 15)];
}
TF(bytes_sum) {
    uint32_t s = 0;
    for (int i = 0; i < 256; i++) s += buf[i] * (uint32_t)(i & 7);
    return (double)s;
}
TF(clamp_u8) {
    float *f = (float *)buf;
    for (int i = 0; i < 32; i++) { float v = f[i] * (float)x; buf[128 + i] = v != v ? 0 : v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }
    return (double)buf[128 + (a & 31)];
}
TF(int_to_float_loop) {
    int32_t *p = (int32_t *)buf; float *f = (float *)(buf + 128);
    for (int i = 0; i < 32; i++) f[i] = (float)p[i] * 0.5f;
    return (double)f[a & 31];
}
TF(float_to_int_loop) {
    float *f = (float *)buf; int32_t *p = (int32_t *)(buf + 128);
    for (int i = 0; i < 32; i++) p[i] = (int32_t)(f[i] * (float)y);
    return (double)p[a & 31];
}
TF(double_vec) {
    double *d = (double *)buf;
    for (int i = 0; i < 16; i++) d[i + 16] = d[i] * x + y;
    return d[16 + (a & 15)];
}
