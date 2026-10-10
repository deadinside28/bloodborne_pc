// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/upscaler/frame_generation_helpers.h"

#include <cmath>

namespace Vulkan {

FfxVkPortableImage MakeFgImage(vk::Image image, vk::Format format, u32 width, u32 height,
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

FfxVkPortableFrameGenerationPrepareInfo BuildPrepareInfo(
    const FrameGenerationManager::PrepareInputs& in, bool reset) {
    FfxVkPortableFrameGenerationPrepareInfo prep{};
    prep.structSize = sizeof(prep);
    prep.commandBuffer = in.cmdbuf;
    const vk::Format depth_fmt = (in.depth_format != vk::Format::eUndefined)
                                      ? in.depth_format
                                      : vk::Format::eD32Sfloat;
    prep.depth = MakeFgImage(in.depth_image, depth_fmt, in.render_w, in.render_h,
                             vk::ImageUsageFlagBits::eSampled, vk::ImageAspectFlagBits::eDepth,
                             FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    prep.motionVectors = MakeFgImage(in.motion_image, vk::Format::eR16G16Sfloat, in.render_w, in.render_h,
                                     vk::ImageUsageFlagBits::eSampled, vk::ImageAspectFlagBits::eColor,
                                     FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    prep.renderSize = {in.render_w, in.render_h};
    prep.jitterOffset = {in.jitter_x, in.jitter_y};
    prep.motionVectorScale = {1.0f, 1.0f};

    const float near_p = (in.near_plane > 0.001f && std::isfinite(in.near_plane)) ? in.near_plane : 0.1f;
    const float far_p = (in.far_plane > near_p + 1.0f && std::isfinite(in.far_plane)) ? in.far_plane : (near_p + 2999.9f);
    const float fov = (in.fov_radians > 0.05f && in.fov_radians < 3.10f && std::isfinite(in.fov_radians)) ? in.fov_radians : 0.7853982f;
    const float frame_time = (in.frame_time_ms > 0.001f && in.frame_time_ms < 1000.0f && std::isfinite(in.frame_time_ms)) ? in.frame_time_ms : 16.6667f;

    prep.frameTimeMilliseconds = frame_time;
    prep.cameraNear = near_p;
    prep.cameraFar = far_p;
    prep.cameraVerticalFovRadians = fov;
    prep.viewSpaceToMeters = 1.0f;
    prep.minLuminance = 0.0f;
    prep.maxLuminance = 1.0f;
    prep.transferFunction = FFX_VK_PORTABLE_TRANSFER_FUNCTION_SRGB;
    prep.cameraPosition = {in.camera_pos[0], in.camera_pos[1], in.camera_pos[2]};
    prep.cameraUp = {in.camera_up[0], in.camera_up[1], in.camera_up[2]};
    prep.cameraRight = {in.camera_right[0], in.camera_right[1], in.camera_right[2]};
    prep.cameraForward = {in.camera_forward[0], in.camera_forward[1], in.camera_forward[2]};
    prep.reset = reset ? VK_TRUE : VK_FALSE;
    prep.frameId = in.frame_id;
    return prep;
}

FfxVkPortableFrameGenerationDispatchInfo BuildDispatchInfo(
    const FrameGenerationManager::DispatchInputs& in, vk::Format source_fmt,
    vk::Format output_fmt, u32 display_w, u32 display_h, bool is_reset) {
    FfxVkPortableFrameGenerationDispatchInfo disp{};
    disp.structSize = sizeof(disp);
    disp.commandBuffer = in.cmdbuf;
    disp.currentColor = MakeFgImage(in.current_color_image, source_fmt, display_w, display_h,
                                    vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc,
                                    vk::ImageAspectFlagBits::eColor,
                                    FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    if (in.hudless_color_image) {
        disp.hudlessColor = MakeFgImage(in.hudless_color_image, source_fmt, display_w, display_h,
                                        vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc,
                                        vk::ImageAspectFlagBits::eColor,
                                        FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    } else {
        disp.hudlessColor = MakeFgImage(vk::Image{}, vk::Format::eUndefined, 0, 0,
                                        vk::ImageUsageFlags{}, vk::ImageAspectFlags{},
                                        FFX_VK_PORTABLE_RESOURCE_STATE_UNDEFINED);
    }
    disp.distortionField = MakeFgImage(vk::Image{}, vk::Format::eUndefined, 0, 0,
                                       vk::ImageUsageFlags{}, vk::ImageAspectFlags{},
                                       FFX_VK_PORTABLE_RESOURCE_STATE_UNDEFINED);
    disp.output = MakeFgImage(in.output_image, output_fmt, display_w, display_h,
                              vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                              vk::ImageAspectFlagBits::eColor,
                              FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
    disp.displaySize = {in.display_w, in.display_h};
    disp.interpolationRect = {0, 0, in.display_w, in.display_h};

    const float near_p = (in.near_plane > 0.001f && std::isfinite(in.near_plane)) ? in.near_plane : 0.1f;
    const float far_p = (in.far_plane > near_p + 1.0f && std::isfinite(in.far_plane)) ? in.far_plane : (near_p + 2999.9f);
    const float fov = (in.fov_radians > 0.05f && in.fov_radians < 3.10f && std::isfinite(in.fov_radians)) ? in.fov_radians : 0.7853982f;
    const float frame_time = (in.frame_time_ms > 0.001f && in.frame_time_ms < 1000.0f && std::isfinite(in.frame_time_ms)) ? in.frame_time_ms : 16.6667f;

    disp.frameTimeMilliseconds = frame_time;
    disp.cameraNear = near_p;
    disp.cameraFar = far_p;
    disp.cameraVerticalFovRadians = fov;
    disp.viewSpaceToMeters = 1.0f;
    disp.minLuminance = 0.0f;
    disp.maxLuminance = 1.0f;
    disp.transferFunction = FFX_VK_PORTABLE_TRANSFER_FUNCTION_SRGB;
    disp.reset = is_reset ? VK_TRUE : VK_FALSE;
    disp.frameId = in.frame_id;
    return disp;
}

} // namespace Vulkan
