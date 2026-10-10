// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <initializer_list>
#include <string>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

struct DumpImage {
    vk::Image image; ///< in layout General
    u32 width, height, bytes_per_pixel;
    const char* name;
    const char* format;
    vk::ImageAspectFlagBits aspect = vk::ImageAspectFlagBits::eColor;
};

class UpscalerDiagnostics {
public:
    UpscalerDiagnostics(const Instance& instance, Scheduler& scheduler);

    /// Checks if a multi-frame dump trigger is active. Returns frame index or -1.
    int CheckFrameDump();

    /// Copies `images` into host buffers and writes them to raw files.
    void DumpImages(vk::CommandBuffer cmdbuf, int frame,
                    std::initializer_list<DumpImage> images);

    /// bbport BB_FINAL_DUMP_TRIGGER: saves the final presented frame.
    void DumpFinalFrameIfDue(vk::CommandBuffer cmdbuf, vk::Image image, u32 width, u32 height,
                             vk::Format format);

    /// bbport BB_PRESENT_DUMP_TRIGGER: checks if present dump is due.
    bool PresentDumpDue();

    /// Saves `image` as present_<w>x<h> in BB_DUMP_DIR.
    void DumpPresented(vk::Image image, u32 width, u32 height, vk::Format format);

private:
    const Instance& instance;
    Scheduler& scheduler;
    int present_dump_remaining{0};
    int present_dump_index{0};
};

/// Global function for compatibility if called externally.
void DumpFinalFrameIfDue(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                         vk::Image image, u32 width, u32 height, vk::Format format);

void DumpImages(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                int frame, std::initializer_list<DumpImage> images);

int DumpFrame();

} // namespace Vulkan
