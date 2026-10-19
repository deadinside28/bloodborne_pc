// SPDX-License-Identifier: GPL-2.0-or-later
#include "upscale/vk_frame_generation.h"
#include "bbport_toggles.h"

#include <atomic>
#include <cstdio>

#include <vk_mem_alloc.h>

#include "bbport_settings.h"
#include "ffx_vk_portable.h"
#include "render/scheduler.h"
#include "vulkan/instance.h"

namespace Vulkan {

namespace {

constexpr vk::ImageSubresourceRange ColorRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

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

/// The output-size images the frame generation reads and writes: the scene's image (the UI
/// image of the upscaler), the HUD-less copy and the outputs.
constexpr vk::ImageUsageFlags ColorUsage = vk::ImageUsageFlagBits::eStorage |
                                           vk::ImageUsageFlagBits::eSampled |
                                           vk::ImageUsageFlagBits::eTransferSrc |
                                           vk::ImageUsageFlagBits::eTransferDst;

void ImageBarrier(vk::CommandBuffer cmdbuf, vk::Image image, vk::PipelineStageFlags2 src_stage,
                  vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                  vk::AccessFlags2 dst_access, vk::ImageLayout old_layout = vk::ImageLayout::eGeneral) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = image,
        .subresourceRange = ColorRange,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
}

} // namespace

namespace {
std::atomic<u64> stat_captured{0}, stat_interpolated{0}, stat_ready{0}, stat_taken{0}, stat_missed{0};
void Stats() {
    static std::atomic<u64> calls{0};
    if (++calls % 2000 == 0) {
        std::printf("Frame generation stats: captured %llu interpolated %llu ready %llu taken %llu "
                    "missed %llu\n", (unsigned long long)stat_captured.load(),
                    (unsigned long long)stat_interpolated.load(), (unsigned long long)stat_ready.load(),
                    (unsigned long long)stat_taken.load(), (unsigned long long)stat_missed.load());
    }
}
} // namespace

FrameGeneration::FrameGeneration(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    FfxVkPortableDeviceCapabilities caps{};
    caps.structSize = sizeof(caps);
    auto& settings = BbSettings::Get();
    if (ffxVkPortableQueryDeviceCapabilities(instance.GetPhysicalDevice(), &caps) !=
        FFX_VK_PORTABLE_OK) {
        settings.frame_generation_problem = "the GPU's capabilities could not be read";
    } else if (!caps.fsr3FrameGenerationPrerequisites) {
        settings.frame_generation_problem = "the GPU lacks what FSR 3 frame generation needs";
    } else {
        supported = true;
    }
    settings.frame_generation_supported = supported;
    std::printf("Frame generation: FSR 3.1 %s%s%s\n", supported ? "available" : "unavailable",
                supported ? "" : ": ", supported ? "" : settings.frame_generation_problem.load());
}

FrameGeneration::~FrameGeneration() {
    Destroy();
}

bool FrameGeneration::Wanted() const {
    return supported && !failed && BbSettings::Get().frame_generation.load(std::memory_order_relaxed);
}

void FrameGeneration::Destroy() {
    if (context) {
        scheduler.Finish();
        ffxVkPortableFrameGenerationContextDestroy(context);
        context = nullptr;
    }
}

