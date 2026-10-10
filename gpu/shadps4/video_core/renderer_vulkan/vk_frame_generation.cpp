// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_frame_generation.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "bbport_settings.h"

#include <cstdio>
#include <algorithm>
#include "ffx_vk_portable.h"

#include "video_core/renderer_vulkan/upscaler/frame_generation_helpers.h"

namespace Vulkan {

FrameGenerationManager::FrameGenerationManager(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    FfxVkPortableDeviceCapabilities caps{};
    caps.structSize = sizeof(caps);
    if (ffxVkPortableQueryDeviceCapabilities(instance.GetPhysicalDevice(), &caps) == FFX_VK_PORTABLE_OK) {
        supported = (caps.fsr3FrameGenerationPrerequisites == VK_TRUE);
        BbSettings::Get().frame_generation_supported.store(supported);
        if (!supported) {
            problem = "GPU does not meet FSR 3 Frame Generation prerequisites";
            BbSettings::Get().frame_generation_problem.store(problem);
            std::printf("FrameGen: %s\n", problem);
        } else {
            std::printf("FrameGen: FSR 3.1 Frame Generation prerequisites met on this GPU\n");
        }
    } else {
        supported = false;
        problem = "Querying GPU capabilities for Frame Generation failed";
        BbSettings::Get().frame_generation_supported.store(false);
        BbSettings::Get().frame_generation_problem.store(problem);
        std::printf("FrameGen: %s\n", problem);
    }
}

FrameGenerationManager::~FrameGenerationManager() {
    Destroy();
}

bool FrameGenerationManager::IsActive() const noexcept {
    return supported && !failed && context != nullptr && BbSettings::Get().frame_generation.load();
}

void FrameGenerationManager::Reset() {
    needs_reset = true;
}

bool FrameGenerationManager::Initialize(u32 max_render_w, u32 max_render_h, u32 display_w, u32 display_h,
                                        vk::Format source_format, vk::Format output_format) {
    if (!supported) {
        return false;
    }

    if (context && current_max_render_w == max_render_w && current_max_render_h == max_render_h &&
        current_display_w == display_w && current_display_h == display_h &&
        current_source_format == source_format && current_output_format == output_format) {
        return true;
    }

    Destroy();

    const auto device = instance.GetDevice();
    FfxVkPortableDeviceInfo device_info{};
    device_info.structSize = sizeof(device_info);
    device_info.physicalDevice = instance.GetPhysicalDevice();
    device_info.device = device;
    device_info.getDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    device_info.queue = instance.GetGraphicsQueue();
    device_info.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex();
    device_info.shaderFloat16Enabled = instance.IsShaderFloat16Enabled();
    device_info.subgroupSizeControlEnabled = instance.IsSubgroupSizeControlEnabled();
    device_info.synchronization2Enabled = VK_TRUE;
    device_info.shaderStorageImageWriteWithoutFormatEnabled = VK_TRUE;

    create_info = std::make_unique<FfxVkPortableFrameGenerationCreateInfo>();
    create_info->structSize = sizeof(FfxVkPortableFrameGenerationCreateInfo);
    create_info->flags = 0;
    create_info->maxRenderSize = {max_render_w, max_render_h};
    create_info->displaySize = {display_w, display_h};
    create_info->interpolationSourceFormat = static_cast<VkFormat>(source_format);
    create_info->outputFormat = static_cast<VkFormat>(output_format);

    if (const u64 issues = ffxVkPortableValidateFrameGenerationCreateInfo(create_info.get())) {
        problem = "Invalid FSR 3 Frame Generation create info";
        BbSettings::Get().frame_generation_problem.store(problem);
        failed = true;
        std::printf("FrameGen: Validation failed on create info (issues: 0x%llx: %s)\n",
                    static_cast<unsigned long long>(issues),
                    ffxVkPortableValidationIssueName(issues));
        return false;
    }

    if (ffxVkPortableFrameGenerationContextCreate(&device_info, create_info.get(), &context) != FFX_VK_PORTABLE_OK) {
        problem = "Failed to create FSR 3 Frame Generation context";
        BbSettings::Get().frame_generation_problem.store(problem);
        failed = true;
        context = nullptr;
        std::printf("FrameGen: Context creation failed\n");
        return false;
    }

    const auto allocator = instance.GetAllocator();
    const auto make_res = [&](VideoCore::UniqueImage& img, vk::UniqueImageView& v, vk::Format fmt,
                              vk::ImageUsageFlags usage) {
        v.reset();
        img = VideoCore::UniqueImage(device, allocator);
        img.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = fmt,
            .extent = {display_w, display_h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = usage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        v = Check(device.createImageViewUnique({
            .image = vk::Image(img),
            .viewType = vk::ImageViewType::e2D,
            .format = fmt,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
    };
    make_res(interpolated_image, interpolated_view, output_format,
             vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst);
    make_res(hudless_source_image, hudless_source_view, source_format,
             vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc);

    current_max_render_w = max_render_w;
    current_max_render_h = max_render_h;
    current_display_w = display_w;
    current_display_h = display_h;
    current_source_format = source_format;
    current_output_format = output_format;
    failed = false;
    needs_reset = true;
    prepared = false;
    problem = nullptr;
    BbSettings::Get().frame_generation_problem.store(nullptr);
    std::printf("FrameGen: Initialized context (%ux%u -> display %ux%u)\n",
                max_render_w, max_render_h, display_w, display_h);
    return true;
}

void FrameGenerationManager::Destroy() {
    if (context) {
        scheduler.Finish();
        ffxVkPortableFrameGenerationContextDestroy(context);
        context = nullptr;
    }
    create_info.reset();
    interpolated_view.reset();
    interpolated_image = VideoCore::UniqueImage{};
    hudless_source_view.reset();
    hudless_source_image = VideoCore::UniqueImage{};
    prepared = false;
    has_interpolated_frame.store(false, std::memory_order_release);
}

bool FrameGenerationManager::RecordPrepare(const PrepareInputs& in) {
    if (!context || failed) {
        prepared = false;
        return false;
    }

    const vk::Image source = in.interpolation_source_image ? in.interpolation_source_image
                                                           : vk::Image(hudless_source_image);
    const auto prep = BuildPrepareInfo(in, needs_reset || in.reset);
    FfxVkPortableImage interp_source = MakeFgImage(
        source, current_source_format, current_display_w, current_display_h,
        vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc,
        vk::ImageAspectFlagBits::eColor,
        FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);

    if (create_info) {
        if (const u64 issues = ffxVkPortableValidateFrameGenerationPrepareInfo(create_info.get(), &prep)) {
            std::printf("FrameGen: Prepare validation failed (issues: 0x%llx: %s)\n",
                        static_cast<unsigned long long>(issues),
                        ffxVkPortableValidationIssueName(issues));
            prepared = false;
            return false;
        }
    }

    const auto res = ffxVkPortableFrameGenerationContextPrepare(context, &prep, &interp_source);
    if (res != FFX_VK_PORTABLE_OK) {
        std::printf("FrameGen: Prepare dispatch failed (%d)\n", static_cast<int>(res));
        prepared = false;
        return false;
    }
    prepared = true;
    return true;
}

bool FrameGenerationManager::RecordDispatch(const DispatchInputs& in) {
    if (!context || failed || !prepared) {
        has_interpolated_frame.store(false, std::memory_order_release);
        return false;
    }

    DispatchInputs adjusted = in;
    if (!adjusted.hudless_color_image && hudless_source_image) {
        adjusted.hudless_color_image = vk::Image(hudless_source_image);
    }

    const bool is_reset = needs_reset || in.reset;
    const auto disp = BuildDispatchInfo(adjusted, current_source_format, current_output_format,
                                        current_display_w, current_display_h, is_reset);

    if (create_info) {
        if (const u64 issues = ffxVkPortableValidateFrameGenerationDispatchInfo(create_info.get(), &disp)) {
            std::printf("FrameGen: Dispatch validation failed (issues: 0x%llx: %s)\n",
                        static_cast<unsigned long long>(issues),
                        ffxVkPortableValidationIssueName(issues));
            has_interpolated_frame.store(false, std::memory_order_release);
            prepared = false;
            return false;
        }
    }

    const auto res = ffxVkPortableFrameGenerationContextRecordDispatch(context, &disp);
    if (res != FFX_VK_PORTABLE_OK) {
        std::printf("FrameGen: RecordDispatch failed (%d)\n", static_cast<int>(res));
        has_interpolated_frame.store(false, std::memory_order_release);
        prepared = false;
        return false;
    }
    prepared = false;
    has_interpolated_frame.store(!is_reset, std::memory_order_release);
    needs_reset = false;
    return true;
}

} // namespace Vulkan
