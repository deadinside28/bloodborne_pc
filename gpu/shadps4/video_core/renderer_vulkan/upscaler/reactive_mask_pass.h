// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

class ReactiveMaskPass {
public:
    ReactiveMaskPass(const Instance& instance, Scheduler& scheduler);
    ~ReactiveMaskPass();

    void CreatePipelines();
    void EnsureResources(u32 width, u32 height);
    void Destroy();

    void ResetFrame();
    void SnapshotOpaque(vk::Image source, vk::ImageLayout source_layout, u32 w, u32 h);
    bool Record(vk::ImageView color_view, u32 w, u32 h);

    [[nodiscard]] bool HasSnapshot() const noexcept { return snapshot_taken; }
    [[nodiscard]] bool IsOpaqueValid() const noexcept { return opaque_valid; }
    [[nodiscard]] bool IsMaskReady() const noexcept { return mask_ready; }
    void MarkMaskReady(bool ready = true) noexcept { mask_ready = ready; }
    [[nodiscard]] vk::ImageView OpaqueView() const noexcept { return *opaque_view; }
    [[nodiscard]] vk::ImageView ReactiveView() const noexcept { return *reactive_view; }
    [[nodiscard]] vk::Image ReactiveImage() const noexcept { return vk::Image(reactive_image); }

private:
    const Instance& instance;
    Scheduler& scheduler;

    bool snapshot_taken{false};
    bool opaque_valid{false};
    bool mask_ready{false};

    VideoCore::UniqueImage opaque_image;
    VideoCore::UniqueImage reactive_image;
    vk::UniqueImageView opaque_view;
    vk::UniqueImageView reactive_view;

    vk::UniqueDescriptorSetLayout reactive_desc_layout;
    vk::UniquePipelineLayout reactive_pipeline_layout;
    vk::UniquePipeline reactive_pipeline;
};

} // namespace Vulkan
