// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <memory>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_frame_generation.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

class CameraMotion;

class FrameGenerationBridge {
public:
    FrameGenerationBridge(const Instance& instance, Scheduler& scheduler);
    ~FrameGenerationBridge();

    void Destroy();
    void SetDisplayInfo(u32 display_w, u32 display_h, vk::Format display_format);

    [[nodiscard]] FrameGenerationManager* GetManager() noexcept {
        return frame_generation.get();
    }
    [[nodiscard]] const FrameGenerationManager* GetManager() const noexcept {
        return frame_generation.get();
    }
    [[nodiscard]] bool IsActive() const noexcept {
        return frame_generation && frame_generation->IsActive();
    }

    bool Prepare(vk::CommandBuffer cmdbuf, vk::Image scene_image, u32 render_w, u32 render_h,
                 u32 out_w, u32 out_h, vk::Image depth_image, vk::ImageView depth_view,
                 vk::Format depth_format, vk::Image motion_image, vk::ImageView motion_view,
                 CameraMotion& camera_motion, float frame_ms, bool reset, u64 frame_id);

    bool RecordDispatch(vk::CommandBuffer cmdbuf, vk::Image current_color, u32 display_w,
                        u32 display_h, float frame_ms, CameraMotion& camera_motion,
                        bool reset);

private:
    const Instance& instance;
    Scheduler& scheduler;
    std::unique_ptr<FrameGenerationManager> frame_generation;

    u32 disp_w{1920};
    u32 disp_h{1080};
    vk::Format disp_format{vk::Format::eB8G8R8A8Unorm};
    u64 last_frame_id{0};
    bool is_prepared{false};
};

} // namespace Vulkan
