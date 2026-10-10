// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
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
class Fsr4Bridge;
class Fsr3Bridge;
class FrameGenerationBridge;

struct ScaledUpscaleContext {
    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    CameraMotion& camera_motion;
    SceneTargets& scene_targets;
    ImageViewCache& view_cache;
    UiCompositionPass& ui_pass;
    TaaPass& taa_pass;
    Fsr4Bridge& fsr4_bridge;
    Fsr3Bridge& fsr3_bridge;
    FrameGenerationBridge& fg_bridge;

    vk::Image motion_image;
    vk::ImageView motion_view;
    vk::Image output_image;
    vk::ImageView output_view;
    u32 render_width{0};
    u32 render_height{0};
    u32 target_width{1920};
    u32 target_height{1080};
    bool scaled_session{false};
    int applied_preset{-1};
    std::array<float, 2> jitter{};
    bool reset{false};
    u64 frame_id{0};
    std::chrono::steady_clock::time_point last_frame{};
    bool dispatched_last_frame{false};
    std::atomic<bool>& dispatch_failed;
};

bool ExecuteScaledUpscale(ScaledUpscaleContext& ctx);

} // namespace Vulkan
