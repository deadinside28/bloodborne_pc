// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the CPU's time stamp counter, read in order with the code around it.
#pragma once

#if !defined(__aarch64__)
#include <x86intrin.h>
#endif

#include "common/types.h"

namespace Common {

/// The time stamp counter (AArch64: the virtual counter, CNTVCT_EL0, whose rate is CNTFRQ_EL0;
/// the runtime's TSC frequency has to report that rate there).
inline u64 FencedRDTSC() {
#if defined(__aarch64__)
    u64 tsc;
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0\n\tisb" : "=r"(tsc) :: "memory");
    return tsc;
#else
    _mm_lfence();
    const u64 tsc = __rdtsc();
    _mm_lfence();
    return tsc;
#endif
}

} // namespace Common
