// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cassert>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_frame_generation.h"
#include "video_core/renderer_vulkan/upscaler/frame_generation_helpers.h"
#include "bbport_settings.h"

int main() {
    std::printf("Testing FSR 3.1 Frame Generation manager...\n");
    BbSettings::Get().frame_generation.store(true);

    Vulkan::Instance instance(0, false);
    Vulkan::Scheduler scheduler(instance);
    const auto device = instance.GetDevice();
    const auto allocator = instance.GetAllocator();

    Vulkan::FrameGenerationManager fg(instance, scheduler);
    std::printf("FrameGen Supported: %d (Problem: %s)\n", fg.IsSupported(),
                fg.GetProblem() ? fg.GetProblem() : "none");
    if (!fg.IsSupported()) {
        std::printf("Frame Generation not supported on this GPU, test skipped.\n");
        return 0;
    }

    const u32 W = 1920, H = 1080;
    const vk::Format disp_format = vk::Format::eB8G8R8A8Unorm;

    const bool init_ok = fg.Initialize(W, H, W, H, disp_format, disp_format);
    assert(init_ok);
    assert(fg.IsActive());

    VideoCore::UniqueImage depth_img(device, allocator);
    depth_img.Create({
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eD32Sfloat,
        .extent = {W, H, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eDepthStencilAttachment,
        .initialLayout = vk::ImageLayout::eUndefined,
    });

    VideoCore::UniqueImage motion_img(device, allocator);
    motion_img.Create({
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16Sfloat,
        .extent = {W, H, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined,
    });

    VideoCore::UniqueImage current_color(device, allocator);
    current_color.Create({
        .imageType = vk::ImageType::e2D,
        .format = disp_format,
        .extent = {W, H, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined,
    });

    for (int frame = 0; frame < 4; ++frame) {
        auto cmdbuf = scheduler.CommandBuffer();

        Vulkan::FrameGenerationManager::PrepareInputs prep{};
        prep.cmdbuf = cmdbuf;
        prep.depth_image = vk::Image(depth_img);
        prep.depth_format = vk::Format::eD32Sfloat;
        prep.motion_image = vk::Image(motion_img);
        prep.interpolation_source_image = fg.GetSourceImage();
        prep.interpolation_source_view = fg.GetSourceView();
        prep.render_w = W;
        prep.render_h = H;
        prep.frame_time_ms = 16.6f;
        prep.near_plane = 0.1f;
        prep.far_plane = 3000.0f;
        prep.fov_radians = 0.785f;
        prep.camera_up[1] = 1.0f;
        prep.camera_forward[2] = 1.0f;
        prep.reset = (frame == 0);
        prep.frame_id = frame;

        const bool prep_ok = fg.RecordPrepare(prep);
        assert(prep_ok);

        Vulkan::FrameGenerationManager::DispatchInputs disp{};
        disp.cmdbuf = cmdbuf;
        disp.current_color_image = vk::Image(current_color);
        disp.hudless_color_image = (frame % 2 == 0) ? fg.GetSourceImage() : vk::Image{};
        disp.output_image = fg.GetInterpolatedImage();
        disp.display_w = W;
        disp.display_h = H;
        disp.frame_time_ms = 16.6f;
        disp.near_plane = 0.1f;
        disp.far_plane = 3000.0f;
        disp.fov_radians = 0.785f;
        disp.reset = (frame == 0);
        disp.frame_id = frame;

        const bool disp_ok = fg.RecordDispatch(disp);
        assert(disp_ok);

        if (frame == 0) {
            assert(!fg.CanPresentInterpolated());
        } else {
            assert(fg.CanPresentInterpolated());
        }

        Vulkan::SubmitInfo submit{};
        scheduler.Flush(submit);
        scheduler.Finish();

        fg.ClearInterpolatedFrame();
        assert(!fg.CanPresentInterpolated());
    }

    fg.Destroy();
    assert(!fg.IsActive());

    std::puts("Frame Generation: context, prepare, dispatch, interpolation PASS");
    return 0;
}
