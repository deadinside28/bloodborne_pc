// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

class ImageViewCache {
public:
    ImageViewCache(const Instance& instance, Scheduler& scheduler);
    ~ImageViewCache();

    vk::ImageView Get(const VideoCore::Image& image, vk::Format format, vk::ImageAspectFlags aspect);
    void Destroy();

private:
    const Instance& instance;
    Scheduler& scheduler;

    struct Entry {
        vk::Image image;
        u64 uid{0};
        vk::Format format{};
        vk::ImageAspectFlags aspect{};
        vk::ImageView view{};
        u64 last_use{0};
    };

    std::array<Entry, 6> entries{};
    u64 uses{0};
};

} // namespace Vulkan
