// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/upscaler/frame_generation_bridge.h"
#include <cstdio>
#include "bbport_settings.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"

namespace Vulkan {

FrameGenerationBridge::FrameGenerationBridge(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    frame_generation = std::make_unique<FrameGenerationManager>(instance, scheduler);
}

FrameGenerationBridge::~FrameGenerationBridge() = default;

void FrameGenerationBridge::Destroy() {
    if (frame_generation) {
        frame_generation->Destroy();
    }
    is_prepared = false;
}

void FrameGenerationBridge::SetDisplayInfo(u32 display_w, u32 display_h, vk::Format display_format) {
    if (display_w > 0 && display_h > 0) {
        disp_w = display_w;
        disp_h = display_h;
    }
    if (display_format != vk::Format::eUndefined) {
        disp_format = display_format;
    }
}

bool FrameGenerationBridge::Prepare(vk::CommandBuffer cmdbuf, vk::Image scene_image,
                                    u32 render_w, u32 render_h, u32 out_w, u32 out_h,
                                    vk::Image depth_image, vk::ImageView depth_view,
                                    vk::Format depth_format, vk::Image motion_image,
                                    vk::ImageView motion_view, CameraMotion& camera_motion,
                                    float frame_ms, bool reset, u64 frame_id) {
    if (!frame_generation || !BbSettings::Get().frame_generation.load()) {
        is_prepared = false;
        return false;
    }

    const u32 max_rw = std::min(std::max(render_w, 1u), disp_w);
    const u32 max_rh = std::min(std::max(render_h, 1u), disp_h);
    if (!frame_generation->Initialize(max_rw, max_rh, disp_w, disp_h, disp_format, disp_format)) {
        is_prepared = false;
        return false;
    }

    const vk::Image hudless = frame_generation->GetSourceImage();
    if (!hudless || !scene_image) {
        is_prepared = false;
        return false;
    }

    // Prepare hudless_source_image by blitting the upscaled/native scene without UI
    const std::array pre_blit_barriers{
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eBlit,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .image = scene_image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        },
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eBlit,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = hudless,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        },
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = u32(pre_blit_barriers.size()),
        .pImageMemoryBarriers = pre_blit_barriers.data(),
    });

    const vk::ImageBlit blit_region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .srcOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{s32(out_w), s32(out_h), 1}},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{s32(disp_w), s32(disp_h), 1}},
    };
    cmdbuf.blitImage(scene_image, vk::ImageLayout::eTransferSrcOptimal,
                     hudless, vk::ImageLayout::eTransferDstOptimal,
                     blit_region, vk::Filter::eLinear);

    const std::array post_blit_barriers{
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eBlit,
            .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
            .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = scene_image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        },
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eBlit,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = hudless,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        },
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = u32(post_blit_barriers.size()),
        .pImageMemoryBarriers = post_blit_barriers.data(),
    });

    FrameGenerationManager::PrepareInputs prep{};
    prep.cmdbuf = cmdbuf;
    prep.depth_image = depth_image;
    prep.depth_view = depth_view;
    prep.depth_format = depth_format;
    prep.motion_image = motion_image;
    prep.motion_view = motion_view;
    prep.interpolation_source_image = hudless;
    prep.interpolation_source_view = frame_generation->GetSourceView();
    prep.render_w = render_w;
    prep.render_h = render_h;
    camera_motion.GetJitter(prep.jitter_x, prep.jitter_y);
    prep.frame_time_ms = frame_ms;
    prep.near_plane = camera_motion.Near();
    prep.far_plane = camera_motion.Far();
    prep.fov_radians = camera_motion.VerticalFov();
    camera_motion.GetCameraVectors(prep.camera_pos, prep.camera_up,
                                   prep.camera_right, prep.camera_forward);
    prep.reset = reset;
    prep.frame_id = frame_id;
    last_frame_id = frame_id;

    is_prepared = frame_generation->RecordPrepare(prep);
    return is_prepared;
}

bool FrameGenerationBridge::RecordDispatch(vk::CommandBuffer cmdbuf, vk::Image current_color,
                                           u32 display_w, u32 display_h, float frame_ms,
                                           CameraMotion& camera_motion, bool reset) {
    if (!frame_generation || !frame_generation->IsActive() || !is_prepared) {
        return false;
    }

    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const vk::Image output = frame_generation->GetInterpolatedImage();

    const vk::ImageMemoryBarrier2 pre_barrier{
        .srcStageMask = all,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = output,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    FrameGenerationManager::DispatchInputs disp{};
    disp.cmdbuf = cmdbuf;
    disp.current_color_image = current_color;
    disp.hudless_color_image = frame_generation->GetSourceImage();
    disp.output_image = output;
    disp.display_w = display_w;
    disp.display_h = display_h;
    disp.frame_time_ms = frame_ms;
    disp.near_plane = camera_motion.Near();
    disp.far_plane = camera_motion.Far();
    disp.fov_radians = camera_motion.VerticalFov();
    disp.reset = reset;
    disp.frame_id = last_frame_id;

    const bool ok = frame_generation->RecordDispatch(disp);
    is_prepared = false;

    const vk::ImageMemoryBarrier2 post_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = all,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = output,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    return ok;
}

} // namespace Vulkan
