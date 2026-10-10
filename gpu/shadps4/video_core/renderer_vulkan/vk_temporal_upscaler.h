// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/upscaler/frame_generation_bridge.h"
#include "video_core/renderer_vulkan/upscaler/fsr4_bridge.h"
#include "video_core/renderer_vulkan/upscaler/reactive_mask_pass.h"
#include "video_core/renderer_vulkan/upscaler/taa_pass.h"
#include "video_core/renderer_vulkan/upscaler/ui_composition_pass.h"
#include "video_core/renderer_vulkan/upscaler/upscaler_diagnostics.h"
#include "video_core/renderer_vulkan/upscaler/fsr3_bridge.h"
#include "video_core/renderer_vulkan/upscaler/merge_pass.h"
#include "video_core/renderer_vulkan/upscaler/view_cache.h"
#include "video_core/renderer_vulkan/vk_dlss.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;
class CameraMotion;
class SceneTargets;

class TemporalUpscaler {
public:
    using Target = Vulkan::Target;
    using Display = Vulkan::Display;

    TemporalUpscaler(const Instance& instance, Scheduler& scheduler,
                     VideoCore::TextureCache& texture_cache, Runtime& runtime,
                     CameraMotion& camera_motion, SceneTargets& scene_targets);
    ~TemporalUpscaler();

    [[nodiscard]] bool Enabled() const noexcept { return enabled; }

    void OnSceneColor(VideoCore::ImageId color);
    void OnBlendedSceneDraw();
    void OnSceneComposite();
    void OnDispatch(u64 cs_hash);

    bool OnFrameStart();
    bool RasterScaling() const;
    [[nodiscard]] float SceneMipBias() const;
    [[nodiscard]] u64 RedirectState() const noexcept {
        return u64(ui_pass.IsUiPhase()) | u64(ui_pass.IsDisplayRedirect()) << 1 |
               u64(done_this_frame) << 2 | u64(ui_pass.GetUiColor().index) << 8 |
               u64(ui_pass.GetUiDepth().index) << 36;
    }

    [[nodiscard]] std::array<float, 2> Jitter() const noexcept {
        return done_this_frame ? std::array<float, 2>{} : jitter;
    }

    void OnDraw(u64 vs_hash, VideoCore::ImageId color, VideoCore::ImageId depth,
                bool native_viewport);
    void OnColorTarget(VideoCore::ImageId color);

    bool RedirectColor(VideoCore::ImageId color, const VideoCore::ImageViewInfo& view,
                       Target& target) {
        return ui_pass.RedirectColor(color, view, target, texture_cache);
    }
    bool RedirectDepth(VideoCore::ImageId depth, Target& target) {
        return ui_pass.RedirectDepth(depth, target);
    }
    bool RedirectSampled(VideoCore::ImageId image, const VideoCore::ImageViewInfo& info,
                         vk::ImageView& view, vk::ImageLayout& layout) {
        return ui_pass.RedirectSampled(image, info, view, layout);
    }
    [[nodiscard]] bool RedirectsSampled(VideoCore::ImageId image) const {
        return ui_pass.IsDisplayRedirect() && image == ui_pass.GetUiColor();
    }

    bool DisplayOverride(VAddr address, Display& display) {
        return ui_pass.DisplayOverride(address, display, diagnostics);
    }
    bool PresentDumpDue() {
        return diagnostics.PresentDumpDue();
    }
    void DumpPresented(vk::Image image, u32 width, u32 height, vk::Format format) {
        diagnostics.DumpPresented(image, width, height, format);
    }

    void SetDisplayInfo(u32 display_w, u32 display_h, vk::Format display_format) {
        fg_bridge.SetDisplayInfo(display_w, display_h, display_format);
    }
    [[nodiscard]] FrameGenerationManager* GetFrameGeneration() noexcept {
        return fg_bridge.GetManager();
    }
    bool RecordFrameGenDispatch(vk::CommandBuffer cmdbuf, vk::Image current_color,
                                u32 display_w, u32 display_h, float frame_ms, u64 frame_id) {
        return fg_bridge.RecordDispatch(cmdbuf, current_color, display_w, display_h, frame_ms,
                                        camera_motion, reset);
    }

private:
    ImageViewCache view_cache;
    vk::ImageView CachedView(const VideoCore::Image& image, vk::Format format,
                             vk::ImageAspectFlags aspect) {
        return view_cache.Get(image, format, aspect);
    }

    void Run();
    void RunScaled();
    [[nodiscard]] bool Scaled() const;
    [[nodiscard]] bool RenderTarget(u32 w, u32 h) const {
        if (!scaled_session) return w == 1920 && h == 1080;
        return w >= render_width && h >= render_height && w < render_width + 8 &&
               h < render_height + 8;
    }
    [[nodiscard]] std::array<u32, 2> SceneSize(u32 w, u32 h) const;
    [[nodiscard]] bool ReducedScene(const VideoCore::Image& color) const;
    [[nodiscard]] bool Active() const;
    [[nodiscard]] bool ReactiveOn() const;
    bool EnsureResources(u32 width, u32 height, u32 out_width, u32 out_height, bool hdr);
    /// bbport: DLSS selected and the bridge is ready (NVIDIA RTX, gpu/dlss_bridge).
    [[nodiscard]] bool UseDlss() const;
    /// Records DLSS into `cmdbuf` (output in General). `hdr`: linear scene color input.
    bool RecordDlss(vk::CommandBuffer cmdbuf, const Dlss::Resource& color,
                    const Dlss::Resource& depth, u32 w, u32 h, u32 ow, u32 oh, float frame_ms,
                    bool hdr);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    CameraMotion& camera_motion;
    SceneTargets& scene_targets;

    TaaPass taa_pass;
    ReactiveMaskPass reactive_mask_pass;
    Fsr4Bridge fsr4_bridge;
    UiCompositionPass ui_pass;
    UpscalerDiagnostics diagnostics;
    FrameGenerationBridge fg_bridge;
    Fsr3Bridge fsr3_bridge;
    MergePass merge_pass;

    int applied_preset = -1;
    int applied_upscaler = -1;
    bool dispatched_last_frame = false;
    bool last_active = false, last_jitter = false;

    bool enabled = false;
    bool failed = false;
    std::atomic<bool> dispatch_failed{false};
    u64 trigger_hash = 0x9a9cf8a9;
    VideoCore::ImageId scene_color{};
    bool done_this_frame = false;
    u32 preset_file_frames = 0;
    int applied_output = -1;
    std::array<float, 2> jitter{};
    u32 jitter_index = 0;
    bool reset = true;
    u64 frame_id = 0;
    std::chrono::steady_clock::time_point last_frame{};

    u32 width = 0, height = 0;
    u32 out_width = 0, out_height = 0;
    bool context_hdr = true;
    u32 target_width = 1920, target_height = 1080;
    u64 ui_trigger_vs = 0x34e8a281;
    bool scaled_session = false;
    u32 render_width = 0, render_height = 0;

    bool resources_ready = false;
    bool resources_fsr4 = false;
    bool resources_taa = false;
    bool dlss_failed = false;
    VideoCore::UniqueImage motion_image;
    VideoCore::UniqueImage output_image;
    vk::UniqueImageView motion_view;
    vk::UniqueImageView output_view;
};

} // namespace Vulkan
