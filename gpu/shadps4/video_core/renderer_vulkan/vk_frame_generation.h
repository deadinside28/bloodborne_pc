// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"

struct FfxVkPortableFrameGenerationContext;
struct FfxVkPortableFrameGenerationCreateInfo;

namespace Vulkan {

class FrameGenerationManager {
public:
    FrameGenerationManager(const Instance& instance, Scheduler& scheduler);
    ~FrameGenerationManager();

    [[nodiscard]] bool IsSupported() const noexcept { return supported; }
    [[nodiscard]] bool IsActive() const noexcept;
    [[nodiscard]] const char* GetProblem() const noexcept { return problem; }

    bool Initialize(u32 max_render_w, u32 max_render_h, u32 display_w, u32 display_h,
                    vk::Format source_format, vk::Format output_format);
    void Destroy();
    void Reset();

    struct PrepareInputs {
        vk::CommandBuffer cmdbuf;
        vk::Image depth_image;
        vk::ImageView depth_view;
        vk::Format depth_format{vk::Format::eUndefined};
        vk::Image motion_image;
        vk::ImageView motion_view;
        vk::Image interpolation_source_image;
        vk::ImageView interpolation_source_view;
        u32 render_w{0};
        u32 render_h{0};
        float jitter_x{0.0f};
        float jitter_y{0.0f};
        float frame_time_ms{16.667f};
        float near_plane{0.1f};
        float far_plane{3000.0f};
        float fov_radians{0.785f};
        float camera_pos[3]{0.0f, 0.0f, 0.0f};
        float camera_up[3]{0.0f, 1.0f, 0.0f};
        float camera_right[3]{1.0f, 0.0f, 0.0f};
        float camera_forward[3]{0.0f, 0.0f, 1.0f};
        bool reset{false};
        u64 frame_id{0};
    };

    struct DispatchInputs {
        vk::CommandBuffer cmdbuf;
        vk::Image current_color_image;
        vk::ImageView current_color_view;
        vk::Image hudless_color_image;
        vk::ImageView hudless_color_view;
        vk::Image output_image;
        vk::ImageView output_view;
        u32 display_w{0};
        u32 display_h{0};
        float frame_time_ms{16.667f};
        float near_plane{0.1f};
        float far_plane{3000.0f};
        float fov_radians{0.785f};
        bool reset{false};
        u64 frame_id{0};
    };

    bool RecordPrepare(const PrepareInputs& inputs);
    bool RecordDispatch(const DispatchInputs& inputs);

    [[nodiscard]] bool CanPresentInterpolated() const noexcept {
        return IsActive() && has_interpolated_frame.load(std::memory_order_acquire);
    }
    void ClearInterpolatedFrame() noexcept {
        has_interpolated_frame.store(false, std::memory_order_release);
    }

    [[nodiscard]] vk::Image GetInterpolatedImage() const { return vk::Image(interpolated_image); }
    [[nodiscard]] vk::ImageView GetInterpolatedView() const { return *interpolated_view; }
    [[nodiscard]] vk::Image GetSourceImage() const { return vk::Image(hudless_source_image); }
    [[nodiscard]] vk::ImageView GetSourceView() const { return *hudless_source_view; }

private:
    const Instance& instance;
    Scheduler& scheduler;
    FfxVkPortableFrameGenerationContext* context{nullptr};
    bool supported{false};
    bool failed{false};
    bool needs_reset{true};
    bool prepared{false};
    std::atomic<bool> has_interpolated_frame{false};
    const char* problem{nullptr};

    u32 current_max_render_w{0};
    u32 current_max_render_h{0};
    u32 current_display_w{0};
    u32 current_display_h{0};
    vk::Format current_source_format{vk::Format::eUndefined};
    vk::Format current_output_format{vk::Format::eUndefined};
    std::unique_ptr<FfxVkPortableFrameGenerationCreateInfo> create_info;

    VideoCore::UniqueImage interpolated_image;
    vk::UniqueImageView interpolated_view;
    VideoCore::UniqueImage hudless_source_image;
    vk::UniqueImageView hudless_source_view;
};

} // namespace Vulkan
