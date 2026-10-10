// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_core/renderer_vulkan/vk_frame_generation.h"
#include "ffx_vk_portable.h"

namespace Vulkan {

FfxVkPortableImage MakeFgImage(vk::Image image, vk::Format format, u32 width, u32 height,
                               vk::ImageUsageFlags usage, vk::ImageAspectFlags aspect,
                               FfxVkPortableResourceState state);

FfxVkPortableFrameGenerationPrepareInfo BuildPrepareInfo(
    const FrameGenerationManager::PrepareInputs& in, bool reset);

FfxVkPortableFrameGenerationDispatchInfo BuildDispatchInfo(
    const FrameGenerationManager::DispatchInputs& in, vk::Format source_fmt,
    vk::Format output_fmt, u32 display_w, u32 display_h, bool is_reset);

} // namespace Vulkan
