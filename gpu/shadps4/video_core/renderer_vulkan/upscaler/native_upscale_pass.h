// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;
class CameraMotion;
class SceneTargets;
class ImageViewCache;
class UiCompositionPass;
class TaaPass;
class ReactiveMaskPass;
class Fsr4Bridge;
class Fsr3Bridge;
class MergePass;
class UpscalerDiagnostics;
class FrameGenerationBridge;

struct NativeUpscaleContext {
    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    CameraMotion& camera_motion;
    SceneTargets& scene_targets;
    ImageViewCache& view_cache;
    UiCompositionPass& ui_pass;
    TaaPass& taa_pass;
    ReactiveMaskPass& reactive_mask_pass;
    Fsr4Bridge& fsr4_bridge;
    Fsr3Bridge& fsr3_bridge;
    MergePass& merge_pass;
    UpscalerDiagnostics& diagnostics;
    FrameGenerationBridge& fg_bridge;

    VideoCore::ImageId scene_color{};
    vk::Image motion_image;
    vk::ImageView motion_view;
    vk::Image output_image;
    vk::ImageView output_view;
    int applied_preset{-1};
    std::array<float, 2> jitter{};
    bool reset{false};
    u64 frame_id{0};
    std::chrono::steady_clock::time_point last_frame{};
    bool dispatched_last_frame{false};
};

bool ExecuteNativeUpscale(NativeUpscaleContext& ctx);

} // namespace Vulkan
