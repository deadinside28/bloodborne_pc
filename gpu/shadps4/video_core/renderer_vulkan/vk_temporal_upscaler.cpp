// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "video_core/renderer_vulkan/upscaler/native_upscale_pass.h"
#include "video_core/renderer_vulkan/upscaler/scaled_upscale_pass.h"
#include "video_core/renderer_vulkan/motion_history.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"
#include "bbport_settings.h"
#include "bbport_timeline.h"
#include "bbport_toggles.h"

namespace Vulkan {

TemporalUpscaler::TemporalUpscaler(const Instance& instance_, Scheduler& scheduler_,
                                   VideoCore::TextureCache& texture_cache_, Runtime& runtime_,
                                   CameraMotion& camera_motion_, SceneTargets& scene_targets_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_},
      runtime{runtime_}, camera_motion{camera_motion_}, scene_targets{scene_targets_},
      taa_pass{instance_, scheduler_}, reactive_mask_pass{instance_, scheduler_},
      fsr4_bridge{instance_, scheduler_}, ui_pass{instance_, scheduler_},
      diagnostics{instance_, scheduler_}, fg_bridge{instance_, scheduler_},
      fsr3_bridge{instance_, scheduler_}, merge_pass{instance_, scheduler_},
      view_cache{instance_, scheduler_} {
    BbSettings::ConfigureUpscalerSupport(instance.IsFsr4Int8Supported(),
                                         instance.IsFsr411Supported(),
                                         instance.IsFsr411Fp8Supported(),
                                         instance.IsFsr411MatrixSupported());
    {
        const Dlss* dlss = Dlss::Get();
        static std::string problem;
        problem = !dlss ? "the DLSS bridge and NVIDIA's DLSS library are not installed"
                        : dlss->Problem();
        BbSettings::ConfigureDlssSupport(dlss && dlss->Available(), problem.c_str());
    }
    enabled = BbSettings::Get().upscaler != BbSettings::UpscalerOff;
    if (const char* env = std::getenv("BB_RENDER_RES")) {
        u32 w = 0, h = 0;
        if (std::sscanf(env, "%ux%u", &w, &h) == 2 && w >= 320 && h >= 240) {
            scaled_session = true;
            render_width = w;
            render_height = h;
        }
    }
    if (enabled && !instance.IsStorageImageWriteWithoutFormatEnabled()) {
        std::printf("Upscaler: shaderStorageImageWriteWithoutFormat unsupported, FSR 3 off\n");
        enabled = false;
    }
    if (enabled) {
        std::printf("Upscaler: FSR 3.1 available (FSR-Vulkan) on scene color before post\n");
    }
}

TemporalUpscaler::~TemporalUpscaler() {
    scheduler.Finish();
    fsr3_bridge.Destroy();
    merge_pass.Destroy();
    taa_pass.Destroy();
    reactive_mask_pass.Destroy();
    fsr4_bridge.Destroy();
    fg_bridge.Destroy();
    view_cache.Destroy();
}

void TemporalUpscaler::OnSceneColor(VideoCore::ImageId color) {
    if (scene_color != color) {
        scene_color = color;
        reactive_mask_pass.ResetFrame();
    }
}

void TemporalUpscaler::OnBlendedSceneDraw() {
    if (reactive_mask_pass.HasSnapshot() || !scene_color || !Active() || !ReactiveOn()) return;
    if (!texture_cache.HasImage(scene_color)) return;
    auto& color = texture_cache.GetImage(scene_color);
    const bool reduced = ReducedScene(color);
    const u32 ow = color.info.size.width, oh = color.info.size.height;
    const auto scene = Scaled() ? SceneSize(ow, oh) : std::array<u32, 2>{ow, oh};
    const u32 w = reduced ? scene_targets.Size().width : scene[0];
    const u32 h = reduced ? scene_targets.Size().height : scene[1];
    if (color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat ||
        !(color.usage_flags & vk::ImageUsageFlagBits::eTransferSrc)) return;
    if (!EnsureResources(w, h, Scaled() ? target_width : ow, Scaled() ? target_height : oh, !Scaled())) {
        failed = true;
        return;
    }
    scheduler.EndRendering();
    vk::Image source = color.GetImage();
    vk::ImageLayout source_layout = vk::ImageLayout::eTransferSrcOptimal;
    if (reduced) {
        VideoCore::ImageViewInfo ci;
        ci.format = color.info.pixel_format;
        source = scene_targets.Read(scene_color, ci, vk::PipelineStageFlagBits2::eTransfer,
                                    vk::AccessFlagBits2::eTransferRead).image;
        source_layout = vk::ImageLayout::eGeneral;
    } else {
        runtime.Transit(&color, vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
        runtime.FlushBarriers();
    }
    reactive_mask_pass.SnapshotOpaque(source, source_layout, w, h);
}

void TemporalUpscaler::OnSceneComposite() {
    if (!reactive_mask_pass.IsOpaqueValid() || reactive_mask_pass.IsMaskReady() || failed || !ReactiveOn()) return;
    if (!scene_color || !texture_cache.HasImage(scene_color)) return;
    auto& color = texture_cache.GetImage(scene_color);
    const bool reduced = ReducedScene(color);
    const auto scene = Scaled() ? SceneSize(color.info.size.width, color.info.size.height)
                                : std::array<u32, 2>{color.info.size.width, color.info.size.height};
    const u32 w = reduced ? scene_targets.Size().width : scene[0];
    const u32 h = reduced ? scene_targets.Size().height : scene[1];
    if (w != width || h != height || !(color.usage_flags & vk::ImageUsageFlagBits::eStorage)) return;
    scheduler.EndRendering();
    if (reduced) {
        VideoCore::ImageViewInfo ci;
        ci.format = color.info.pixel_format;
        const auto proxy = scene_targets.Read(scene_color, ci);
        reactive_mask_pass.Record(proxy.view, w, h);
        return;
    }
    const auto device = instance.GetDevice();
    const auto color_view = Check(device.createImageView({
        .image = vk::Image(color.backing->image), .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat, .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.FlushBarriers();
    reactive_mask_pass.Record(color_view, w, h);
    scheduler.DeferOperation([device, color_view] { device.destroyImageView(color_view); });
}

void TemporalUpscaler::OnDispatch(u64 cs_hash) {
    if (cs_hash != trigger_hash || done_this_frame || failed || Scaled() || !Active()) return;
    done_this_frame = true;
    if (!scene_color || !camera_motion.Ready() || !camera_motion.Depth()) {
        reset = true;
        return;
    }
    if (!texture_cache.HasImage(scene_color) || !texture_cache.HasImage(camera_motion.Depth())) {
        reset = true;
        return;
    }
    if (dispatch_failed.exchange(false, std::memory_order_relaxed)) failed = true;
    if (failed) return;
    Run();
}

bool TemporalUpscaler::OnFrameStart() {
    const auto& settings = BbSettings::Get();
    const int preset = BbSettings::RenderPreset();
    if (applied_preset != preset || settings.upscaler == BbSettings::UpscalerOff) failed = false;
    if (dispatch_failed.exchange(false, std::memory_order_relaxed)) {
        failed = true;
    }
    const bool active = Active();
    const bool jitter_on = active && settings.jitter && !BbToggle::Disabled(1u << 25);
    const int upscaler = settings.upscaler.load();
    const int output = settings.output_res.load();
    const bool output_changed = !scaled_session && applied_output != output;
    if (output_changed) {
        target_width = BbSettings::OutputWidths[output];
        target_height = BbSettings::OutputHeights[output];
        std::printf("Output resolution: %ux%u (live)\n", target_width, target_height);
        failed = false;
        fsr4_bridge.ResetFailure();
    }
    const bool changed = output_changed || applied_preset != preset || active != last_active ||
                         jitter_on != last_jitter || applied_upscaler != upscaler;
    if (applied_upscaler != upscaler) {
        applied_upscaler = upscaler;
        reset = true;
        resources_ready = false;
        fsr4_bridge.ResetFailure();
    }
    if (!scaled_session) {
        scene_targets.SetSize(SceneResolution::ForPreset(active ? preset : 0,
                                                        {target_width, target_height}));
        render_width = scene_targets.Size().width;
        render_height = scene_targets.Size().height;
    }
    BbSettings::Get().active_render_width = Scaled() ? render_width : scene_targets.Size().width;
    BbSettings::Get().active_render_height = Scaled() ? render_height : scene_targets.Size().height;
    if (changed || !dispatched_last_frame) reset = true;
    if (changed) jitter_index = 0;
    applied_preset = preset;
    applied_output = output;
    applied_upscaler = upscaler;
    last_active = active;
    last_jitter = jitter_on;
    dispatched_last_frame = false;

    // The display pass of an upscaled frame reads the upscaled UI image.
    ui_pass.SetDisplayRedirect(ui_pass.IsUiPhase());
    ui_pass.SetUiPhase(false);
    ui_pass.SetLdrTarget({});
    ui_pass.ResetFrame();
    reactive_mask_pass.ResetFrame();
    done_this_frame = false;
    scene_color = {};

    if (!jitter_on) {
        jitter = {};
        camera_motion.SetJitter(jitter);
        return changed;
    }
    static const auto Halton = [](u32 index, u32 base) {
        float f = 1.0f, result = 0.0f;
        for (u32 i = index; i > 0; i /= base) {
            f /= float(base);
            result += f * float(i % base);
        }
        return result;
    };
    const u32 phases = Scaled() ? Motion::JitterPhases(render_width, target_width)
                                : Motion::JitterPhases(scene_targets.Size().width, 1920);
    jitter_index = jitter_index % phases + 1;
    const float sign = BbToggle::Disabled(1u << 26) ? -1.0f : 1.0f;
    jitter = {
        sign * (Halton(jitter_index, 2) - 0.5f),
        sign * (Halton(jitter_index, 3) - 0.5f)
    };
    camera_motion.SetJitter(jitter);
    return changed;
}

bool TemporalUpscaler::RasterScaling() const {
    return !scaled_session && scene_targets.Reduced() && !done_this_frame;
}

float TemporalUpscaler::SceneMipBias() const {
    if (!Active()) return 0.0f;
    const float scale = Scaled() ? float(target_width) / float(render_width)
                                 : float(1920) / float(scene_targets.Size().width);
    return scale > 1.0f ? -std::log2(scale) : 0.0f;
}

bool TemporalUpscaler::Scaled() const {
    return scaled_session || ui_pass.IsDisplayRedirect();
}

void TemporalUpscaler::OnColorTarget(VideoCore::ImageId color) {
    if (ui_pass.IsUiPhase() || !Active() || !Scaled() || !color) return;
    if (!texture_cache.HasImage(color)) return;
    const auto& image = texture_cache.GetImage(color);
    if (image.info.guest_address && RenderTarget(image.info.size.width, image.info.size.height) &&
        !FrameCapture::IsDisplayBuffer(image.info.guest_address)) {
        ui_pass.SetLdrTarget(color);
    }
}

void TemporalUpscaler::OnDraw(u64 vs_hash, VideoCore::ImageId color, VideoCore::ImageId depth,
                              bool native_viewport) {
    // bbport (issue #67, patch by bmy): a UI draw without a color target (a Scaleform mask, color
    // writes off) writes the stencil the UI's next draws test, so it must go into the UI's
    // output-size depth like them.
    if (ui_pass.IsUiPhase() && !color && depth && depth != ui_pass.GetUiDepth() && ui_pass.GetUiColor()) {
        const auto& depth_image = texture_cache.GetImage(depth);
        const auto& ui_target = texture_cache.GetImage(ui_pass.GetUiColor());
        if (depth_image.info.size.width == ui_target.info.size.width &&
            depth_image.info.size.height == ui_target.info.size.height) {
            ui_pass.EnsureResources(target_width, target_height, ui_target.info.pixel_format,
                                    depth_image.info.pixel_format, render_width, render_height);
            ui_pass.PrepareDepth(depth, texture_cache, runtime);
        }
        return;
    }
    if (!Active() || !Scaled() || !color) return;
    if (ui_pass.IsUiPhase()) {
        ui_pass.SetDisplayRedirect(true);
        return;
    }
    if (native_viewport && vs_hash == ui_trigger_vs) {
        ui_pass.RunUiOnly(color, depth, texture_cache, runtime, camera_motion, scene_targets,
                          scaled_session, render_width, render_height, target_width, target_height);
        reset = true;
        return;
    }
    if (vs_hash == ui_trigger_vs && ui_pass.GetLdrTarget() && camera_motion.Depth()) {
        RunScaled();
        ui_pass.SetDisplayRedirect(true);
    }
}

std::array<u32, 2> TemporalUpscaler::SceneSize(u32 w, u32 h) const {
    if (!scaled_session) return {render_width, render_height};
    auto size = camera_motion.RenderSize();
    if (size[0] == 0 || size[1] == 0) size = {render_width, render_height};
    return {std::min(size[0], w), std::min(size[1], h)};
}

bool TemporalUpscaler::ReducedScene(const VideoCore::Image& color) const {
    return !scaled_session && scene_targets.Reduced() && scene_targets.EligibleScene(color);
}

bool TemporalUpscaler::Active() const {
    return enabled && !failed &&
           (BbSettings::Get().upscaler == BbSettings::UpscalerFsr3 ||
            BbSettings::IsFsr4(BbSettings::Get().upscaler) ||
            BbSettings::Get().upscaler == BbSettings::UpscalerTaa ||
            BbSettings::Get().upscaler == BbSettings::UpscalerDlss) &&
           !BbToggle::Disabled(1u << 24);
}

bool TemporalUpscaler::ReactiveOn() const {
    return BbSettings::Get().reactive && !BbToggle::Disabled(1u << 27) && !UseDlss() &&
           BbSettings::Get().upscaler != BbSettings::UpscalerTaa;
}

bool TemporalUpscaler::EnsureResources(u32 w, u32 h, u32 ow, u32 oh, bool hdr) {
    const bool use_taa = BbSettings::Get().upscaler == BbSettings::UpscalerTaa;
    const bool use_fsr4 = fsr4_bridge.IsActive() || UseDlss();
    if (resources_ready && width == w && height == h && out_width == ow && out_height == oh &&
        context_hdr == hdr && resources_fsr4 == use_fsr4 && resources_taa == use_taa) {
        return true;
    }
    scheduler.Finish();
    const auto device = instance.GetDevice();
    const auto allocator = instance.GetAllocator();
    width = w; height = h; out_width = ow; out_height = oh; context_hdr = hdr;
    if (!use_fsr4 && !use_taa) {
        if (!fsr3_bridge.EnsureContext(w, h, ow, oh, hdr)) return false;
    } else {
        fsr3_bridge.Destroy();
    }
    const auto make_image = [&](VideoCore::UniqueImage& img, vk::UniqueImageView& v, vk::Format fmt,
                                vk::ImageUsageFlags usage, u32 iw, u32 ih) {
        v.reset();
        img = VideoCore::UniqueImage(device, allocator);
        img.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D, .format = fmt, .extent = {iw, ih, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal, .usage = usage, .initialLayout = vk::ImageLayout::eUndefined,
        });
        v = Check(device.createImageViewUnique({
            .image = vk::Image(img), .viewType = vk::ImageViewType::e2D, .format = fmt,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
    };
    make_image(motion_image, motion_view, vk::Format::eR16G16Sfloat,
               vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled, w, h);
    make_image(output_image, output_view,
               hdr ? vk::Format::eR16G16B16A16Sfloat : vk::Format::eR8G8B8A8Unorm,
               vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                   vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
               ow, oh);

    taa_pass.EnsureResources(ow, oh);
    reactive_mask_pass.EnsureResources(w, h);
    fsr4_bridge.EnsureResources(w, h);
    merge_pass.CreatePipelines();
    taa_pass.CreatePipelines();
    reactive_mask_pass.CreatePipelines();
    fsr4_bridge.CreatePipelines();

    reset = true;
    resources_ready = true;
    resources_fsr4 = use_fsr4;
    resources_taa = use_taa;
    return true;
}

void TemporalUpscaler::Run() {
    if (!scene_color || !camera_motion.Depth()) return;
    if (!texture_cache.HasImage(scene_color) || !texture_cache.HasImage(camera_motion.Depth())) return;
    auto& color = texture_cache.GetImage(scene_color);
    auto& depth = texture_cache.GetImage(camera_motion.Depth());
    const u32 ow = color.info.size.width, oh = color.info.size.height;
    const bool reduced = ReducedScene(color) && scene_targets.EligibleScene(depth);
    const u32 w = reduced ? scene_targets.Size().width : ow;
    const u32 h = reduced ? scene_targets.Size().height : oh;
    if (color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat ||
        depth.info.size.width != ow || depth.info.size.height != oh ||
        !(color.usage_flags & vk::ImageUsageFlagBits::eStorage)) return;
    if (!EnsureResources(w, h, ow, oh, true)) { failed = true; return; }

    NativeUpscaleContext ctx{
        .instance = instance,
        .scheduler = scheduler,
        .texture_cache = texture_cache,
        .runtime = runtime,
        .camera_motion = camera_motion,
        .scene_targets = scene_targets,
        .view_cache = view_cache,
        .ui_pass = ui_pass,
        .taa_pass = taa_pass,
        .reactive_mask_pass = reactive_mask_pass,
        .fsr4_bridge = fsr4_bridge,
        .fsr3_bridge = fsr3_bridge,
        .merge_pass = merge_pass,
        .diagnostics = diagnostics,
        .fg_bridge = fg_bridge,
        .scene_color = scene_color,
        .motion_image = vk::Image(motion_image),
        .motion_view = *motion_view,
        .output_image = vk::Image(output_image),
        .output_view = *output_view,
        .applied_preset = applied_preset,
        .jitter = jitter,
        .reset = reset,
        .frame_id = frame_id,
        .last_frame = last_frame,
        .dispatched_last_frame = dispatched_last_frame,
    };
    if (!ExecuteNativeUpscale(ctx)) {
        failed = true;
    }
    reset = ctx.reset;
    frame_id = ctx.frame_id;
    last_frame = ctx.last_frame;
    dispatched_last_frame = ctx.dispatched_last_frame;
    done_this_frame = true;
}

void TemporalUpscaler::RunScaled() {
    const auto ldr = ui_pass.GetLdrTarget();
    if (!ldr || !camera_motion.Depth()) return;
    if (!texture_cache.HasImage(ldr) || !texture_cache.HasImage(camera_motion.Depth())) return;
    auto& color = texture_cache.GetImage(ldr);
    auto& depth = texture_cache.GetImage(camera_motion.Depth());
    const u32 iw = color.info.size.width, ih = color.info.size.height;
    const auto [w, h] = SceneSize(iw, ih);
    const u32 ow = target_width, oh = target_height;
    if (depth.info.size.width != iw || depth.info.size.height != ih || w > ow || h > oh) return;
    if (!EnsureResources(w, h, ow, oh, false)) { failed = true; return; }

    ScaledUpscaleContext ctx{
        .instance = instance,
        .scheduler = scheduler,
        .texture_cache = texture_cache,
        .runtime = runtime,
        .camera_motion = camera_motion,
        .scene_targets = scene_targets,
        .view_cache = view_cache,
        .ui_pass = ui_pass,
        .taa_pass = taa_pass,
        .fsr4_bridge = fsr4_bridge,
        .fsr3_bridge = fsr3_bridge,
        .fg_bridge = fg_bridge,
        .motion_image = vk::Image(motion_image),
        .motion_view = *motion_view,
        .output_image = vk::Image(output_image),
        .output_view = *output_view,
        .render_width = render_width,
        .render_height = render_height,
        .target_width = target_width,
        .target_height = target_height,
        .scaled_session = scaled_session,
        .applied_preset = applied_preset,
        .jitter = jitter,
        .reset = reset,
        .frame_id = frame_id,
        .last_frame = last_frame,
        .dispatched_last_frame = dispatched_last_frame,
        .dispatch_failed = dispatch_failed,
    };
    if (!ExecuteScaledUpscale(ctx)) {
        failed = true;
    }
    reset = ctx.reset;
    frame_id = ctx.frame_id;
    last_frame = ctx.last_frame;
    dispatched_last_frame = ctx.dispatched_last_frame;
    done_this_frame = true;
}

bool TemporalUpscaler::UseDlss() const {
    const Dlss* dlss = Dlss::Get();
    return BbSettings::Get().upscaler == BbSettings::UpscalerDlss && dlss && dlss->Available() &&
           !dlss_failed;
}

bool TemporalUpscaler::RecordDlss(vk::CommandBuffer cmdbuf, const Dlss::Resource& color,
                                  const Dlss::Resource& depth, u32 w, u32 h, u32 ow, u32 oh,
                                  float frame_ms, bool hdr) {
    Dlss* dlss = Dlss::Get();
    const Dlss::FeatureDesc desc{w, h, ow, oh, Dlss::QualityForScale(float(ow) / float(w)), hdr};
    bool ok = true;
    if (!dlss->HasFeature(desc)) {
        scheduler.WaitSubmittedWork();
        dlss->ReleaseFeature();
        ok = dlss->CreateFeature(cmdbuf, desc);
        reset = true;
    }
    if (ok) {
        const float sign = BbToggle::Disabled(1u << 26) ? -1.0f : 1.0f;
        const auto& settings = BbSettings::Get();
        ok = dlss->Evaluate(cmdbuf, {
            .color = color,
            .depth = depth,
            .motion = {vk::Image(motion_image), *motion_view, vk::Format::eR16G16Sfloat,
                       vk::ImageAspectFlagBits::eColor, w, h},
            .output = {vk::Image(output_image), *output_view, vk::Format::eR16G16B16A16Sfloat,
                       vk::ImageAspectFlagBits::eColor, ow, oh},
            .jitter_x = sign * jitter[0],
            .jitter_y = sign * jitter[1],
            .reset = reset,
            .frame_ms = frame_ms,
            .sharpness = settings.sharpen ? std::min(settings.sharpness.load(), 1.0f) : 0.0f,
        });
    }
    if (!ok) {
        std::printf("Upscaler: DLSS failed; falling back to FSR 3.1\n");
        BbSettings::Get().upscaler = BbSettings::UpscalerFsr3;
        dlss_failed = true;
        reset = true;
    }
    return ok;
}

} // namespace Vulkan
