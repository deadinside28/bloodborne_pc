// SPDX-License-Identifier: GPL-2.0-or-later
#include "buffers/memory_model.h"

#include <cstdio>
#include <cstdlib>

#include "bbport_toggles.h"

namespace VideoCore {

namespace {

bool EnvFlag(const char* name) {
    const char* env = std::getenv(name);
    return env && env[0] == '1';
}

} // namespace

void DisableGuestInPlace() {
    Detail::guest_in_place.store(2, std::memory_order_relaxed);
    Detail::write_tracking.store(0, std::memory_order_relaxed); // recomputed: on now
}

bool Detail::ComputeGuestInPlace() {
    static const bool requested = [] {
        const bool on = EnvFlag("BB_GUEST_IN_PLACE");
        if (on) {
            std::printf("Guest memory: the GPU uses the game's direct memory in place "
                        "(BB_GUEST_IN_PLACE=1)\n");
        }
        return on;
    }();
    // DisableGuestInPlace may have come first: it stays off then.
    u8 unknown = 0;
    guest_in_place.compare_exchange_strong(unknown, requested ? 1 : 2, std::memory_order_relaxed);
    return guest_in_place.load(std::memory_order_relaxed) == 1;
}

bool Detail::ComputeWriteTracking() {
    static const bool forced = EnvFlag("BB_WRITE_TRACKING");
    const bool on = forced || !GuestInPlace();
    write_tracking.store(on ? 1 : 2, std::memory_order_relaxed);
    return on;
}

bool WriteVerify() {
    static const bool on = EnvFlag("BB_WRITE_VERIFY");
    return on && !WriteTracking();
}

bool ConstantsInPlace() {
    static const bool on = EnvFlag("BB_CONSTANTS_IN_PLACE");
    return GuestInPlace() && on != BbToggle::Experiment(1);
}

} // namespace VideoCore
