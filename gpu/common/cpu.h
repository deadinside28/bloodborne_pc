// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the CPU's cycle counter and spin-wait hint, for x86-64 and AArch64. The counter's rate
// is the runtime's TSC frequency (runtime_tsc_frequency: on AArch64 it has to report CNTFRQ_EL0,
// the rate of CNTVCT_EL0); amounts of time go through TscTicks, never fixed cycle counts.
#pragma once

#include <cstdint>

#include "common/types.h"

#if !defined(__aarch64__)
#include <x86intrin.h>
#endif

extern "C" std::uint64_t runtime_tsc_frequency(void);

namespace Common {

/// The cycle counter (unordered with the code around it; Common::FencedRDTSC orders it).
inline u64 Tsc() {
#if defined(__aarch64__)
    u64 v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return __rdtsc();
#endif
}

/// Inside a spin-wait loop.
inline void CpuPause() {
#if defined(__aarch64__)
    __asm__ volatile("yield");
#else
    __builtin_ia32_pause();
#endif
}

/// The counter's ticks per second.
inline double TscFrequency() {
    static const double frequency = double(runtime_tsc_frequency());
    return frequency;
}

/// Counter ticks in `seconds`.
inline u64 TscTicks(double seconds) {
    return u64(seconds * TscFrequency());
}

} // namespace Common