bool FrameGeneration::EnsureContext(u32 render_width, u32 render_height, u32 width, u32 height,
                                    vk::Format format) {
    if (context && context_render_w == render_width && context_render_h == render_height &&
        context_w == width && context_h == height && context_format == format) {
        return true;
    }
    Destroy();
    const auto device = instance.GetDevice();
    FfxVkPortableDeviceInfo device_info{};
    device_info.structSize = sizeof(device_info);
    device_info.instance = instance.GetInstance();
    device_info.physicalDevice = instance.GetPhysicalDevice();
    device_info.device = device;
    device_info.getDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    device_info.queue = instance.GetGraphicsQueue();
    device_info.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex();
    device_info.shaderFloat16Enabled = instance.IsShaderFloat16Enabled();
    device_info.subgroupSizeControlEnabled = instance.IsSubgroupSizeControlEnabled();
    device_info.synchronization2Enabled = VK_TRUE;
    device_info.shaderStorageImageWriteWithoutFormatEnabled = VK_TRUE;

    FfxVkPortableFrameGenerationCreateInfo create{};
    create.structSize = sizeof(create);
    create.maxRenderSize = {render_width, render_height};
    create.displaySize = {width, height};
    create.interpolationSourceFormat = static_cast<VkFormat>(format);
    create.outputFormat = static_cast<VkFormat>(format);
    if (const u64 issues = ffxVkPortableValidateFrameGenerationCreateInfo(&create)) {
        std::printf("Frame generation: invalid context (%s)\n", ffxVkPortableValidationIssueName(issues));
        failed = true;
        return false;
    }
    if (ffxVkPortableFrameGenerationContextCreate(&device_info, &create, &context) !=
        FFX_VK_PORTABLE_OK) {
        std::printf("Frame generation: the context could not be made\n");
        context = nullptr;
        failed = true;
        return false;
    }
    context_render_w = render_width;
    context_render_h = render_height;
    context_w = width;
    context_h = height;
    context_format = format;
    reset_next = true;
    std::printf("Frame generation: %ux%u from %ux%u scenes (%s)\n", width, height, render_width,
                render_height, vk::to_string(format).c_str());
    return true;
}

void FrameGeneration::CaptureScene(const Scene& in) {
    scene_captured = false;
    if (!Wanted() || !EnsureContext(in.render_width, in.render_height, in.width, in.height,
                                    in.color_format)) {
        reset_next = true;
        return;
    }
    scene = in;
    scene_captured = true;
    scene_id = ++frame_id;
    hudless_ready = false;
    scene_reset = reset_next || in.reset;
    reset_next = false;
    ++stat_captured;
    // The frame is interpolated as shown, UI and all (no HUD-less image: what the game draws over
    // the upscaled scene is not only its HUD, and FSR pastes any difference uninterpolated,
    // a double image). Optical flow needs that finished frame: Prepare runs at the display pass.
    // The depth is copied now, while it is this frame's (the game reuses its depth buffer).
    if (!depth_copy || depth_copy_format != in.depth_format || depth_copy_w != in.render_width ||
        depth_copy_h != in.render_height) {
        scheduler.Finish();
        depth_copy = VideoCore::HostImage(instance.GetDevice(), instance.GetAllocator());
        depth_copy.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = in.depth_format,
            .extent = {in.render_width, in.render_height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
                     vk::ImageUsageFlagBits::eDepthStencilAttachment,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        depth_copy_format = in.depth_format;
        depth_copy_w = in.render_width;
        depth_copy_h = in.render_height;
    }
    scheduler.Record([depth = in.depth, copy = vk::Image(depth_copy), w = in.render_width,
                      h = in.render_height](vk::CommandBuffer cmdbuf) {
        const auto all = vk::PipelineStageFlagBits2::eAllCommands;
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
        const std::array<vk::ImageMemoryBarrier2, 2> before{{
            {.srcStageMask = all, .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
             .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
             .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
             .oldLayout = vk::ImageLayout::eGeneral, .newLayout = vk::ImageLayout::eGeneral,
             .image = depth, .subresourceRange = range},
            {.srcStageMask = all, .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
             .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
             .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
             .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eGeneral,
             .image = copy, .subresourceRange = range},
        }};
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = before.data()});
        const vk::ImageCopy region{
            .srcSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
            .dstSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
            .extent = {w, h, 1},
        };
        cmdbuf.copyImage(depth, vk::ImageLayout::eGeneral, copy, vk::ImageLayout::eGeneral, region);
        const std::array<vk::ImageMemoryBarrier2, 2> after{{
            {.srcStageMask = vk::PipelineStageFlagBits2::eCopy,
             .srcAccessMask = vk::AccessFlagBits2::eTransferRead, .dstStageMask = all,
             .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
             .oldLayout = vk::ImageLayout::eGeneral, .newLayout = vk::ImageLayout::eGeneral,
             .image = depth, .subresourceRange = range},
            {.srcStageMask = vk::PipelineStageFlagBits2::eCopy,
             .srcAccessMask = vk::AccessFlagBits2::eTransferWrite, .dstStageMask = all,
             .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
             .oldLayout = vk::ImageLayout::eGeneral, .newLayout = vk::ImageLayout::eGeneral,
             .image = copy, .subresourceRange = range},
        }};
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = after.data()});
    });
}


