// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <memory>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

struct FfxVkPortableUpscaleContext;

namespace Vulkan {

class Fsr3Bridge {
public:
    Fsr3Bridge(const Instance& instance, Scheduler& scheduler);
    ~Fsr3Bridge();

    void Destroy();
    bool EnsureContext(u32 w, u32 h, u32 ow, u32 oh, bool hdr);
    [[nodiscard]] bool HasContext() const noexcept { return context != nullptr; }

    struct DispatchInputs {
        vk::CommandBuffer cmdbuf;
        vk::Image color_image;
        vk::Format color_format;
        vk::ImageUsageFlags color_usage;
        u32 color_width{0};
        u32 color_height{0};
        vk::Image depth_image;
        vk::Format depth_format;
        vk::ImageUsageFlags depth_usage;
        u32 depth_width{0};
        u32 depth_height{0};
        vk::Image motion_image;
        vk::Image output_image;
        vk::Format output_format;
        vk::ImageUsageFlags output_usage;
        vk::Image reactive_image;
        u32 render_w{0};
        u32 render_h{0};
        u32 output_w{0};
        u32 output_h{0};
        float jitter_x{0.0f};
        float jitter_y{0.0f};
        float frame_ms{16.6f};
        float near_plane{0.1f};
        float far_plane{3000.0f};
        float fov_radians{0.785f};
        float sharpness{0.0f};
        bool sharpen{false};
        bool reset{false};
        u64 frame_id{0};
    };

    bool RecordDispatch(const DispatchInputs& in);
    bool RecordDispatchAsync(const DispatchInputs& in, std::atomic<bool>& failed_flag);

private:
    const Instance& instance;
    Scheduler& scheduler;
    FfxVkPortableUpscaleContext* context{nullptr};
    u32 current_w{0}, current_h{0}, current_ow{0}, current_oh{0};
    bool current_hdr{true};
};

} // namespace Vulkan
