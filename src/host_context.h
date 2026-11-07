/* SPDX-FileCopyrightText: Copyright 2026 bbport contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * The registers a signal handler reports, on x86-64 and AArch64 Linux: the faulting pc, the
 * stack and frame pointers, the first argument register. */
#ifndef BB_HOST_CONTEXT_H
#define BB_HOST_CONTEXT_H
#include <stdint.h>
#include <ucontext.h>

static inline uintptr_t host_context_pc(const ucontext_t *uc) {
#if defined(__aarch64__)
    return (uintptr_t)uc->uc_mcontext.pc;
#else
    return (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#endif
}
static inline uintptr_t host_context_sp(const ucontext_t *uc) {
#if defined(__aarch64__)
    return (uintptr_t)uc->uc_mcontext.sp;
#else
    return (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
#endif
}
static inline uintptr_t host_context_fp(const ucontext_t *uc) {
#if defined(__aarch64__)
    return (uintptr_t)uc->uc_mcontext.regs[29];
#else
    return (uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
#endif
}
static inline uintptr_t host_context_arg0(const ucontext_t *uc) {
#if defined(__aarch64__)
    return (uintptr_t)uc->uc_mcontext.regs[0];
#else
    return (uintptr_t)uc->uc_mcontext.gregs[REG_RDI];
#endif
}
#endif
