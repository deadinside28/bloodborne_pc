// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

class TaaPass {
public:
    TaaPass(const Instance& instance, Scheduler& scheduler);
    ~TaaPass();

    void CreatePipelines();
    void EnsureResources(u32 width, u32 height);
    void Destroy();

    void Record(vk::CommandBuffer cmdbuf, vk::ImageView color, vk::ImageView depth,
                vk::ImageView motion, vk::ImageView output_view, vk::ImageView opaque_view,
                u32 out_width, u32 out_height, const std::array<float, 2>& jitter,
                bool reset, const std::array<std::array<float, 4>, 3>& depth_params);

    void RecordExtraSharpen(vk::Image target, vk::ImageView output_view,
                            vk::UniqueImageView& ui_storage_view, bool ldr,
                            u32 w, u32 h);

private:
    const Instance& instance;
    Scheduler& scheduler;

    std::array<VideoCore::UniqueImage, 2> taa_history;
    std::array<vk::UniqueImageView, 2> taa_history_views;
    u32 taa_next{0};

    vk::UniqueSampler taa_sampler;
    vk::UniqueDescriptorSetLayout taa_desc_layout;
    vk::UniquePipelineLayout taa_pipeline_layout;
    vk::UniquePipeline taa_pipeline;

    vk::UniqueDescriptorSetLayout taa_sharpen_desc_layout;
    vk::UniquePipelineLayout taa_sharpen_pipeline_layout;
    vk::UniquePipeline taa_sharpen_pipeline;
    vk::UniquePipeline taa_sharpen_ldr_pipeline;

    VideoCore::UniqueImage extra_sharpen_image;
    vk::UniqueImageView extra_sharpen_view;
    u32 extra_sharpen_width{0}, extra_sharpen_height{0};
};

} // namespace Vulkan
