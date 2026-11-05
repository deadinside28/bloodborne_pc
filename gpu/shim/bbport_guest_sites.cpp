// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the guest sites' table (bbport_guest_sites.h). Append-only: readers (the recompiled
// code's __recomp_guest_site, on every pass over a site) take no lock; a writer publishes an entry
// by the count after filling it.

#include "bbport_guest_sites.h"
#include "bbport_guest_code_hooks.h"

#include <cstddef>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

static_assert(sizeof(BbGuestRegisters) == sizeof(Core::GuestRegisterSnapshot));
static_assert(offsetof(BbGuestRegisters, rsp) == offsetof(Core::GuestRegisterSnapshot, rsp));
static_assert(offsetof(BbGuestRegisters, r15) == offsetof(Core::GuestRegisterSnapshot, r15));
static_assert(offsetof(BbGuestRegisters, rflags) == offsetof(Core::GuestRegisterSnapshot, rflags));
static_assert(offsetof(BbGuestRegisters, mxcsr) == offsetof(Core::GuestRegisterSnapshot, mxcsr));
static_assert(offsetof(BbGuestRegisters, ymm) == offsetof(Core::GuestRegisterSnapshot, ymm));

namespace {

constexpr std::size_t MaxSites = 1024;
BbGuestSite sites[MaxSites];
std::atomic<std::size_t> count{0};
std::mutex writer;

// address -> index + 1, open addressing (at most half full); 0: empty.
constexpr std::size_t Slots = 2 * MaxSites;
std::atomic<std::uint32_t> slots[Slots];

std::size_t SlotOf(std::uint64_t address) {
    return std::size_t((address * 0x9e3779b97f4a7c15ull) >> 53) & (Slots - 1);
}

} // namespace

namespace {
int Register(std::uint64_t address, std::uint64_t patch, const std::uint8_t* original, std::uint32_t length,
             BbGuestSiteHandler handler, std::uint64_t tag, const char* name);
}

extern "C" __attribute__((visibility("default"))) int bbport_guest_site_register(
    std::uint64_t address, const std::uint8_t* original, std::uint32_t length,
    BbGuestSiteHandler handler, std::uint64_t tag, const char* name) {
    return Register(address, address, original, length, handler, tag, name);
}

extern "C" __attribute__((visibility("default"))) int bbport_guest_site_register_after(
    std::uint64_t address, const std::uint8_t* original, std::uint32_t length,
    BbGuestSiteHandler handler, std::uint64_t tag, const char* name) {
    return Register(address + length, address, original, length, handler, tag, name);
}

namespace {
int Register(std::uint64_t address, std::uint64_t patch, const std::uint8_t* original, std::uint32_t length,
             BbGuestSiteHandler handler, std::uint64_t tag, const char* name) {
    if (!handler || !original || length == 0 || length > BB_GUEST_SITE_MAX_BYTES) {
        return -1;
    }
    std::scoped_lock lock{writer};
    const std::size_t n = count.load(std::memory_order_relaxed);
    if (n == MaxSites || bbport_guest_site_at(address)) {
        std::fprintf(stderr, "Guest site %s at %#llx: %s\n", name ? name : "?",
                     static_cast<unsigned long long>(address), n == MaxSites ? "table full" : "taken");
        return -1;
    }
    BbGuestSite& s = sites[n];
    s.address = address;
    s.patch = patch;
    s.length = length;
    std::memcpy(s.original, original, length);
    s.handler = handler;
    s.tag = tag;
    s.name = name;
    std::size_t slot = SlotOf(address);
    while (slots[slot].load(std::memory_order_relaxed)) {
        slot = (slot + 1) & (Slots - 1);
    }
    count.store(n + 1, std::memory_order_release);
    slots[slot].store(std::uint32_t(n + 1), std::memory_order_release);
    return 0;
}
} // namespace

extern "C" __attribute__((visibility("default"))) std::size_t bbport_guest_sites(const BbGuestSite** out) {
    if (out) {
        *out = sites;
    }
    return count.load(std::memory_order_acquire);
}

extern "C" __attribute__((visibility("default"))) const BbGuestSite* bbport_guest_site_at(std::uint64_t address) {
    for (std::size_t slot = SlotOf(address);; slot = (slot + 1) & (Slots - 1)) {
        const std::uint32_t i = slots[slot].load(std::memory_order_acquire);
        if (!i) {
            return nullptr;
        }
        if (sites[i - 1].address == address) {
            return &sites[i - 1];
        }
    }
}

extern "C" __attribute__((visibility("default"))) BbGuestCallHook bbport_guest_call_hook = nullptr;

extern "C" __attribute__((visibility("default"))) std::uint64_t bbport_guest_call_args(const BbGuestCallArgs* call) {
    if (bbport_guest_call_hook) {
        return bbport_guest_call_hook(call);
    }
#if defined(__x86_64__)
    using Fn = std::uint64_t(__attribute__((sysv_abi))*)(std::uint64_t, std::uint64_t, std::uint64_t,
                                                         std::uint64_t, std::uint64_t, std::uint64_t, double);
    const auto* a = call->args;
    return reinterpret_cast<Fn>(call->original ? call->original : call->address)(a[0], a[1], a[2], a[3], a[4], a[5],
                                                                                  call->xmm0);
#else
    std::fprintf(stderr, "Guest call %#llx: no way to call the game's code here\n",
                 static_cast<unsigned long long>(call->address));
    std::abort();
#endif
}

extern "C" __attribute__((visibility("default"))) std::uint64_t bbport_guest_call(
    std::uint64_t address, std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d, std::uint64_t e,
    std::uint64_t f) {
    const BbGuestCallArgs call{address, 0, {a, b, c, d, e, f}, 0.0};
    return bbport_guest_call_args(&call);
}
