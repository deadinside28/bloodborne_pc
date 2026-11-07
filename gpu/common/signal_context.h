// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: what a SIGSEGV handler needs from its ucontext (x86-64 and AArch64 Linux).
#pragma once

#include <cstring>
#include <sys/ucontext.h>

#include "common/types.h"

namespace Common {

/// The faulting instruction.
inline void* GetRip(void* context) {
#if defined(__aarch64__)
    return reinterpret_cast<void*>(static_cast<ucontext_t*>(context)->uc_mcontext.pc);
#else
    return reinterpret_cast<void*>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
#endif
}

/// The stack pointer.
inline u64 GetSp(void* context) {
#if defined(__aarch64__)
    return static_cast<ucontext_t*>(context)->uc_mcontext.sp;
#else
    return u64(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RSP]);
#endif
}

/// The frame pointer (x29 / rbp).
inline u64 GetFramePointer(void* context) {
#if defined(__aarch64__)
    return static_cast<ucontext_t*>(context)->uc_mcontext.regs[29];
#else
    return u64(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RBP]);
#endif
}

/// The fault was a write.
inline bool IsWriteError(void* context) {
#if defined(__aarch64__)
    // The kernel's esr_context record in the signal frame's extension area (ESR_MAGIC): a data
    // abort (EC 0x24/0x25) whose ISS has WnR (bit 6) set was a write.
    const auto& mc = static_cast<ucontext_t*>(context)->uc_mcontext;
    const u8* at = reinterpret_cast<const u8*>(mc.__reserved);
    const u8* const end = at + sizeof(mc.__reserved);
    while (at + 8 <= end) {
        u32 magic, size;
        std::memcpy(&magic, at, 4);
        std::memcpy(&size, at + 4, 4);
        if (magic == 0 || size < 8) {
            break;
        }
        if (magic == 0x45535201 && size >= 16) {
            u64 esr;
            std::memcpy(&esr, at + 8, 8);
            const u64 ec = esr >> 26;
            return (ec == 0x24 || ec == 0x25) && (esr & (1u << 6));
        }
        at += size;
    }
    return false;
#else
    return static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_ERR] & 0x2; // error code bit 1
#endif
}

} // namespace Common
