// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan/timeline.h"

#include <limits>

#include "common/assert.h"
#include "render/vk_breadcrumbs.h"
#include "vulkan/instance.h"

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
