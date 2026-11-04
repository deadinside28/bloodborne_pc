// SPDX-License-Identifier: GPL-2.0-or-later
// Address-space probe for a target device: can the game's guest ranges be mapped at their own
// addresses? (docs/ANDROID_ARM64_PORT_PLAN.md §1d: guest memory is [0x1000000000, 0xfc00000000)
// and host-owned guest-visible memory starts at 0x0800000000; PS4 code keeps pointers in 40-bit
// fields, so the game needs identity mapping below 2^40.)
#define _GNU_SOURCE
#include <inttypes.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static int try_map(uint64_t addr, uint64_t size) {
    void *p = mmap((void *)(uintptr_t)addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return 0;
    int ok = (uint64_t)(uintptr_t)p == addr;
    munmap(p, size);
    return ok;
}

int main(void) {
    printf("page size %ld\n", sysconf(_SC_PAGESIZE));
    // highest mappable 1 GiB-aligned address: user address space size
    int bits = 0;
    for (int b = 36; b <= 52; b++)
        if (try_map((1ull << b) - (1ull << 30), 1ull << 20)) bits = b;
    printf("user address space: %d bits (top 0x%" PRIx64 ")\n", bits, bits ? 1ull << bits : 0);
    static const struct { const char *what; uint64_t addr, size; } r[] = {
        { "guest low region start 0x0800000000 (image, stacks)", 0x0800000000ull, 1ull << 30 },
        { "guest user memory start 0x1000000000", 0x1000000000ull, 1ull << 30 },
        { "0x7f00000000 (just under 512 GiB)", 0x7f00000000ull, 1ull << 28 },
        { "0x8000000000 (512 GiB, 2^39)", 0x8000000000ull, 1ull << 28 },
        { "0xfb00000000 (end of guest user memory)", 0xfb00000000ull, 1ull << 28 },
    };
    for (unsigned i = 0; i < sizeof r / sizeof r[0]; i++)
        printf("  %-52s %s\n", r[i].what, try_map(r[i].addr, r[i].size) ? "ok" : "NOT mappable");
    // largest contiguous reservation from the guest user start (the game reserves the whole range)
    uint64_t lo = 0, hi = 0xfc00000000ull - 0x1000000000ull;
    while (hi - lo > (1ull << 30)) {
        uint64_t mid = (lo + hi) / 2 & ~((1ull << 30) - 1);
        if (try_map(0x1000000000ull, mid)) lo = mid; else hi = mid;
    }
    printf("largest reservation at 0x1000000000: %" PRIu64 " GiB (the full guest range is %" PRIu64 " GiB)\n",
           lo >> 30, (0xfc00000000ull - 0x1000000000ull) >> 30);
    return 0;
}
