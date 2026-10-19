// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the CPU's time stamp counter, read in order with the code around it.
#pragma once

#include <x86intrin.h>

#include "common/types.h"

namespace Common {

inline u64 FencedRDTSC() {
    _mm_lfence();
    const u64 tsc = __rdtsc();
    _mm_lfence();
    return tsc;
}

} // namespace Common
