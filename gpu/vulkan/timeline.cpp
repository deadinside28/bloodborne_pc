// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan/timeline.h"
#include "shim/bb_env.h"

#include <chrono>
#include <cstdlib>

#include <limits>
#include "common/cpu.h"

#include "common/assert.h"
#include "render/vk_breadcrumbs.h"
#include "vulkan/instance.h"

extern "C" uint64_t runtime_tsc_frequency(void);

namespace Vulkan {

Semaphore::Semaphore(const Instance& instance_) : instance{instance_} {
    const vk::StructureChain create_info = {
        vk::SemaphoreCreateInfo{},
        vk::SemaphoreTypeCreateInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        },
    };
    auto [result, created] = instance.GetDevice().createSemaphoreUnique(create_info.get());
    ASSERT_MSG(result == vk::Result::eSuccess, "timeline semaphore: {}", vk::to_string(result));
    semaphore = std::move(created);
}

Semaphore::~Semaphore() = default;

void Semaphore::Refresh() {
    static const s64 poll_ns = [] {
        const char* env = bb_env("BB_TIMELINE_POLL_US");
#ifdef _WIN32
        const s64 fallback = 50;
#else
        const s64 fallback = 50; // RADV: an ioctl a read (8.6 % of the Deck's recording thread)
#endif
        return (env && *env ? s64(std::strtoll(env, nullptr, 10)) : fallback) * 1000;
    }();
    // In time stamp counter ticks: a clock read through libc was ~1.6 % of the Deck's recording
    // thread (callers check the GPU's progress thousands of times a frame).
    static const s64 poll_ticks = s64(double(poll_ns) * double(runtime_tsc_frequency()) / 1e9);
    if (poll_ticks > 0) {
        const s64 now = s64(Common::Tsc());
        s64 last = last_read_tsc.load(std::memory_order_relaxed);
        if (now - last < poll_ticks ||
            !last_read_tsc.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
            return; // read a moment ago (or by another thread now)
        }
    }
    const auto [result, value] = instance.GetDevice().getSemaphoreCounterValue(*semaphore);
    if (result == vk::Result::eErrorDeviceLost) {
        Breadcrumbs::ReportDeviceLost("reading the GPU progress");
    }
    ASSERT_MSG(result == vk::Result::eSuccess, "timeline value: {}", vk::to_string(result));
    // Raise the known tick, never lower it (several threads refresh).
    u64 known = gpu_tick.load(std::memory_order_acquire);
    while (known < value &&
           !gpu_tick.compare_exchange_weak(known, value, std::memory_order_release,
                                           std::memory_order_acquire)) {
    }
}

void Semaphore::Wait(u64 tick) {
    if (IsFree(tick)) {
        return;
    }
    Refresh();
    if (IsFree(tick)) {
        return;
    }
    const vk::Semaphore handle = *semaphore;
    const vk::SemaphoreWaitInfo wait{
        .semaphoreCount = 1,
        .pSemaphores = &handle,
        .pValues = &tick,
    };
    for (;;) {
        const vk::Result result =
            instance.GetDevice().waitSemaphores(&wait, std::numeric_limits<u64>::max());
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eErrorDeviceLost) {
            Breadcrumbs::ReportDeviceLost("waiting for a submission");
            ASSERT_MSG(false, "Device lost while waiting for a submission");
        }
    }
    Refresh();
}

} // namespace Vulkan
