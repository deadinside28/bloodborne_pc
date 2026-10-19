// SPDX-License-Identifier: GPL-2.0-or-later
// bbport upscale: FSR 3.1 frame generation (the portable Vulkan interface of FSR-Vulkan, in
// third_party/fsr-vulkan). A frame interpolated between each two of the game's is shown halfway
// between them.
//
// Where its inputs come from (all in the draw stream, on the recording thread):
//  - CaptureScene, at the end of the upscaler's scaled run: the frame's depth is copied (the game
//    reuses its depth buffer before the display pass) and its motion vectors and camera kept.
//  - Interpolate, at the game's display pass: the output-size image holds the finished frame,
//    UI and all. Prepare (optical flow on that frame) and the interpolation are recorded there,
//    before the next frame's work. No HUD-less image: the game draws more than its HUD over the
//    upscaled scene, and FSR pastes whatever differs uninterpolated (a double image).
//  - The presenter encodes that output like the real frame (PrepareFrame) and shows it first.
// Settings: frame_generation in bbport.ini, the in-game menu, BB_FRAME_GEN=1.
#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "common/types.h"
#include "textures/image.h"
#include "vulkan/vk_common.h"

struct FfxVkPortableFrameGenerationContext;

namespace Vulkan {

class Instance;
class Scheduler;

class FrameGeneration {
public:
    FrameGeneration(const Instance& instance, Scheduler& scheduler);
    ~FrameGeneration();

    /// The setting is on and the GPU can.
    bool Wanted() const;

    /// What the scene of this frame was rendered with (render size: depth and motion).
    struct Scene {
        vk::Image color;   ///< output size, the upscaled scene (UI not drawn yet), General
        vk::Format color_format;
        u32 width, height; ///< output size
        vk::Image depth;   ///< render size, General
        vk::Format depth_format;
        vk::Image motion;  ///< render size, RG16F pixels (previous - current), General
        u32 render_width, render_height;
        float jitter_x, jitter_y;
        float frame_ms;
        float camera_near, camera_far, vertical_fov;
        bool reset;
        /// The camera in the world (FSR's frame interpolation uses them).
        float camera_position[3], camera_up[3], camera_right[3], camera_forward[3];
    };
    /// End of the upscaler's scaled run (recording thread): keeps the HUD-less copy and the inputs.
    void CaptureScene(const Scene& scene);

    /// The first UI draw of the frame is about to run over `color` (the scene's image): what it
    /// holds now is the frame without its UI, post processing included (recording thread).
    void CaptureHudless(vk::Image color);

    /// The game's display pass of `display` (recording thread): `color` (the scene's image) now
    /// holds the frame with its UI. Records the interpolation if this frame's scene was captured.
    void Interpolate(vk::Image color, VAddr display);

    /// Presenter: the frame interpolated before the last real one of `display` (General layout,
    /// the scene's format and size), if there is one; it is used once.
    struct Output {
        vk::Image image;
        vk::ImageView view;
        vk::Format format;
        u32 width, height;
        u64 frame;
    };
    bool TakeOutput(VAddr display, Output& out);

private:
    bool EnsureContext(u32 render_width, u32 render_height, u32 width, u32 height, vk::Format format);
    void Destroy();

    const Instance& instance;
    Scheduler& scheduler;
    bool supported = false;
    bool failed = false;

    FfxVkPortableFrameGenerationContext* context = nullptr;
    u32 context_render_w = 0, context_render_h = 0, context_w = 0, context_h = 0;
    vk::Format context_format{};

    Scene scene{};
    bool scene_captured = false; ///< this frame's, not yet interpolated
    u64 frame_id = 0;
    bool reset_next = true;
    u64 scene_id = 0;        ///< the captured scene's frame id (its Prepare's and Dispatch's)
    bool scene_reset = true; ///< it starts over (nothing before it to interpolate from)
    /// The scene's depth, copied at the capture (the game reuses its depth buffer before the
    /// display pass, where Prepare runs).
    VideoCore::HostImage depth_copy;
    VideoCore::HostImage hudless;     ///< the frame without its UI (CaptureHudless)
    bool hudless_ready = false;       ///< taken for the captured scene
    vk::Format depth_copy_format{};
    u32 depth_copy_w = 0, depth_copy_h = 0;

    struct Target {
        VideoCore::HostImage image;
        vk::UniqueImageView view;
        vk::Format format{};
        u32 width = 0, height = 0;
        u64 frame = 0;
        bool ready = false;
    };
    std::mutex targets_mutex;
    std::unordered_map<VAddr, Target> targets;
};

} // namespace Vulkan
