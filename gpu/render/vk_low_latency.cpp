// SPDX-License-Identifier: GPL-2.0-or-later
#include "render/vk_low_latency.h"

#include <chrono>
#include <cstdio>

#include "bbport_settings.h"
#include "vulkan/instance.h"

namespace Vulkan {

LowLatency& LowLatency::Get() {
    static LowLatency instance;
    return instance;
}

void LowLatency::Initialize(const Instance& instance) {
    device = instance.GetDevice();
    available = instance.HasLowLatency();
    auto& settings = BbSettings::Get();
    settings.low_latency_supported = available;
    if (!available) {
        std::printf("Low latency: VK_NV_low_latency2 unavailable (NVIDIA Reflex needs NVIDIA's "
                    "driver 550 or newer)\n");
        return;
    }
    const vk::SemaphoreTypeCreateInfo timeline{.semaphoreType = vk::SemaphoreType::eTimeline,
                                               .initialValue = 0};
    sleep_semaphore = Check<"create the low latency semaphore">(
        device.createSemaphoreUnique({.pNext = &timeline}));
    std::printf("Low latency: NVIDIA Reflex (VK_NV_low_latency2) available, %s\n",
                BbSettings::LowLatencyName(BbSettings::EffectiveLowLatency()));
}

void LowLatency::SwapchainCreated(vk::SwapchainKHR created) {
    if (!available) {
        return;
    }
    std::scoped_lock lock{mutex};
    swapchain = created;
    applied_mode = -1; // the mode is per swapchain
}

void LowLatency::SwapchainDestroying(vk::SwapchainKHR destroying) {
    if (!available) {
        return;
    }
    std::scoped_lock lock{mutex};
    if (swapchain != destroying) {
        return;
    }
    // A sleep the game's thread asked for still waits on the semaphore: the driver signals it
    // for this swapchain, so it must be done before the swapchain goes.
    if (sleep_value) {
        const vk::SemaphoreWaitInfo wait{.semaphoreCount = 1,
                                         .pSemaphores = &*sleep_semaphore,
                                         .pValues = &sleep_value};
        (void)device.waitSemaphores(wait, 100'000'000ull);
    }
    swapchain = nullptr;
    applied_mode = -1;
}

void LowLatency::ApplySettings() {
    if (!available) {
        return;
    }
    // The setting, but at least On with DLSS Frame Generation (BbSettings::EffectiveLowLatency).
    const int mode = std::clamp(BbSettings::EffectiveLowLatency(), 0, 2);
    std::scoped_lock lock{mutex};
    if (!swapchain || mode == applied_mode) {
        return;
    }
    const vk::LatencySleepModeInfoNV info{
        .lowLatencyMode = mode != 0,
        .lowLatencyBoost = mode == 2,
        .minimumIntervalUs = 0,
    };
    const vk::Result result = device.setLatencySleepModeNV(swapchain, &info);
    if (result != vk::Result::eSuccess) {
        if (!told.exchange(true)) {
            std::printf("Low latency: the driver refused the sleep mode (%s)\n",
                        vk::to_string(result).c_str());
        }
        return;
    }
    applied_mode = mode;
    std::printf("Low latency: %s\n", BbSettings::LowLatencyName(mode));
}

void LowLatency::Marker(vk::LatencyMarkerNV marker, u64 id) {
    std::scoped_lock lock{mutex};
    if (!swapchain || applied_mode <= 0) {
        return;
    }
    const vk::SetLatencyMarkerInfoNV info{.presentID = id, .marker = marker};
    device.setLatencyMarkerNV(swapchain, &info);
}

void LowLatency::Sleep() {
    u64 value;
    {
        std::scoped_lock lock{mutex};
        if (!swapchain || applied_mode <= 0) {
            return;
        }
        value = ++sleep_value;
        const vk::LatencySleepInfoNV info{.signalSemaphore = *sleep_semaphore, .value = value};
        if (device.latencySleepNV(swapchain, &info) != vk::Result::eSuccess) {
            --sleep_value;
            return;
        }
    }
    // Outside the lock: the present thread goes on meanwhile. Bounded: a stuck driver must not
    // hold the game.
    const vk::SemaphoreWaitInfo wait{.semaphoreCount = 1,
                                     .pSemaphores = &*sleep_semaphore,
                                     .pValues = &value};
    (void)device.waitSemaphores(wait, 50'000'000ull);
}

void LowLatency::GameFrameStart() {
    if (!available) {
        return;
    }
    frame_start_hooked.store(true, std::memory_order_relaxed);
    Sleep();
    const u64 id = sim_id.fetch_add(1, std::memory_order_relaxed) + 1;
    Marker(vk::LatencyMarkerNV::eSimulationStart, id);
}

void LowLatency::GameFrameEnd() {
    if (!available) {
        return;
    }
    Marker(vk::LatencyMarkerNV::eSimulationEnd, sim_id.load(std::memory_order_relaxed));
    if (!frame_start_hooked.load(std::memory_order_relaxed)) {
        // No pacer hook (the game's own 30 FPS): the next frame begins right after the flip.
        Sleep();
        const u64 id = sim_id.fetch_add(1, std::memory_order_relaxed) + 1;
        Marker(vk::LatencyMarkerNV::eSimulationStart, id);
    }
}

void LowLatency::RenderSubmitStart() {
    if (!available || render_marked.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    Marker(vk::LatencyMarkerNV::eRendersubmitStart, render_id.load(std::memory_order_relaxed));
}

u64 LowLatency::RenderSubmitEnd() {
    if (!available) {
        return 0;
    }
    const u64 id = render_id.load(std::memory_order_relaxed);
    if (!render_marked.exchange(false, std::memory_order_relaxed)) {
        Marker(vk::LatencyMarkerNV::eRendersubmitStart, id); // a frame of one submission
    }
    Marker(vk::LatencyMarkerNV::eRendersubmitEnd, id);
    render_id.store(id + 1, std::memory_order_relaxed);
    return id;
}

void LowLatency::PresentStart(u64 id, bool generated) {
    if (!available || id == 0) {
        return;
    }
    Marker(generated ? vk::LatencyMarkerNV::eOutOfBandPresentStart
                     : vk::LatencyMarkerNV::ePresentStart,
           id);
}

void LowLatency::PresentEnd(u64 id, bool generated) {
    if (!available || id == 0) {
        return;
    }
    Marker(generated ? vk::LatencyMarkerNV::eOutOfBandPresentEnd : vk::LatencyMarkerNV::ePresentEnd,
           id);
}

} // namespace Vulkan
