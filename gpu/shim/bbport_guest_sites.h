// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: guest sites, every place the port changes the game's code to run its own, described so
// that the recompiled game (recompiler/, on AArch64 none of the port's x86 stubs can run) does the
// same: at `address`, before the instruction there, `handler(tag, registers)` runs with the guest's
// registers (Core::GuestRegisterSnapshot, which it may change) and returns where the game goes on:
// 0 for `address` itself (the original instructions run, as after a hook that only observes), or
// any guest address (a handler that replaces instructions returns `address + length` after doing
// their work; one that returns from the game's function pops the return address and returns it).
//
// The native port keeps its own stubs (the fast path on x86-64); a site's handler must do what its
// stub does (hybrid execution, BB_AOT, runs the handlers on a PC: recompiler/README.md).
//
// The recompiler reads the sites from BB_AOT_DUMP's `<dump>.sites` (address, length, original
// bytes, name); the runtime (src/runtime_aot.c, __recomp_guest_site) finds a site's handler here.
// A site registered in one configuration and not another is fine: no handler, nothing runs.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define _Alignas alignas
extern "C" {
#endif

#define BB_GUEST_SITE_MAX_BYTES 32

// The registers a handler gets: Core::GuestRegisterSnapshot's layout (bbport_guest_code_hooks.h;
// checked in bbport_guest_sites.cpp), for C.
typedef struct BbGuestRegisters {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp, r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rflags;
    uint32_t mxcsr;
    _Alignas(16) uint8_t ymm[16][32];
} BbGuestRegisters;

typedef uint64_t (*BbGuestSiteHandler)(uint64_t tag, void *registers) __attribute__((sysv_abi));

typedef struct BbGuestSite {
    uint64_t address;  // where the handler runs (absolute: the game's image at its runtime base)
    uint64_t patch;    // the original instructions the native patch covers: [patch, patch + length)
    uint32_t length;   //   (address == patch, or address == patch + length: an "after" site)
    uint8_t original[BB_GUEST_SITE_MAX_BYTES];
    BbGuestSiteHandler handler;        // NULL: a variant (below)
    uint8_t replacement[BB_GUEST_SITE_MAX_BYTES];
    uint64_t tag;
    const char *name;
} BbGuestSite;

// Records a site (the original bytes as they are now: call it before patching). Returns 0, or -1
// when the table is full, the span too long, or the address taken by another site.
int bbport_guest_site_register(uint64_t address, const uint8_t *original, uint32_t length,
                               BbGuestSiteHandler handler, uint64_t tag, const char *name);
// The same with the handler after the original instructions at `address` (a stub that runs
// them first, then the port's code): it runs at address + length and returns 0, or where to go.
int bbport_guest_site_register_after(uint64_t address, const uint8_t *original, uint32_t length,
                                     BbGuestSiteHandler handler, uint64_t tag, const char *name);
// A variant: a byte patch the port writes into the game's code, maybe turned on and off while the
// game runs (BBSC's). The recompiler lifts both the original and the patched code there; the
// recompiled code takes the one the game's code holds now (the bytes are the switch).
int bbport_guest_variant_register(uint64_t address, const uint8_t *original, const uint8_t *replacement,
                                  uint32_t length, const char *name);
// The sites so far (the table only grows; entries never move): the count, and the first entry.
size_t bbport_guest_sites(const BbGuestSite **sites);
// The site at `address`, or NULL.
const BbGuestSite *bbport_guest_site_at(uint64_t address);

// A call of the game's own function at `address` (absolute) from a handler or other port code
// (System V: six integer registers, eight vector registers, then the stack). Inside a site's handler in
// recompiled code the runtime makes it (bbport_guest_call_hook: the function as recompiled code
// calls it, on the guest's stack); elsewhere it is the native call. `original`, when set, is a
// function replaced at its entry (a site there): the call runs the game's own function, not the
// site's handler (natively: `original`, the moved prologue's trampoline).
#define BB_GUEST_CALL_STACK 8
typedef struct BbGuestCallArgs {
    uint64_t address;
    uint64_t original;
    uint64_t args[6];                      // rdi rsi rdx rcx r8 r9
    uint64_t xmm[8];                       // xmm0-7's low 64 bits (a float in the low 32)
    uint64_t stack[BB_GUEST_CALL_STACK];   // the arguments past those, in order
    uint64_t xmm0_result;                  // out: xmm0's low 64 bits after the call
} BbGuestCallArgs;
typedef uint64_t (*BbGuestCallHook)(BbGuestCallArgs *call);
extern BbGuestCallHook bbport_guest_call_hook;
// Returns rax; call->xmm0_result: xmm0 (a float or double result).
uint64_t bbport_guest_call_args(BbGuestCallArgs *call);
uint64_t bbport_guest_call(uint64_t address, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e,
                           uint64_t f);

#ifdef __cplusplus
}

#include <type_traits>

/// A game function called through bbport_guest_call_args with its C++ type (integers, pointers,
/// floats and doubles; no structs by value): BbGuestFunction<R(*)(A...)>{address}(args...).
template <typename F>
struct BbGuestFunction;
template <typename R, typename... A>
struct BbGuestFunction<R (*)(A...)> {
    uint64_t address;
    struct Slots {
        BbGuestCallArgs call{};
        unsigned ints = 0, vecs = 0, stack = 0;
        void Stack(uint64_t w) {
            if (stack == BB_GUEST_CALL_STACK) __builtin_trap();  // more than the call block holds
            call.stack[stack++] = w;
        }
        template <typename T>
        void Put(T v) {
            static_assert(std::is_scalar_v<T>, "no structs by value");
            if constexpr (std::is_floating_point_v<T>) {
                uint64_t w = 0;
                __builtin_memcpy(&w, &v, sizeof(v));
                if (vecs < 8) call.xmm[vecs++] = w; else Stack(w);
            } else {
                uint64_t w;
                if constexpr (std::is_pointer_v<T> || std::is_null_pointer_v<T>) w = reinterpret_cast<uint64_t>(v);
                else w = static_cast<uint64_t>(v);
                if (ints < 6) call.args[ints++] = w; else Stack(w);
            }
        }
    };
    R operator()(A... args) const {
        Slots s;
        s.call.address = address;
        (s.Put(args), ...);
        const uint64_t r = bbport_guest_call_args(&s.call);
        if constexpr (std::is_void_v<R>) {
            return;
        } else if constexpr (std::is_floating_point_v<R>) {
            R v;
            __builtin_memcpy(&v, &s.call.xmm0_result, sizeof(v));
            return v;
        } else if constexpr (std::is_pointer_v<R>) {
            return reinterpret_cast<R>(r);
        } else if constexpr (std::is_same_v<R, bool>) {
            return (r & 0xff) != 0;
        } else {
            return static_cast<R>(r);
        }
    }
};
/// BbGuestFunction for a function pointer type and an address (an integer or a pointer): what
/// `reinterpret_cast<F>(address)(...)` was for the game's functions.
template <typename F, typename T>
BbGuestFunction<F> BbGameFn(T address) {
    if constexpr (std::is_pointer_v<T>) return BbGuestFunction<F>{reinterpret_cast<uint64_t>(address)};
    else return BbGuestFunction<F>{static_cast<uint64_t>(address)};
}
#endif
