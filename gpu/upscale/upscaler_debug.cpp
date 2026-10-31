// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the temporal upscaler's debug dumps (BB_DUMP_TRIGGER, BB_FINAL_DUMP_TRIGGER, the
// presented frames) and its validation messages. Split out of vk_temporal_upscaler.cpp
// (2026-10-26); declared in upscaler_internal.h.

#include "upscale/vk_temporal_upscaler.h"
#include "shim/bb_env.h"
#include "render/vk_gpu_profiler.h"
#include "upscale/motion_history.h"
#include "upscale/vk_scene_resolution.h"
#include "upscale/ui_composition.h"

#include <array>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <vk_mem_alloc.h>

#include "bbport_guest_hooks.h"
#include "game_profile.h"
#include "bbport_settings.h"
#include "bbport_toggles.h"
#include "ffx_vk_portable.h"
#include "bbgpu/shaders/upscale_merge_comp.h"
#include "bbgpu/shaders/upscale_reactive_comp.h"
#include "bbgpu/shaders/taa_comp.h"
#include "bbgpu/shaders/taa_sharpen_comp.h"
#include "bbgpu/shaders/taa_sharpen_ldr_comp.h"
#include "bbgpu/shaders/fsr1_easu_comp.h"
#include "bbgpu/shaders/fsr4_decode_comp.h"
#include "bbgpu/shaders/fsr4_encode_comp.h"
#include "bbgpu/shaders/fsr4_reactive_comp.h"
#include "vulkan/shader_module.h"
#include "upscale/vk_camera_motion.h"
#include "upscale/vk_frame_generation.h"
#include "upscale/vk_smaa.h"
#include "render/vk_frame_capture.h"
#include "vulkan/instance.h"
#include "render/runtime.h"
#include "render/scheduler.h"
#include "textures/texture_cache.h"
#include "upscale/upscaler_internal.h"

