// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

class MergePass {
public:
    MergePass(const Instance& instance, Scheduler& scheduler);
    ~MergePass();

    void CreatePipelines();
    void Destroy();

    void Record(vk::CommandBuffer cmdbuf, vk::ImageView output_view, vk::ImageView scene_view,
                vk::ImageView mask_view, vk::ImageView motion_view, vk::ImageView object_motion_view,
                u32 ow, u32 oh, u32 mode);

private:
    const Instance& instance;
    Scheduler& scheduler;

    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniquePipeline pipeline;
};

} // namespace Vulkan
