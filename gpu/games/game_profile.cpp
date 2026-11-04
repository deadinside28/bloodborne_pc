// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "game_profile.h"
#include "shim/bb_env.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/// Hybrid execution (src/runtime_aot.c): the game function a host code address was recompiled
/// from, or 0. Weak: absent where the runtime has no hybrid execution.
extern "C" __attribute__((weak)) std::uint64_t runtime_aot_guest_pc(std::uint64_t host_pc);

namespace Game {

// One per game (games/<game>.cpp).
extern const Profile BloodborneProfile;

namespace {
constexpr std::array<const Profile*, 1> Profiles = {&BloodborneProfile};
std::atomic<const Profile*> active{nullptr};
} // namespace

void Select(const char* serial) {
    const char* env = bb_env("BB_GAME_PROFILE");
    if (env && std::strcmp(env, "none") == 0) {
        std::printf("Game profile: none (BB_GAME_PROFILE=none): the game runs as an unknown one\n");
        return;
    }
    for (const Profile* profile : Profiles) {
        const bool match = serial && std::ranges::any_of(profile->serials, [&](const char* s) {
                               return std::strcmp(s, serial) == 0;
                           });
        if (match) {
            active.store(profile, std::memory_order_release);
            std::printf("Game profile: %s (%s)\n", profile->name, serial);
            return;
        }
    }
    std::printf("Game profile: none for %s: the generic paths only\n", serial ? serial : "?");
}

const Profile* Active() {
    return active.load(std::memory_order_acquire);
}

bool IsDynamicWriter(std::uint64_t guest_rip) {
    const Profile* profile = Active();
    if (!profile) {
        return false;
    }
    const auto in = [&](std::uint64_t rip) {
        return std::ranges::any_of(profile->dynamic_writers, [&](const CodeRange& r) {
            return rip >= r.begin && rip < r.end;
        });
    };
    if (in(guest_rip)) {
        return true;
    }
    // Hybrid execution: the writer may be the recompiled copy (its function's start is enough:
    // the ranges are whole functions).
    const std::uint64_t mapped = runtime_aot_guest_pc ? runtime_aot_guest_pc(guest_rip) : 0;
    return mapped && in(mapped);
}

} // namespace Game
