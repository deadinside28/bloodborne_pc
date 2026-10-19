// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: what a SIGSEGV handler needs from its ucontext (x86-64 Linux).
#pragma once

#include <sys/ucontext.h>

#include "common/types.h"

namespace Common {

/// The faulting instruction.
inline void* GetRip(void* context) {
    return reinterpret_cast<void*>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
}

/// The fault was a write (page fault error code bit 1).
inline bool IsWriteError(void* context) {
    return static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_ERR] & 0x2;
}

} // namespace Common
