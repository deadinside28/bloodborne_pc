// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/upscaler/fsr3_bridge.h"
#include <cstdio>
#include "ffx_vk_portable.h"

namespace Vulkan {

namespace {

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

void PrintIssues(const char* what, u64 issues) {
    std::printf("Upscaler FSR3: %s invalid:", what);
    for (u32 bit = 0; bit < 64; ++bit) {
        if (issues & (1ull << bit)) {
            std::printf(" %s", ffxVkPortableValidationIssueName(1ull << bit));
        }
    }
    std::printf("\n");
}

FfxVkPortableUpscaleDispatchInfo BuildDispatchInfo(const Fsr3Bridge::DispatchInputs& in) {
    FfxVkPortableUpscaleDispatchInfo info{};
    info.structSize = sizeof(info);
    info.commandBuffer = in.cmdbuf;
    const u32 cw = in.color_width ? in.color_width : in.render_w;
    const u32 ch = in.color_height ? in.color_height : in.render_h;
    const u32 dw = in.depth_width ? in.depth_width : in.render_w;
    const u32 dh = in.depth_height ? in.depth_height : in.render_h;
    info.color = Describe(in.color_image, in.color_format, cw, ch,
                          in.color_usage, vk::ImageAspectFlagBits::eColor,
                          FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    info.depth = Describe(in.depth_image, in.depth_format, dw, dh,
                          in.depth_usage, vk::ImageAspectFlagBits::eDepth,
                          FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    info.motionVectors = Describe(in.motion_image, vk::Format::eR16G16Sfloat, in.render_w,
                                  in.render_h,
                                  vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                                  vk::ImageAspectFlagBits::eColor,
                                  FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    info.output = Describe(in.output_image, in.output_format, in.output_w, in.output_h,
                           in.output_usage, vk::ImageAspectFlagBits::eColor,
                           FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
    if (in.reactive_image) {
        info.reactiveMask = Describe(in.reactive_image, vk::Format::eR8Unorm, in.render_w, in.render_h,
                                     vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                                     vk::ImageAspectFlagBits::eColor,
                                     FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    }
    info.jitterOffset = {in.jitter_x, in.jitter_y};
    info.motionVectorScale = {1.0f, 1.0f};
    info.renderSize = FfxVkPortableExtent2D{in.render_w, in.render_h};
    info.outputSize = FfxVkPortableExtent2D{in.output_w, in.output_h};
    info.frameTimeMilliseconds = in.frame_ms;
    info.preExposure = 1.0f;
    info.cameraNear = in.near_plane;
    info.cameraFar = in.far_plane;
    info.cameraVerticalFovRadians = in.fov_radians;
    info.viewSpaceToMeters = 1.0f;
    info.sharpness = in.sharpness;
    info.enableSharpening = in.sharpen ? VK_TRUE : VK_FALSE;
    info.reset = in.reset ? VK_TRUE : VK_FALSE;
    info.frameId = in.frame_id;
    return info;
}

} // namespace

Fsr3Bridge::Fsr3Bridge(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {}

Fsr3Bridge::~Fsr3Bridge() {
    Destroy();
}

void Fsr3Bridge::Destroy() {
    if (context) {
        scheduler.Finish();
        ffxVkPortableUpscaleContextDestroy(context);
        context = nullptr;
    }
    current_w = current_h = current_ow = current_oh = 0;
}

bool Fsr3Bridge::EnsureContext(u32 w, u32 h, u32 ow, u32 oh, bool hdr) {
    if (context && current_w == w && current_h == h && current_ow == ow && current_oh == oh &&
        current_hdr == hdr) {
        return true;
    }
    Destroy();
    FfxVkPortableDeviceInfo dev_info{};
    dev_info.structSize = sizeof(dev_info);
    dev_info.physicalDevice = instance.GetPhysicalDevice();
    dev_info.device = instance.GetDevice();

    FfxVkPortableUpscaleCreateInfo ci{};
    ci.structSize = sizeof(ci);
    ci.flags = (hdr ? FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT : 0) | FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE;
    ci.maxRenderSize = FfxVkPortableExtent2D{w, h};
    ci.maxOutputSize = FfxVkPortableExtent2D{ow, oh};

    if (const u64 issues = ffxVkPortableValidateUpscaleCreateInfo(&ci)) {
        PrintIssues("create info", issues);
        return false;
    }
    if (ffxVkPortableUpscaleContextCreate(&dev_info, &ci, &context) != FFX_VK_PORTABLE_OK) {
        std::printf("Upscaler: FSR 3 context creation failed\n");
        context = nullptr;
        return false;
    }
    current_w = w; current_h = h; current_ow = ow; current_oh = oh; current_hdr = hdr;
    return true;
}

bool Fsr3Bridge::RecordDispatch(const DispatchInputs& in) {
    if (!context) return false;
    auto info = BuildDispatchInfo(in);
    FfxVkPortableUpscaleCreateInfo ci{};
    ci.structSize = sizeof(ci);
    ci.flags = (current_hdr ? FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT : 0) | FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE;
    ci.maxRenderSize = FfxVkPortableExtent2D{in.render_w, in.render_h};
    ci.maxOutputSize = FfxVkPortableExtent2D{in.output_w, in.output_h};

    if (const u64 issues = ffxVkPortableValidateUpscaleDispatchInfo(&ci, &info)) {
        PrintIssues("dispatch", issues);
        return false;
    }
    return ffxVkPortableUpscaleContextRecordDispatch(context, &info) == FFX_VK_PORTABLE_OK;
}

bool Fsr3Bridge::RecordDispatchAsync(const DispatchInputs& in, std::atomic<bool>& failed_flag) {
    if (!context) return false;
    auto info = BuildDispatchInfo(in);
    FfxVkPortableUpscaleCreateInfo ci{};
    ci.structSize = sizeof(ci);
    ci.flags = (current_hdr ? FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT : 0) | FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE;
    ci.maxRenderSize = FfxVkPortableExtent2D{in.render_w, in.render_h};
    ci.maxOutputSize = FfxVkPortableExtent2D{in.output_w, in.output_h};

    auto checked = info;
    checked.commandBuffer = reinterpret_cast<VkCommandBuffer>(uintptr_t{1});
    if (const u64 issues = ffxVkPortableValidateUpscaleDispatchInfo(&ci, &checked)) {
        PrintIssues("scaled dispatch", issues);
        return false;
    }

    scheduler.Record([this, ctx = context, info, &failed_flag](vk::CommandBuffer cmdbuf) mutable {
        info.commandBuffer = cmdbuf;
        if (ffxVkPortableUpscaleContextRecordDispatch(ctx, &info) != FFX_VK_PORTABLE_OK) {
            std::printf("Upscaler: FSR 3 scaled dispatch failed\n");
            failed_flag.store(true, std::memory_order_relaxed);
        }
    });
    return true;
}

} // namespace Vulkan
