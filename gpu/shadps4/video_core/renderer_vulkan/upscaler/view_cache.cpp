// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/upscaler/view_cache.h"

namespace Vulkan {

ImageViewCache::ImageViewCache(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {}

ImageViewCache::~ImageViewCache() {
    Destroy();
}

void ImageViewCache::Destroy() {
    const auto device = instance.GetDevice();
    for (auto& entry : entries) {
        if (entry.view) {
            device.destroyImageView(entry.view);
            entry.view = nullptr;
        }
        entry.image = nullptr;
        entry.uid = 0;
        entry.last_use = 0;
    }
}

vk::ImageView ImageViewCache::Get(const VideoCore::Image& image, vk::Format format,
                                  vk::ImageAspectFlags aspect) {
    const vk::Image handle = image.GetImage();
    Entry* oldest = &entries[0];
    for (auto& entry : entries) {
        if (entry.view && entry.image == handle && entry.uid == image.image_uid &&
            entry.format == format && entry.aspect == aspect) {
            entry.last_use = ++uses;
            return entry.view;
        }
        if (entry.last_use < oldest->last_use) {
            oldest = &entry;
        }
    }

    const auto device = instance.GetDevice();
    if (oldest->view) {
        scheduler.DeferOperation([device, view = oldest->view] { device.destroyImageView(view); });
    }
    *oldest = {
        .image = handle,
        .uid = image.image_uid,
        .format = format,
        .aspect = aspect,
        .view = Check(device.createImageView({
            .image = handle,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        })),
        .last_use = ++uses,
    };
    return oldest->view;
}

} // namespace Vulkan
