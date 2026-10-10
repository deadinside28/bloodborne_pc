// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/upscaler/native_upscale_pass.h"
#include <algorithm>
#include "bbport_settings.h"
#include "bbport_toggles.h"
#include "video_core/renderer_vulkan/upscaler/frame_generation_bridge.h"
#include "video_core/renderer_vulkan/upscaler/fsr3_bridge.h"
#include "video_core/renderer_vulkan/upscaler/fsr4_bridge.h"
#include "video_core/renderer_vulkan/upscaler/merge_pass.h"
#include "video_core/renderer_vulkan/upscaler/reactive_mask_pass.h"
#include "video_core/renderer_vulkan/upscaler/taa_pass.h"
#include "video_core/renderer_vulkan/upscaler/ui_composition_pass.h"
#include "video_core/renderer_vulkan/upscaler/upscaler_diagnostics.h"
#include "video_core/renderer_vulkan/upscaler/view_cache.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

bool ExecuteNativeUpscale(NativeUpscaleContext& ctx) {
    if (auto* profiler = GpuProfiler::Get()) {
        const char* label = BbSettings::Get().upscaler == BbSettings::UpscalerTaa
                                ? "upscaler Run (TAA)" : "upscaler Run (FSR)";
        profiler->Mark(0xF5A0'0000ull ^ std::hash<std::string_view>{}(label),
                       [label] { return std::string{label}; });
    }
    if (!ctx.scene_color || !ctx.camera_motion.Depth()) return false;
    if (!ctx.texture_cache.HasImage(ctx.scene_color) || !ctx.texture_cache.HasImage(ctx.camera_motion.Depth())) return false;
    auto& color = ctx.texture_cache.GetImage(ctx.scene_color);
    auto& depth = ctx.texture_cache.GetImage(ctx.camera_motion.Depth());
    const u32 ow = color.info.size.width, oh = color.info.size.height;
    const bool reduced = ctx.scene_targets.Reduced() &&
                         ctx.scene_targets.EligibleScene(color) &&
                         ctx.scene_targets.EligibleScene(depth);
    const u32 w = reduced ? ctx.scene_targets.Size().width : ow;
    const u32 h = reduced ? ctx.scene_targets.Size().height : oh;
    if (color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat ||
        depth.info.size.width != ow || depth.info.size.height != oh ||
        !(color.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
        return false;
    }

    const auto depth_format = depth.info.pixel_format;
    const auto depth_view = ctx.view_cache.Get(depth, depth_format, vk::ImageAspectFlagBits::eDepth);
    const auto color_view = ctx.view_cache.Get(color, vk::Format::eR16G16B16A16Sfloat, vk::ImageAspectFlagBits::eColor);
    ctx.scheduler.EndRendering();
    ctx.runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
    ctx.runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
    ctx.runtime.FlushBarriers();

    vk::Image input_color = color.GetImage(), input_depth = depth.GetImage();
    vk::ImageView input_color_view = color_view, input_depth_view = depth_view;
    if (reduced) {
        VideoCore::ImageViewInfo ci; ci.format = color.info.pixel_format;
        const auto c = ctx.scene_targets.Read(ctx.scene_color, ci);
        ci.format = depth.info.pixel_format;
        const auto d = ctx.scene_targets.Read(ctx.camera_motion.Depth(), ci);
        input_color = c.image; input_depth = d.image; input_color_view = c.view; input_depth_view = d.view;
    }
    const auto cmdbuf = ctx.scheduler.CommandBuffer();
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    const auto barrier = [&](vk::Image img, vk::ImageLayout old_l, vk::PipelineStageFlags2 src_s,
                             vk::AccessFlags2 src_a, vk::ImageLayout new_l,
                             vk::PipelineStageFlags2 dst_s, vk::AccessFlags2 dst_a) {
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = src_s, .srcAccessMask = src_a, .dstStageMask = dst_s, .dstAccessMask = dst_a,
            .oldLayout = old_l, .newLayout = new_l, .image = img,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
    };
    barrier(ctx.motion_image, vk::ImageLayout::eUndefined, all, vk::AccessFlagBits2::eNone,
            vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite);
    barrier(ctx.output_image, vk::ImageLayout::eUndefined, all, vk::AccessFlagBits2::eNone,
            vk::ImageLayout::eGeneral, all, rw);

    ctx.camera_motion.RecordMotion(input_depth_view, ctx.motion_view, w, h);
    barrier(ctx.motion_image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
            vk::AccessFlagBits2::eShaderWrite, vk::ImageLayout::eGeneral, all, vk::AccessFlagBits2::eShaderRead);

    const bool has_reactive = ctx.reactive_mask_pass.IsMaskReady() || ctx.reactive_mask_pass.Record(input_color_view, w, h);
    const auto now = std::chrono::steady_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - ctx.last_frame).count();
    if (frame_ms <= 0.0f || frame_ms > 200.0f) frame_ms = 16.6f;
    ctx.last_frame = now;

    bool dispatched = false;
    if (BbSettings::Get().upscaler == BbSettings::UpscalerTaa) {
        ctx.taa_pass.Record(cmdbuf, input_color_view, input_depth_view, ctx.motion_view,
                            ctx.output_view, ctx.reactive_mask_pass.OpaqueView(), ow, oh, ctx.jitter,
                            ctx.reset, ctx.camera_motion.TaaDepthParameters());
        dispatched = true;
    } else if (ctx.fsr4_bridge.IsActive()) {
        dispatched = ctx.fsr4_bridge.Record(cmdbuf, {input_color, input_color_view, w, h},
                                            {input_depth, input_depth_view, w, h},
                                            ctx.motion_image, ctx.motion_view,
                                            ctx.output_image, ctx.output_view,
                                            w, h, ow, oh, ctx.applied_preset, ctx.jitter, frame_ms,
                                            ctx.camera_motion.Near(), ctx.camera_motion.VerticalFov(), ctx.reset);
        if (dispatched && has_reactive) {
            ctx.fsr4_bridge.RecordReactive(cmdbuf, input_color_view, ctx.reactive_mask_pass.ReactiveView(),
                                           ctx.output_view, w, h, ow, oh, ctx.jitter);
        }
    } else {
        const float sign = BbToggle::Disabled(1u << 26) ? -1.0f : 1.0f;
        Fsr3Bridge::DispatchInputs in{
            .cmdbuf = cmdbuf,
            .color_image = input_color,
            .color_format = color.info.pixel_format,
            .color_usage = color.usage_flags,
            .depth_image = input_depth,
            .depth_format = depth_format,
            .depth_usage = depth.usage_flags,
            .motion_image = ctx.motion_image,
            .output_image = ctx.output_image,
            .output_format = vk::Format::eR16G16B16A16Sfloat,
            .output_usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                            vk::ImageUsageFlagBits::eTransferSrc,
            .reactive_image = has_reactive ? ctx.reactive_mask_pass.ReactiveImage() : vk::Image{},
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
        dispatched = ctx.fsr3_bridge.RecordDispatch(in);
    }

    if (dispatched) {
        ctx.reset = false;
        ctx.dispatched_last_frame = true;
        if (BbSettings::Get().upscaler != BbSettings::UpscalerTaa) {
            ctx.taa_pass.RecordExtraSharpen(ctx.output_image, ctx.output_view,
                                           ctx.ui_pass.GetUiStorageView(), false, ow, oh);
        }
        if (const int dump = ctx.diagnostics.CheckFrameDump(); dump >= 0) {
            ctx.camera_motion.PrintState(dump);
            const vk::Format format = color.info.pixel_format;
            const bool rgba16f = format == vk::Format::eR16G16B16A16Sfloat;
            ctx.diagnostics.DumpImages(cmdbuf, dump, {
                {input_color, w, h, rgba16f ? 8u : 4u, "input", rgba16f ? "rgba16f" : "r11g11b10f"},
                {ctx.motion_image, w, h, 4, "motion", "rg16f"},
                {ctx.output_image, ow, oh, 8, "output", "rgba16f"},
                {input_depth, w, h, 4, "depth", "f32", vk::ImageAspectFlagBits::eDepth},
            });
        }
        barrier(ctx.output_image, vk::ImageLayout::eGeneral, all, rw,
                vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
        ctx.runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
        ctx.runtime.FlushBarriers();

        const int debug_view = BbSettings::Get().debug_view;
        const u32 mode = has_reactive && (debug_view == BbSettings::DebugReactive || BbToggle::Disabled(1u << 28)) ? 1
                         : debug_view == BbSettings::DebugMotion ? 2 : 0;
        ctx.merge_pass.Record(cmdbuf, ctx.output_view, color_view, ctx.reactive_mask_pass.ReactiveView(),
                              ctx.motion_view, ctx.camera_motion.ObjectMotionView(), ow, oh, mode);

        if (BbSettings::Get().frame_generation.load()) {
            ctx.fg_bridge.Prepare(cmdbuf, ctx.output_image, w, h, ow, oh, input_depth,
                                  input_depth_view, depth_format, ctx.motion_image,
                                  ctx.motion_view, ctx.camera_motion, frame_ms, ctx.reset,
                                  BbStats::frame_number.load());
        }
    }
    return dispatched;
}

} // namespace Vulkan
