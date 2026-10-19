// SPDX-License-Identifier: GPL-2.0-or-later
// bbport buffers: how the GPU side reaches the game's memory. In place (BB_GUEST_IN_PLACE=1): the
// GPU uses the game's direct memory where it is (its chunks are exported to Vulkan); else copies
// in VRAM. Asked by every buffer binding: the answers are latched in atomics.
#pragma once

#include <atomic>

#include "common/types.h"

namespace VideoCore {

namespace Detail {
/// 0 not known yet, 1 on, 2 off.
inline std::atomic<u8> guest_in_place{0}, write_tracking{0};
bool ComputeGuestInPlace();
bool ComputeWriteTracking();
} // namespace Detail

/// The GPU uses the game's memory in place (bound to the chunks it lives in).
inline bool GuestInPlace() {
    const u8 state = Detail::guest_in_place.load(std::memory_order_relaxed);
    return state != 0 ? state == 1 : Detail::ComputeGuestInPlace();
}
/// The driver cannot bind the game's memory: in place stays off from now on.
void DisableGuestInPlace();
/// CPU writes to GPU memory caught by page protection. Off in place: the GPU side hears of writes
/// from the writers (file reads, the game's loaders, its DMA and command writes);
/// BB_WRITE_TRACKING=1 brings the protection back.
inline bool WriteTracking() {
    const u8 state = Detail::write_tracking.load(std::memory_order_relaxed);
    return state != 0 ? state == 1 : Detail::ComputeWriteTracking();
}
/// BB_WRITE_VERIFY=1 (diagnostics, in place): watched pages are protected as well, so writes
/// nothing announced show up as write faults.
bool WriteVerify();
/// BB_CONSTANTS_IN_PLACE=1 (in place): small read-only buffers (constants) are read where the game
/// wrote them, when the draw executes, instead of being copied when the draw is decoded or
/// recorded. Experiment bit 1 of BB_TOGGLE_FILE inverts it (A/B while the game runs).
bool ConstantsInPlace();

} // namespace VideoCore
