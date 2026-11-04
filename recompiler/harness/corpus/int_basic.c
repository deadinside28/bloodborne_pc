// SPDX-License-Identifier: GPL-2.0-or-later
// Differential-test corpus: every exported t_* function has the signature
//   uint64_t t_x(uint64_t a, uint64_t b, uint64_t c, uint8_t *buf)   (buf: 256 bytes)
// The runner calls the original x86-64 code and the recompiled code with the same inputs and
// compares the return value and buf. Built freestanding: no libc, no PLT.
#include <stdint.h>
#include <stddef.h>

#define T(name) __attribute__((noinline, used, visibility("default"))) uint64_t t_##name( \
    uint64_t a, uint64_t b, uint64_t c, uint8_t *buf)

T(add) { return a + b + c; }
T(sub_mix) { return (a - b) ^ (c << 3) ^ (b >> 7); }
T(mul32) { return (uint32_t)a * (uint32_t)b + (uint32_t)c; }
T(mul64_hi) { return (uint64_t)(((unsigned __int128)a * b) >> 64); }
T(smul64_hi) { return (uint64_t)(((__int128)(int64_t)a * (int64_t)b) >> 64); }
T(udiv) { return b ? a / b + a % b : 0; }
T(sdiv32) { int32_t x = (int32_t)a, y = (int32_t)b; if (!y || (x == INT32_MIN && y == -1)) return 1; return (uint64_t)(int64_t)(x / y) ^ (uint64_t)(x % y); }
T(sdiv64) { int64_t x = (int64_t)a, y = (int64_t)b | 1; if (y == -1) return 7; return (uint64_t)(x / y) + (uint64_t)(x % y); }
T(div_const) { return a / 7 + (uint32_t)b / 13 + (int64_t)c / -9; }
T(shifts) { unsigned s = c & 63; return (a << s) ^ (b >> s) ^ (uint64_t)((int64_t)a >> s); }
T(shifts32) { unsigned s = c & 31; uint32_t x = (uint32_t)a; return (x << s) ^ (x >> s) ^ (uint32_t)((int32_t)b >> s); }
T(rot) { unsigned s = c & 63; return ((a << s) | (a >> ((64 - s) & 63))) + ((uint32_t)b >> 5 | (uint32_t)b << 27); }
T(bytes) { uint8_t x = (uint8_t)a, y = (uint8_t)b; return (uint8_t)(x + y) | (uint64_t)(uint8_t)(x * y) << 8 | (uint64_t)(int8_t)x << 16; }
T(words) { uint16_t x = (uint16_t)a, y = (uint16_t)b; return (uint16_t)(x - y) ^ ((uint64_t)(int16_t)x << 20); }
T(cmp_signed) { int64_t x = a, y = b; return (x < y) + 2 * (x > y) + 4 * ((int32_t)a <= (int32_t)c) + 8 * (a == c); }
T(cmp_unsigned) { return (a < b) | (a >= c) << 1 | ((uint32_t)a > (uint32_t)b) << 2 | ((uint16_t)a == (uint16_t)c) << 3; }
T(minmax) { int64_t x = a, y = b, z = c; int64_t lo = x < y ? x : y; int64_t hi = x > z ? x : z; return lo * 3 + hi; }
T(abs_neg) { int64_t x = a; int32_t y = b; return (uint64_t)(x < 0 ? -x : x) + (uint32_t)(y < 0 ? -y : y) + (uint64_t)-(int64_t)c; }
T(popcnt) { return __builtin_popcountll(a) + __builtin_popcount((uint32_t)b) * 100; }
T(clz_ctz) { return (a ? __builtin_clzll(a) : 99) + 100 * (b ? __builtin_ctzll(b) : 99) + 10000 * ((uint32_t)c ? __builtin_ctz((uint32_t)c) : 99); }
T(bswap) { return __builtin_bswap64(a) ^ __builtin_bswap32((uint32_t)b) ^ __builtin_bswap16((uint16_t)c); }
T(parity) { return __builtin_parityll(a) + 2 * __builtin_parity((uint32_t)b); }
T(adc_chain) { unsigned __int128 x = ((unsigned __int128)a << 64) | b, y = ((unsigned __int128)c << 64) | a; x += y; return (uint64_t)x ^ (uint64_t)(x >> 64); }
T(sbb_chain) { unsigned __int128 x = ((unsigned __int128)a << 64) | b, y = ((unsigned __int128)b << 64) | c; x -= y; return (uint64_t)x ^ (uint64_t)(x >> 64); }
T(overflow) { int64_t r; uint64_t u; int o1 = __builtin_add_overflow((int64_t)a, (int64_t)b, &r); int o2 = __builtin_mul_overflow(a, c, &u); return r ^ u ^ o1 << 1 ^ o2; }
T(select_chain) { uint64_t r = 0; if (a & 1) r += b; else r -= c; if ((int64_t)b < 0) r ^= a; return r; }

T(loop_sum) { uint64_t s = 0; for (uint64_t i = 0; i < (a & 255); i++) s += i * b ^ c; return s; }
T(collatz) { uint64_t x = (a & 0xffff) | 1, n = 0; while (x != 1 && n < 1000) { x = x & 1 ? 3 * x + 1 : x / 2; n++; } return n; }
T(gcd) { uint64_t x = a | 1, y = b | 1; while (y) { uint64_t t = x % y; x = y; y = t; } return x; }
T(fib_iter) { uint64_t x = 0, y = 1; for (unsigned i = 0; i < (a & 90); i++) { uint64_t t = x + y; x = y; y = t; } return x; }

