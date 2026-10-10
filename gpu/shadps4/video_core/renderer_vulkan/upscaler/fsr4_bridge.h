// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <memory>
#include <string>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_fsr4.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

class Fsr4Bridge {
public:
    Fsr4Bridge(const Instance& instance, Scheduler& scheduler);
    ~Fsr4Bridge();

    void CreatePipelines();
    void EnsureResources(u32 width, u32 height);
    void Destroy();

    [[nodiscard]] bool IsActive() const;
    [[nodiscard]] bool HasFailed() const noexcept { return fsr4_failed; }
    void MarkFailed() noexcept { fsr4_failed = true; }
    void ResetFailure() noexcept { fsr4_failed = false; }

    bool Record(vk::CommandBuffer cmdbuf, Fsr4Upscaler::Image color,
                Fsr4Upscaler::Image depth, vk::Image motion_image,
                vk::ImageView motion_view, vk::Image output_image,
                vk::ImageView output_view, u32 w, u32 h, u32 ow, u32 oh,
                int applied_preset, const std::array<float, 2>& jitter,
                float frame_ms, float near_plane, float vertical_fov,
                bool reset);

    bool RecordDecode(vk::CommandBuffer cmdbuf, vk::ImageView color_view,
                      u32 source_width, u32 source_height,
                      Fsr4Upscaler::Image& decoded_input);

    void RecordEncode(vk::CommandBuffer cmdbuf, vk::ImageView output_view,
                      u32 ow, u32 oh);

    void RecordReactive(vk::CommandBuffer cmdbuf, vk::ImageView color,
                        vk::ImageView reactive_mask, vk::ImageView output_view,
                        u32 w, u32 h, u32 ow, u32 oh, const std::array<float, 2>& jitter);

    [[nodiscard]] bool IsLinearFrame() const noexcept { return fsr4_linear_frame; }
    void SetLinearFrame(bool linear) noexcept { fsr4_linear_frame = linear; }

private:
    const Instance& instance;
    Scheduler& scheduler;

    std::unique_ptr<Fsr4Upscaler> fsr4;
    bool fsr4_failed{false};

    VideoCore::UniqueImage fsr4_linear_image;
    vk::UniqueImageView fsr4_linear_view;
    bool fsr4_linear_frame{false};

    vk::UniqueDescriptorSetLayout fsr4_decode_desc_layout;
    vk::UniquePipelineLayout fsr4_decode_pipeline_layout;
    vk::UniquePipeline fsr4_decode_pipeline;

    vk::UniqueDescriptorSetLayout fsr4_encode_desc_layout;
    vk::UniquePipelineLayout fsr4_encode_pipeline_layout;
    vk::UniquePipeline fsr4_encode_pipeline;

    vk::UniqueDescriptorSetLayout fsr4_reactive_desc_layout;
    vk::UniquePipelineLayout fsr4_reactive_pipeline_layout;
    vk::UniquePipeline fsr4_reactive_pipeline;
    vk::UniqueSampler fsr4_linear_sampler;
};

} // namespace Vulkan
