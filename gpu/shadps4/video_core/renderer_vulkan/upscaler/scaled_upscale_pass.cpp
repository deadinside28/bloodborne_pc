// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/upscaler/scaled_upscale_pass.h"
#include <algorithm>
#include "bbport_settings.h"
#include "bbport_toggles.h"
#include "video_core/renderer_vulkan/upscaler/frame_generation_bridge.h"
#include "video_core/renderer_vulkan/upscaler/fsr3_bridge.h"
#include "video_core/renderer_vulkan/upscaler/fsr4_bridge.h"
#include "video_core/renderer_vulkan/upscaler/taa_pass.h"
#include "video_core/renderer_vulkan/upscaler/ui_composition_pass.h"
#include "video_core/renderer_vulkan/upscaler/view_cache.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

bool ExecuteScaledUpscale(ScaledUpscaleContext& ctx) {
    if (auto* profiler = GpuProfiler::Get()) {
        const char* label = BbSettings::Get().upscaler == BbSettings::UpscalerTaa
                                ? "upscaler RunScaled (TAA)" : "upscaler RunScaled (FSR)";
        profiler->Mark(0xF5A0'0000ull ^ std::hash<std::string_view>{}(label),
                       [label] { return std::string{label}; });
    }
    const auto ldr = ctx.ui_pass.GetLdrTarget();
    if (!ldr || !ctx.camera_motion.Depth()) return false;
    if (!ctx.texture_cache.HasImage(ldr) || !ctx.texture_cache.HasImage(ctx.camera_motion.Depth())) return false;
    auto& color = ctx.texture_cache.GetImage(ldr);
    auto& depth = ctx.texture_cache.GetImage(ctx.camera_motion.Depth());
    const u32 iw = color.info.size.width, ih = color.info.size.height;
    auto cam_size = ctx.camera_motion.RenderSize();
    if (cam_size[0] == 0 || cam_size[1] == 0) cam_size = {ctx.render_width, ctx.render_height};
    const u32 w = ctx.scaled_session ? std::min(cam_size[0], iw) : ctx.render_width;
    const u32 h = ctx.scaled_session ? std::min(cam_size[1], ih) : ctx.render_height;
    const u32 ow = ctx.target_width, oh = ctx.target_height;
    if (depth.info.size.width != iw || depth.info.size.height != ih || w > ow || h > oh) return false;

    ctx.ui_pass.EnsureResources(ow, oh, color.info.pixel_format, depth.info.pixel_format,
                                ctx.render_width, ctx.render_height);
    ctx.ui_pass.PrepareDepth(ctx.camera_motion.Depth(), ctx.texture_cache, ctx.runtime);
    const auto depth_format = depth.info.pixel_format;
    vk::Image depth_image = depth.GetImage(), color_image = color.GetImage();
    vk::ImageView depth_view{}, color_view{};
    u32 source_width = iw, source_height = ih;
    if (!ctx.scaled_session) {
        VideoCore::ImageViewInfo ci, di; ci.format = color.info.pixel_format; di.format = depth_format;
        const auto cp = ctx.scene_targets.Read(ldr, ci);
        const auto dp = ctx.scene_targets.Read(ctx.camera_motion.Depth(), di);
        color_image = cp.image; color_view = cp.view; depth_image = dp.image; depth_view = dp.view;
        source_width = w; source_height = h;
    } else {
        depth_view = ctx.view_cache.Get(depth, depth_format, vk::ImageAspectFlagBits::eDepth);
        color_view = ctx.view_cache.Get(color, color.info.pixel_format, vk::ImageAspectFlagBits::eColor);
        ctx.runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
        ctx.runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
        ctx.runtime.FlushBarriers();
    }

    ctx.scheduler.EndRendering();
    const auto barrier = [&](vk::Image img, vk::ImageAspectFlags aspect, vk::ImageLayout old_l,
                             vk::PipelineStageFlags2 src_s, vk::AccessFlags2 src_a,
                             vk::ImageLayout new_l, vk::PipelineStageFlags2 dst_s, vk::AccessFlags2 dst_a) {
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = src_s, .srcAccessMask = src_a, .dstStageMask = dst_s, .dstAccessMask = dst_a,
            .oldLayout = old_l, .newLayout = new_l, .image = img,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        };
        ctx.scheduler.Record([b](vk::CommandBuffer cmdbuf) {
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
        });
    };
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto color_access = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
    const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    barrier(ctx.ui_pass.GetUiImage(), vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined, all,
            vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral, all, rw);
    barrier(ctx.motion_image, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined, all,
            vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite);
    ctx.camera_motion.RecordMotion(depth_view, ctx.motion_view, w, h);
    barrier(ctx.motion_image, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eGeneral,
            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite, vk::ImageLayout::eGeneral, all, vk::AccessFlagBits2::eShaderRead);

    const auto now = std::chrono::steady_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - ctx.last_frame).count();
    if (frame_ms <= 0.0f || frame_ms > 200.0f) frame_ms = 16.6f;
    ctx.last_frame = now;

    if (ctx.fsr4_bridge.IsActive() || BbSettings::Get().upscaler == BbSettings::UpscalerTaa) {
        const auto cmdbuf = ctx.scheduler.CommandBuffer();
        barrier(ctx.output_image, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined, all,
                vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral, all, rw);
        bool ok = true;
        if (BbSettings::Get().upscaler == BbSettings::UpscalerTaa) {
            ctx.taa_pass.Record(cmdbuf, color_view, depth_view, ctx.motion_view,
                                ctx.output_view, {}, ow, oh, ctx.jitter, ctx.reset,
                                ctx.camera_motion.TaaDepthParameters());
        } else {
            Fsr4Upscaler::Image input{color_image, color_view, source_width, source_height};
            ctx.fsr4_bridge.RecordDecode(cmdbuf, color_view, source_width, source_height, input);
            ok = ctx.fsr4_bridge.Record(cmdbuf, input, {depth_image, depth_view, source_width, source_height},
                                        ctx.motion_image, ctx.motion_view, ctx.output_image,
                                        ctx.output_view, w, h, ow, oh, ctx.applied_preset, ctx.jitter, frame_ms,
                                        ctx.camera_motion.Near(), ctx.camera_motion.VerticalFov(), ctx.reset);
            if (ok && ctx.fsr4_bridge.IsLinearFrame()) {
                ctx.fsr4_bridge.RecordEncode(cmdbuf, ctx.output_view, ow, oh);
            }
        }
        if (ok) {
            const vk::ImageBlit region{
                .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .srcOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{s32(ow), s32(oh), 1}},
                .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .dstOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{s32(ow), s32(oh), 1}},
            };
            cmdbuf.blitImage(ctx.output_image, vk::ImageLayout::eGeneral, ctx.ui_pass.GetUiImage(),
                             vk::ImageLayout::eGeneral, region, vk::Filter::eNearest);
            ctx.taa_pass.RecordExtraSharpen(ctx.ui_pass.GetUiImage(), ctx.ui_pass.GetUiView(),
                                           ctx.ui_pass.GetUiStorageView(), true, ow, oh);
            if (BbSettings::Get().frame_generation.load()) {
                ctx.fg_bridge.Prepare(cmdbuf, ctx.ui_pass.GetUiImage(), w, h, ow, oh, depth_image,
                                      depth_view, depth_format, ctx.motion_image, ctx.motion_view,
                                      ctx.camera_motion, frame_ms, ctx.reset,
                                      BbStats::frame_number.load());
            }
        }
        if (ok) {
            ctx.ui_pass.SetUiPhase(true);
            ctx.ui_pass.SetUiColor(ldr);
            ctx.ui_pass.SetUiDepth(ctx.camera_motion.Depth());
        }
        return ok;
    }

    const float sign = BbToggle::Disabled(1u << 26) ? -1.0f : 1.0f;
    Fsr3Bridge::DispatchInputs in{
        .color_image = color_image,
        .color_format = color.info.pixel_format,
        .color_usage = color.usage_flags,
        .color_width = source_width,
        .color_height = source_height,
        .depth_image = depth_image,
        .depth_format = depth_format,
        .depth_usage = depth.usage_flags,
        .depth_width = source_width,
        .depth_height = source_height,
        .motion_image = ctx.motion_image,
        .output_image = ctx.ui_pass.GetUiImage(),
        .output_format = vk::Format::eR8G8B8A8Unorm,
        .output_usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                        vk::ImageUsageFlagBits::eColorAttachment,
        .render_w = w,
        .render_h = h,
        .output_w = ow,
        .output_h = oh,
        .jitter_x = sign * ctx.jitter[0],
        .jitter_y = sign * ctx.jitter[1],
        .frame_ms = frame_ms,
        .near_plane = ctx.camera_motion.Near(),
        .far_plane = 3000.0f,
        .fov_radians = ctx.camera_motion.VerticalFov(),
        .sharpness = std::min(BbSettings::Get().sharpness.load(), 1.0f),
        .sharpen = BbSettings::Get().sharpen,
        .reset = ctx.reset,
        .frame_id = ctx.frame_id++,
    };
    bool ok = ctx.fsr3_bridge.RecordDispatchAsync(in, ctx.dispatch_failed);
    if (ok) {
        ctx.reset = false;
        ctx.dispatched_last_frame = true;
        ctx.taa_pass.RecordExtraSharpen(ctx.ui_pass.GetUiImage(), ctx.ui_pass.GetUiView(),
                                       ctx.ui_pass.GetUiStorageView(), true, ow, oh);
        if (BbSettings::Get().frame_generation.load()) {
            ctx.fg_bridge.Prepare(ctx.scheduler.CommandBuffer(), ctx.ui_pass.GetUiImage(), w, h, ow,
                                  oh, depth_image, depth_view, depth_format, ctx.motion_image,
                                  ctx.motion_view, ctx.camera_motion, frame_ms, ctx.reset,
                                  BbStats::frame_number.load());
        }
    }
    barrier(ctx.ui_pass.GetUiImage(), vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eGeneral, all,
            rw, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eColorAttachmentOutput, color_access);
    if (ok) {
        ctx.ui_pass.SetUiPhase(true);
        ctx.ui_pass.SetUiColor(ldr);
        ctx.ui_pass.SetUiDepth(ctx.camera_motion.Depth());
    }
    return ok;
}

} // namespace Vulkan
