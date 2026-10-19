// SPDX-License-Identifier: GPL-2.0-or-later
// bbport render: low latency (NVIDIA Reflex) through VK_NV_low_latency2, the Vulkan form of
// Reflex in NVIDIA's Linux driver (what Proton gives Windows games). The driver paces the CPU
// so that frames do not queue up in front of the GPU: the game's thread sleeps at the start of
// each of its frames (vkLatencySleepNV) until the driver wants the frame started, and the
// frame's simulation, render submissions and present are marked with one ID so the driver can
// measure them. Off on GPUs and drivers without the extension (nothing changes).
// Settings: low_latency in bbport.ini (off, on, boost), the overlay menu, BB_LOW_LATENCY.
#pragma once

#include <atomic>
#include <mutex>

#include "common/types.h"
#include "vulkan/vk_common.h"

namespace Vulkan {

class Instance;

class LowLatency {
public:
    static LowLatency& Get();

    /// After the device exists: on when VK_NV_low_latency2 (and VK_KHR_present_id) are enabled.
    void Initialize(const Instance& instance);
    [[nodiscard]] bool Available() const {
        return available;
    }

    /// The swapchain's lifetime (the present thread). The driver's latency calls need no
    /// external synchronization of the swapchain (the registry): only this object's own handle
    /// is guarded, so the game's thread never waits behind a present.
    void SwapchainCreated(vk::SwapchainKHR swapchain);
    void SwapchainDestroying(vk::SwapchainKHR swapchain);
    /// The settings' mode to the driver (the present thread, each present: cheap when unchanged).
    void ApplySettings();

    /// The game's thread at the start of its frame (the frame-rate hooks' pacer step): the
    /// driver's sleep when low latency is on, then the simulation start marker.
    void GameFrameStart();
    /// The game's thread handed its frame to the GPU (the flip): the simulation end marker.
    /// Without the frame-start hook (30 FPS mode), the next frame starts here too.
    void GameFrameEnd();

    /// The ID of the frame being drawn (the command processor's submissions carry it).
    [[nodiscard]] u64 RenderPresentId() const {
        return render_id.load(std::memory_order_relaxed);
    }
    /// The frame's first submission (marked once per frame).
    void RenderSubmitStart();
    /// The frame's last submission (the presenter's encode): returns the frame's ID and moves on.
    u64 RenderSubmitEnd();
    /// The present of frame `id` (0: a blank frame, unmarked); `generated`: a frame generation
    /// frame shown before it (out of band for the driver).
    void PresentStart(u64 id, bool generated);
    void PresentEnd(u64 id, bool generated);

private:
    void Marker(vk::LatencyMarkerNV marker, u64 id);
    void Sleep();

    vk::Device device{};
    bool available = false;
    mutable std::mutex mutex; ///< the swapchain handle and the driver calls on it
    vk::SwapchainKHR swapchain{};
    vk::UniqueSemaphore sleep_semaphore; ///< timeline: the driver signals it after the sleep
    u64 sleep_value = 0;
    int applied_mode = -1; ///< BbSettings low_latency given to the driver for this swapchain

    std::atomic<u64> sim_id{0};
    std::atomic<u64> render_id{1};
    std::atomic<bool> render_marked{false};
    std::atomic<bool> frame_start_hooked{false};
    std::atomic<bool> told{false};
};

} // namespace Vulkan
