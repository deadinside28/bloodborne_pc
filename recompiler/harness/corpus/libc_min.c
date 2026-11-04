// SPDX-License-Identifier: GPL-2.0-or-later
// The two libc functions compilers emit calls to on their own. Hidden: direct calls, no PLT.
#include <stddef.h>
__attribute__((visibility("hidden"))) void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
__attribute__((visibility("hidden"))) void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
__attribute__((visibility("hidden"))) float roundf(float x) {
    if (!(__builtin_fabsf(x) < 8388608.0f)) return x;  // NaN, inf, or already an integer
    float t = (float)(int)x;                            // truncation without truncf
    if (__builtin_fabsf(x - t) >= 0.5f) t += __builtin_copysignf(1.0f, x);
    return t;
}
// libgcc helpers GCC calls when the target has no popcnt (the -march=x86-64 variant).
__attribute__((visibility("hidden"))) int __popcountdi2(unsigned long x) { int n = 0; while (x) { x &= x - 1; n++; } return n; }