// memory
T(buf_fill) { for (int i = 0; i < 256; i++) buf[i] = (uint8_t)(a + i * b); return buf[c & 255]; }
T(buf_sum) { uint64_t s = 0; for (int i = 0; i < 256; i++) s = s * 31 + buf[i]; return s; }
T(buf_words) { uint32_t *w = (uint32_t *)buf; for (int i = 0; i < 64; i++) w[i] = w[i] * (uint32_t)a + (uint32_t)i; return w[b & 63]; }
T(buf_unaligned) { uint64_t v; __builtin_memcpy(&v, buf + (a & 127) + 1, 8); v += b; __builtin_memcpy(buf + (c & 127) + 3, &v, 8); return v; }
T(buf_reverse) { for (int i = 0; i < 128; i++) { uint8_t t = buf[i]; buf[i] = buf[255 - i]; buf[255 - i] = t; } return buf[0]; }
T(buf_sort) { uint8_t *p = buf; for (int i = 1; i < 64; i++) { uint8_t k = p[i]; int j = i - 1; while (j >= 0 && p[j] > k) { p[j + 1] = p[j]; j--; } p[j + 1] = k; } return p[a & 63]; }
T(strlen_like) { buf[(a & 200) + 20] = 0; uint64_t n = 0; while (buf[n]) n++; return n; }
struct S { uint64_t x[6]; uint32_t y; uint16_t z; };
T(struct_copy) { struct S *s = (struct S *)buf, *d = (struct S *)(buf + 128); *d = *s; d->x[a & 5] ^= b; d->y += (uint32_t)c; return d->x[0] + d->z; }
T(memset_like) { __builtin_memset(buf + (a & 63), (int)b, 64 + (c & 63)); return buf[100]; }

// calls, recursion, function pointers, switches
static __attribute__((noinline)) uint64_t helper(uint64_t x, uint64_t y) { return x * 3 + y; }
static __attribute__((noinline)) uint64_t helper7(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8) { return a1 + 2 * a2 + 3 * a3 + 4 * a4 + 5 * a5 + 6 * a6 + 7 * a7 + 8 * a8; }
T(call) { return helper(a, b) + helper(c, a); }
T(call_stack_args) { return helper7(a, b, c, a ^ b, b ^ c, a + c, a * 3, c - b); }
static __attribute__((noinline)) uint64_t fib_rec(uint64_t n) { return n < 2 ? n : fib_rec(n - 1) + fib_rec(n - 2); }
T(recursion) { return fib_rec(a & 15); }
static __attribute__((noinline)) uint64_t op_add(uint64_t x, uint64_t y) { return x + y; }
static __attribute__((noinline)) uint64_t op_xor(uint64_t x, uint64_t y) { return x ^ y; }
static __attribute__((noinline)) uint64_t op_mul(uint64_t x, uint64_t y) { return x * y; }
static __attribute__((noinline)) uint64_t op_shr(uint64_t x, uint64_t y) { return x >> (y & 63); }
static uint64_t (*const ops[4])(uint64_t, uint64_t) = { op_add, op_xor, op_mul, op_shr };
T(fnptr) { return ops[a & 3](b, c) + ops[(a >> 2) & 3](c, b); }
T(switch_dense) {
    switch (a & 15) {
    case 0: return b + 1; case 1: return b * c; case 2: return b ^ 0x55; case 3: return c - b;
    case 4: return b << 3; case 5: return c >> 2; case 6: return b | c; case 7: return b & c;
    case 8: return ~b; case 9: return b + c + 17; case 10: return b * 7; case 11: return c * 11;
    default: return 0x1234;
    }
}
T(switch_loop) { uint64_t s = 0; for (int i = 0; i < 16; i++) { switch ((a >> i) & 7) { case 0: s += 1; break; case 1: s *= 3; break; case 2: s ^= b; break; case 3: s -= c; break; case 4: s += i; break; case 5: s = s << 1 | 1; break; default: s += 100; } } return s; }
static __attribute__((noinline)) uint64_t tail_target(uint64_t x, uint64_t y) { return x * y + 99; }
T(tailcall) { if (a & 1) return tail_target(b, c); return tail_target(c, b + 1); }

// atomics (lock prefix paths)
T(atomic_add) { uint64_t *p = (uint64_t *)buf; uint64_t o = __atomic_fetch_add(p, a, __ATOMIC_SEQ_CST); __atomic_fetch_or(p + 1, b, __ATOMIC_SEQ_CST); __atomic_fetch_and((uint32_t *)(p + 2), (uint32_t)c, __ATOMIC_SEQ_CST); return o + *p; }
T(atomic_cas) { uint64_t *p = (uint64_t *)buf; uint64_t exp = (a & 1) ? *p : b; int ok = __atomic_compare_exchange_n(p, &exp, c, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return exp * 2 + ok; }
T(atomic_xchg) { uint64_t *p = (uint64_t *)buf; return __atomic_exchange_n(p, a, __ATOMIC_SEQ_CST) + __atomic_add_fetch((uint32_t *)(p + 1), 1, __ATOMIC_SEQ_CST); }