void FrameGeneration::CaptureHudless(vk::Image color) {
    if (!scene_captured || color != scene.color || hudless_ready) {
        return;
    }
    if (!hudless) {
        scheduler.Finish();
        hudless = VideoCore::HostImage(instance.GetDevice(), instance.GetAllocator());
        hudless.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = context_format,
            .extent = {context_w, context_h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = ColorUsage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
    }
    hudless_ready = true;
    scheduler.EndRendering();
    scheduler.Record([color, copy = vk::Image(hudless), w = scene.width,
                      h = scene.height](vk::CommandBuffer cmdbuf) {
        const auto all = vk::PipelineStageFlagBits2::eAllCommands;
        ImageBarrier(cmdbuf, color, all, vk::AccessFlagBits2::eMemoryWrite,
                     vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead);
        ImageBarrier(cmdbuf, copy, all, vk::AccessFlagBits2::eMemoryRead,
                     vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite,
                     vk::ImageLayout::eUndefined);
        const vk::ImageCopy region{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .extent = {w, h, 1},
        };
        cmdbuf.copyImage(color, vk::ImageLayout::eGeneral, copy, vk::ImageLayout::eGeneral, region);
        ImageBarrier(cmdbuf, color, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferRead, all,
                     vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite);
        ImageBarrier(cmdbuf, copy, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferWrite, all, vk::AccessFlagBits2::eMemoryRead);
    });
}

void FrameGeneration::Interpolate(vk::Image color, VAddr display) {
    Stats();
    if (!scene_captured || color != scene.color || !context) {
        return;
    }
    ++stat_interpolated;
    scene_captured = false;
    const u64 id = scene_id;
    const bool reset = scene_reset;

    Target* target;
    {
        std::scoped_lock lock{targets_mutex};
        // One output for all display buffers: the interpolation, the display pass and the
        // presenter's encode are all in the draw stream's order. `display` is the UI target.
        (void)display;
        target = &targets[0];
        if (!target->image || target->format != scene.color_format || target->width != scene.width ||
            target->height != scene.height) {
            scheduler.Finish(); // its last encode may still read it
            const auto device = instance.GetDevice();
            target->view.reset();
            target->image = VideoCore::HostImage(device, instance.GetAllocator());
            target->image.Create(vk::ImageCreateInfo{
                // The presenter may view it as sRGB (as it views the display buffer).
                .flags = vk::ImageCreateFlagBits::eMutableFormat |
                         vk::ImageCreateFlagBits::eExtendedUsage,
                .imageType = vk::ImageType::e2D,
                .format = scene.color_format,
                .extent = {scene.width, scene.height, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = ColorUsage,
                .initialLayout = vk::ImageLayout::eUndefined,
            });
            target->view = Check<"create a frame generation view">(device.createImageViewUnique({
                .image = vk::Image(target->image),
                .viewType = vk::ImageViewType::e2D,
                .format = scene.color_format,
                .subresourceRange = ColorRange,
            }));
            target->format = scene.color_format;
            target->width = scene.width;
            target->height = scene.height;
        }
        // Ready as recorded (the presenter's encode comes later in the same stream). The first
        // frame after a reset has nothing before it to interpolate from.
        target->ready = !reset;
        target->frame = id;
        if (target->ready) ++stat_ready;
    }

    FfxVkPortableFrameGenerationDispatchInfo dispatch{};
    dispatch.structSize = sizeof(dispatch);
    dispatch.currentColor = Describe(color, scene.color_format, scene.width, scene.height,
                                     ColorUsage | vk::ImageUsageFlagBits::eColorAttachment,
                                     vk::ImageAspectFlagBits::eColor,
                                     FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    // The UI-less frame (CaptureHudless): FSR interpolates it and lays the UI over unmoved. Without
    // one (no UI drawn this frame) the whole frame is interpolated.
    if (hudless_ready && !BbToggle::Disabled(1u << 30)) {
        dispatch.hudlessColor = Describe(vk::Image(hudless), scene.color_format, scene.width,
                                         scene.height, ColorUsage, vk::ImageAspectFlagBits::eColor,
                                         FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    } else {
        dispatch.hudlessColor.structSize = sizeof(dispatch.hudlessColor); // none: described, null
    }
    hudless_ready = false;
    dispatch.distortionField.structSize = sizeof(dispatch.distortionField); // absent: described, null
    dispatch.output = Describe(vk::Image(target->image), scene.color_format, scene.width,
                               scene.height, ColorUsage, vk::ImageAspectFlagBits::eColor,
                               FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.displaySize = {scene.width, scene.height};
    dispatch.interpolationRect = {0, 0, scene.width, scene.height};
    dispatch.frameTimeMilliseconds = scene.frame_ms;
    dispatch.cameraNear = scene.camera_near;
    dispatch.cameraFar = scene.camera_far;
    dispatch.cameraVerticalFovRadians = scene.vertical_fov;
    dispatch.viewSpaceToMeters = 1.0f;
    dispatch.minLuminance = 0.0f;
    dispatch.maxLuminance = 1.0f;
    dispatch.transferFunction = FFX_VK_PORTABLE_TRANSFER_FUNCTION_SRGB;
    dispatch.reset = reset ? VK_TRUE : VK_FALSE;
    dispatch.frameId = id;

    {
        // Checked once with a placeholder command buffer (the real one is the recording thread's).
        static bool checked = false;
        if (!checked) {
            checked = true;
            FfxVkPortableFrameGenerationCreateInfo create{};
            create.structSize = sizeof(create);
            create.maxRenderSize = {context_render_w, context_render_h};
            create.displaySize = {context_w, context_h};
            create.interpolationSourceFormat = static_cast<VkFormat>(context_format);
            create.outputFormat = static_cast<VkFormat>(context_format);
            auto d = dispatch;
            d.commandBuffer = reinterpret_cast<VkCommandBuffer>(uintptr_t{1});
            if (const u64 issues = ffxVkPortableValidateFrameGenerationDispatchInfo(&create, &d)) {
                std::printf("Frame generation: dispatch: %s\n", ffxVkPortableValidationIssueName(issues));
            }
        }
    }
    FfxVkPortableFrameGenerationPrepareInfo prepare{};
    prepare.structSize = sizeof(prepare);
    prepare.depth = Describe(vk::Image(depth_copy), scene.depth_format, scene.render_width,
                             scene.render_height, vk::ImageUsageFlagBits::eSampled,
                             vk::ImageAspectFlagBits::eDepth, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    prepare.motionVectors = Describe(scene.motion, vk::Format::eR16G16Sfloat, scene.render_width,
                                     scene.render_height,
                                     vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                                     vk::ImageAspectFlagBits::eColor,
                                     FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    prepare.renderSize = {scene.render_width, scene.render_height};
    prepare.jitterOffset = {scene.jitter_x, scene.jitter_y};
    prepare.motionVectorScale = {1.0f, 1.0f};
    prepare.frameTimeMilliseconds = scene.frame_ms;
    prepare.cameraNear = scene.camera_near;
    prepare.cameraFar = scene.camera_far;
    prepare.cameraVerticalFovRadians = scene.vertical_fov;
    prepare.viewSpaceToMeters = 1.0f;
    prepare.minLuminance = 0.0f;
    prepare.maxLuminance = 1.0f;
    prepare.transferFunction = FFX_VK_PORTABLE_TRANSFER_FUNCTION_SRGB;
    prepare.cameraPosition = {scene.camera_position[0], scene.camera_position[1], scene.camera_position[2]};
    prepare.cameraUp = {scene.camera_up[0], scene.camera_up[1], scene.camera_up[2]};
    prepare.cameraRight = {scene.camera_right[0], scene.camera_right[1], scene.camera_right[2]};
    prepare.cameraForward = {scene.camera_forward[0], scene.camera_forward[1], scene.camera_forward[2]};
    prepare.reset = reset ? VK_TRUE : VK_FALSE;
    prepare.frameId = id;
    const FfxVkPortableImage source =
        dispatch.hudlessColor.image != VK_NULL_HANDLE ? dispatch.hudlessColor : dispatch.currentColor;

    scheduler.EndRendering();
    scheduler.Record([this, prepare, source, dispatch, color, out = vk::Image(target->image),
                      target](vk::CommandBuffer cmdbuf) mutable {
        const auto all = vk::PipelineStageFlagBits2::eAllCommands;
        // The UI drew into it; the display pass reads it after.
        ImageBarrier(cmdbuf, color, all, vk::AccessFlagBits2::eMemoryWrite,
                     vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
        ImageBarrier(cmdbuf, out, all, vk::AccessFlagBits2::eMemoryRead,
                     vk::PipelineStageFlagBits2::eComputeShader,
                     vk::AccessFlagBits2::eShaderStorageWrite, vk::ImageLayout::eUndefined);
        prepare.commandBuffer = cmdbuf;
        dispatch.commandBuffer = cmdbuf;
        const auto prepared = ffxVkPortableFrameGenerationContextPrepare(context, &prepare, &source);
        const auto dispatched = prepared == FFX_VK_PORTABLE_OK
                                    ? ffxVkPortableFrameGenerationContextRecordDispatch(context, &dispatch)
                                    : prepared;
        const bool ok = dispatched == FFX_VK_PORTABLE_OK;
        if (!ok) {
            static int told = 0;
            if (told++ < 4) {
                std::printf("Frame generation: the interpolation could not be recorded (%s %d)\n",
                            prepared != FFX_VK_PORTABLE_OK ? "prepare" : "dispatch", int(dispatched));
            }
        }
        ImageBarrier(cmdbuf, color, vk::PipelineStageFlagBits2::eComputeShader,
                     vk::AccessFlagBits2::eShaderRead, all,
                     vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite);
        ImageBarrier(cmdbuf, out, vk::PipelineStageFlagBits2::eComputeShader,
                     vk::AccessFlagBits2::eShaderStorageWrite, all, vk::AccessFlagBits2::eMemoryRead);
        if (!ok) {
            // Shown anyway (it was handed out as ready): the real frame twice, not garbage.
            ImageBarrier(cmdbuf, out, all, vk::AccessFlagBits2::eMemoryRead,
                         vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
            cmdbuf.copyImage(color, vk::ImageLayout::eGeneral, out, vk::ImageLayout::eGeneral,
                             vk::ImageCopy{.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                           .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                           .extent = {target->width, target->height, 1}});
            ImageBarrier(cmdbuf, out, vk::PipelineStageFlagBits2::eTransfer,
                         vk::AccessFlagBits2::eTransferWrite, all, vk::AccessFlagBits2::eMemoryRead);
        }
    });
}

bool FrameGeneration::TakeOutput(VAddr display, Output& out) {
    if (!Wanted()) {
        return false;
    }
    std::scoped_lock lock{targets_mutex};
    (void)display;
    const auto it = targets.find(0);
    if (it == targets.end() || !it->second.ready) {
        ++stat_missed;
        return false;
    }
    ++stat_taken;
    Target& t = it->second;
    out = {vk::Image(t.image), *t.view, t.format, t.width, t.height, t.frame};
    t.ready = false;
    return true;
}

} // namespace Vulkan
