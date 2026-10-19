// SPDX-License-Identifier: GPL-2.0-or-later
// bbport vulkan: a timeline semaphore counting submissions. Each submission signals its tick;
// work tagged with a tick is done on the GPU once the semaphore reaches it.
#pragma once

#include <atomic>

#include "common/types.h"
#include "vulkan/vk_common.h"

namespace Vulkan {

class Instance;

class Semaphore {
public:
    explicit Semaphore(const Instance& instance);
    ~Semaphore();

    /// The tick the next submission signals.
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return current_tick.load(std::memory_order_acquire);
    }
    /// The newest tick known to be reached (Refresh asks the GPU).
    [[nodiscard]] u64 KnownGpuTick() const noexcept {
        return gpu_tick.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool IsFree(u64 tick) const noexcept {
        return KnownGpuTick() >= tick;
    }
    /// Takes the current tick for a submission (returned) and moves on.
    [[nodiscard]] u64 NextTick() noexcept {
        return current_tick.fetch_add(1, std::memory_order_release);
    }
    [[nodiscard]] vk::Semaphore Handle() const noexcept {
        return *semaphore;
    }

    /// Reads the GPU's progress.
    void Refresh();
    /// Returns once the GPU reached `tick` (a lost device stops the port with a report).
    void Wait(u64 tick);

private:
    const Instance& instance;
    vk::UniqueSemaphore semaphore;
    std::atomic<u64> gpu_tick{0};
    std::atomic<u64> current_tick{1};
};

} // namespace Vulkan