namespace Vulkan::UpscalerDebug {

FfxVkPortableImage Describe(vk::Image image, vk::Format format, u32 width, u32 height,
                            vk::ImageUsageFlags usage, vk::ImageAspectFlags aspect,
                            FfxVkPortableResourceState state) {
    FfxVkPortableImage out{};
    out.structSize = sizeof(out);
    out.image = image;
    out.format = static_cast<VkFormat>(format);
    out.extent = {width, height};
    out.mipCount = 1;
    out.arrayLayers = 1;
    out.usage = static_cast<VkImageUsageFlags>(usage);
    out.aspect = static_cast<VkImageAspectFlags>(aspect);
    out.state = state;
    return out;
}

/// bbport: BB_DUMP_TRIGGER=<file> BB_DUMP_DIR=<dir> (default out/dump): creating the file dumps
/// the upscaler's images of the next BB_DUMP_FRAMES (8) frames as raw files
/// <dir>/fNNN_<name>_<w>x<h>_<format>.raw, for checking temporal stability offline.
/// Returns the frame number to dump, or -1.
int DumpFrame() {
    static const char* trigger = bb_env("BB_DUMP_TRIGGER");
    static int remaining = 0, index = 0, polls = 0;
    if (!trigger) {
        return -1;
    }
    // BB_DUMP_EVERY=1: every frame while the trigger file exists (the file is left in place).
    static const bool every = bb_env("BB_DUMP_EVERY") != nullptr;
    if (every) {
        if (std::filesystem::exists(trigger)) {
            std::printf("Dump frame %d: GPU frame %llu\n", index,
                        (unsigned long long)BbStats::gpu_frames.load(std::memory_order_relaxed));
            return index++;
        }
        return -1;
    }
    if (remaining == 0) {
        if (++polls % 30 != 0 || std::remove(trigger) != 0) {
            return -1;
        }
        const char* frames = bb_env("BB_DUMP_FRAMES");
        remaining = frames ? std::max(1, std::atoi(frames)) : 8;
        index = 0;
    }
    --remaining;
    return index++;
}

namespace {
/// Host buffers of finished dumps, kept for the next ones: thousands of dumps in a row (sequence
/// dumps) with a new allocation each ran the driver out of memory for submissions (device lost).
struct DumpBuffer {
    VkBuffer buffer;
    VmaAllocation allocation;
    VmaAllocationInfo info;
    VkDeviceSize size;
    bool float16 = false;
};
std::mutex dump_pool_mutex;
std::vector<DumpBuffer> dump_pool;
} // namespace

/// Copies `images` into host buffers after the commands recorded so far and writes them to
/// files once the GPU is done.
void DumpImages(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                int frame, std::initializer_list<DumpImage> images) {
    static const std::string dir = [] {
        const char* env = bb_env("BB_DUMP_DIR");
        return std::string{env && env[0] ? env : "out/dump"};
    }();
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
    for (const auto& image : images) {
        // BB_DUMP_ONLY=name,name: only those images.
        static const char* only = bb_env("BB_DUMP_ONLY");
        if (only && !std::strstr(only, image.name)) {
            continue;
        }
        const VkDeviceSize size = VkDeviceSize(image.width) * image.height *
                                  (image.float16 ? 8u : image.bytes_per_pixel);
        const VkBufferCreateInfo buffer_ci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        };
        const VmaAllocationCreateInfo alloc_ci{
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                     VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        };
        DumpBuffer pooled{};
        {
            std::scoped_lock lock{dump_pool_mutex};
            const auto it = std::find_if(dump_pool.begin(), dump_pool.end(),
                                         [size](const DumpBuffer& b) { return b.size == size; });
            if (it != dump_pool.end()) {
                pooled = *it;
                dump_pool.erase(it);
            }
        }
        if (!pooled.buffer &&
            vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &pooled.buffer,
                            &pooled.allocation, &pooled.info) != VK_SUCCESS) {
            std::printf("Dump: no host memory for %s\n", image.name);
            continue;
        }
        pooled.size = size;
        pooled.float16 = image.float16;
        const VkBuffer buffer = pooled.buffer;
        const vk::BufferImageCopy region{
            .imageSubresource = {image.aspect, 0, 0, 1},
            .imageExtent = {image.width, image.height, 1},
        };
        cmdbuf.copyImageToBuffer(image.image, vk::ImageLayout::eGeneral, buffer, region);
        char path[512];
        std::snprintf(path, sizeof(path), "%s/f%03d_%s_%ux%u_%s.raw", dir.c_str(), frame,
                      image.name, image.width, image.height, image.format);
        scheduler.DeferPriorityOperation(
            [allocator = instance.GetAllocator(), pooled, file = std::string{path},
             width = image.width, height = image.height] {
                const auto& [buffer, allocation, info, size, float16] = pooled;
                vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
                if (FILE* f = std::fopen(file.c_str(), "wb")) {
                    if (float16) {
                        // RGBA16F: the texels are halfs of the game's encoding, possibly above
                        // its white — clamped into 8-bit RGBA for viewing.
                        const auto* halfs = static_cast<const unsigned short*>(info.pMappedData);
                        std::vector<unsigned char> rgba8(size_t(width) * height * 4);
                        for (size_t p = 0; p < rgba8.size() / 4; ++p) {
                            for (int c = 0; c < 3; ++c) {
                                const unsigned short h = halfs[p * 4 + unsigned(c)];
                                const int sign = h & 0x8000 ? -1 : 1;
                                const int exponent = int((h >> 10) & 0x1F) - 15;
                                const unsigned mantissa = h & 0x3FF;
                                const float value =
                                    exponent <= 0
                                        ? 0.0f
                                        : sign * std::ldexp(1.0f + float(mantissa) / 1024.0f,
                                                            exponent);
                                rgba8[p * 4 + unsigned(c)] =
                                    static_cast<unsigned char>(std::clamp(value, 0.0f, 1.0f) *
                                                               255.0f);
                            }
                            rgba8[p * 4 + 3] = 255;
                        }
                        std::fwrite(rgba8.data(), 1, rgba8.size(), f);
                        float max_value = 0.0f;
                        for (size_t p = 0; p < rgba8.size() / 4; ++p) {
                            for (int c = 0; c < 3; ++c) {
                                const unsigned short h = halfs[p * 4 + unsigned(c)];
                                const int exponent = int((h >> 10) & 0x1F) - 15;
                                const unsigned mantissa = h & 0x3FF;
                                if (exponent > 0) {
                                    max_value = std::max(max_value,
                                        std::ldexp(1.0f + float(mantissa) / 1024.0f, exponent));
                                }
                            }
                        }
                        const std::string sidecar = file + ".max";
                        if (FILE* m = std::fopen(sidecar.c_str(), "w")) {
                            std::fprintf(m, "%.4f\n", max_value);
                            std::fclose(m);
                        }
                    } else {
                        std::fwrite(info.pMappedData, 1, size, f);
                    }
                    std::fclose(f);
                }
                std::scoped_lock lock{dump_pool_mutex};
                if (dump_pool.size() < 64) {
                    dump_pool.push_back(pooled);
                } else {
                    vmaDestroyBuffer(allocator, buffer, allocation);
                }
            });
    }
    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eNone,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
    std::printf("Dump: frame %d -> %s\n", frame, dir.c_str());
}

void PrintIssues(const char* what, u64 issues) {
    std::printf("Upscaler: %s invalid:", what);
    for (u32 bit = 0; bit < 64; ++bit) {
        if (issues & (1ull << bit)) {
            std::printf(" %s", ffxVkPortableValidationIssueName(1ull << bit));
        }
    }
    std::printf("\n");
}

} // namespace Vulkan::UpscalerDebug

namespace Vulkan {

void DumpPresentedFloat16(const Instance& instance, Scheduler& scheduler,
                          vk::CommandBuffer cmdbuf, vk::Image image, u32 width, u32 height,
                          int index, const char* name, vk::Format source_format) {
    if (source_format == vk::Format::eR16G16B16A16Sfloat) {
        UpscalerDebug::DumpImages(instance, scheduler, cmdbuf, index,
                                  {{image, width, height, 4, name, "rgba16f",
                                    vk::ImageAspectFlagBits::eColor, true}});
        return;
    }
    const bool bgra = source_format == vk::Format::eB8G8R8A8Unorm ||
                      source_format == vk::Format::eB8G8R8A8Srgb;
    UpscalerDebug::DumpImages(instance, scheduler, cmdbuf, index,
                              {{image, width, height, 4, name, bgra ? "bgra" : "rgba",
                                vk::ImageAspectFlagBits::eColor, false}});
}

} // namespace Vulkan

namespace Vulkan {

void DumpRaw(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
             vk::Image image, u32 width, u32 height, u32 bytes_per_texel, int index, const char* name,
             const char* format) {
    DumpImages(instance, scheduler, cmdbuf, index, {{image, width, height, bytes_per_texel, name, format}});
}

void DumpPresented(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                   vk::Image image, u32 width, u32 height, vk::Format format, int index,
                   const char* name) {
    const bool bgra = format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
    DumpImages(instance, scheduler, cmdbuf, index, {{image, width, height, 4, name, bgra ? "bgra" : "rgba"}});
}

void DumpFinalFrameIfDue(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                         vk::Image image, u32 width, u32 height, vk::Format format) {
    static const char* trigger = bb_env("BB_FINAL_DUMP_TRIGGER");
    static int index = 0;
    if (!trigger || std::remove(trigger) != 0) {
        return;
    }
    std::printf("Final dump %d: GPU frame %llu\n", index,
                (unsigned long long)BbStats::gpu_frames.load(std::memory_order_relaxed));
    if (format == vk::Format::eR16G16B16A16Sfloat) {
        // Native+: the frame is the chain's float image — converted for viewing.
        DumpImages(instance, scheduler, cmdbuf, index++,
                   {{image, width, height, 4, "final", "rgba16f", vk::ImageAspectFlagBits::eColor,
                     true}});
        return;
    }
    const bool bgra = format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
    DumpImages(instance, scheduler, cmdbuf, index++,
               {{image, width, height, 4, "final", bgra ? "bgra" : "rgba"}});
}

bool TemporalUpscaler::PresentDumpDue() {
    // Diagnostic capture of the actual completed display buffer, including native UI.
    // BB_PRESENT_DUMP_COUNT=N: the trigger dumps N consecutive frames (temporal stability).
    static const char* dump = bb_env("BB_PRESENT_DUMP_TRIGGER");
    static const int dump_count = [] {
        const char* env = bb_env("BB_PRESENT_DUMP_COUNT");
        return env ? std::max(1, std::atoi(env)) : 1;
    }();
    if (dump && present_dump_remaining == 0 && std::remove(dump) == 0) {
        present_dump_remaining = dump_count;
    }
    if (present_dump_remaining == 0) {
        return false;
    }
    --present_dump_remaining;
    return true;
}

void TemporalUpscaler::DumpPresented(vk::Image image, u32 width, u32 height, vk::Format format) {
    scheduler.EndRendering();
    const bool bgra = format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
    DumpImages(instance, scheduler, scheduler.CommandBuffer(), present_dump_index++,
               {{image, width, height, 4, "present", bgra ? "bgra" : "rgba"}});
}
} // namespace Vulkan
