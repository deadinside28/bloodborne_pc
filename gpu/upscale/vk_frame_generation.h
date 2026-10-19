// SPDX-License-Identifier: GPL-2.0-or-later
// bbport upscale: frame generation. Frames made between each two of the game's are shown between
// them, evenly spaced. Two providers (BbSettings::frame_generation):
//  - FSR 3.1 frame interpolation (the portable Vulkan interface of FSR-Vulkan, in
//    third_party/fsr-vulkan): any GPU, one frame (2x).
//  - DLSS Frame Generation (upscale/vk_dlss through the optional bridge gpu/dlss_bridge with
//    NVIDIA's libnvidia-ngx-dlssg.so): GeForce RTX 40 one frame (2x), RTX 50 up to three (4x).
//
// Where its inputs come from (all in the draw stream, on the recording thread):
//  - CaptureScene, at the end of the upscaler's scaled run: the frame's depth is copied (the game
//    reuses its depth buffer before the display pass) and its motion vectors and camera kept.
//  - CaptureHudless, at the first UI draw: a copy of the frame without its UI.
//  - Interpolate, at the game's display pass: the output-size image holds the finished frame,
//    UI and all. The generation is recorded there, before the next frame's work.
//  - The presenter encodes the generated frames like the real one (PrepareFrame) and shows them
//    first.
// Settings: frame_generation and frame_generation_multiplier in bbport.ini, the in-game menu,
// BB_FRAME_GEN=fsr3|dlss (1: fsr3), BB_FRAME_GEN_MULTIPLIER=2..4.
#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "common/types.h"
#include "textures/image.h"
#include "upscale/vk_dlss.h"
#include "vulkan/vk_common.h"

struct FfxVkPortableFrameGenerationContext;

namespace Vulkan {

class Instance;
class Scheduler;

class FrameGeneration {
public:
    /// Frames generated per game frame at most (DLSS 4x).
    static constexpr u32 MaxGenerated = 3;

    FrameGeneration(const Instance& instance, Scheduler& scheduler);
    ~FrameGeneration();

    /// A provider is chosen in the settings and can run (its libraries, the GPU, no failure).
    bool Wanted() const;
    /// The provider that runs now (BbSettings::FrameGen), FrameGenOff when none can.
    int Provider() const;
    /// Frames generated per game frame: FSR 3.1 one; DLSS the multiplier less one, within what
    /// the GPU offers.
    u32 WantedCount() const;

    /// What the scene of this frame was rendered with (render size: depth and motion).
    struct Scene {
        vk::Image color;   ///< output size, the upscaled scene (UI not drawn yet), General
        vk::Format color_format;
        u32 width, height; ///< output size
        vk::Image depth;   ///< render size, General
        vk::Format depth_format;
        vk::Image motion;  ///< render size, RG16F pixels (previous - current), General
        vk::ImageView motion_view;
        u32 render_width, render_height;
        float jitter_x, jitter_y;
        float frame_ms;
        float camera_near, camera_far, vertical_fov;
        bool reset;
        /// The camera in the world (FSR's frame interpolation uses them).
        float camera_position[3], camera_up[3], camera_right[3], camera_forward[3];
        /// The camera as clip-space matrices (camera_matrices.h; DLSS separates the camera's
        /// motion from the objects' with them). has_matrices false before two cameras.
        bool has_matrices;
        std::array<float, 16> view_to_clip, clip_to_prev_clip, prev_clip_to_clip;
    };
    /// End of the upscaler's scaled run (recording thread): keeps the inputs and copies the depth.
    void CaptureScene(const Scene& scene);

    /// The first UI draw of the frame is about to run over `color` (the scene's image): what it
    /// holds now is the frame without its UI, post processing included (recording thread).
    void CaptureHudless(vk::Image color);

    /// The game's display pass of `display` (recording thread): `color` (the scene's image) now
    /// holds the frame with its UI. Records the generation if this frame's scene was captured.
    void Interpolate(vk::Image color, VAddr display);

    /// Presenter: the frames generated before the last real one of `display` (General layout,
    /// the scene's format and size), oldest first; how many (0 when none). They are used once.
    struct Output {
        vk::Image image;
        vk::ImageView view;
        vk::Format format;
        u32 width, height;
        u64 frame;
    };
    u32 TakeOutputs(VAddr display, std::array<Output, MaxGenerated>& out);

private:
    bool EnsureContext(u32 render_width, u32 render_height, u32 width, u32 height, vk::Format format);
    bool EnsureDlss(u32 render_width, u32 render_height, u32 width, u32 height, vk::Format format);
    void Destroy();
    /// A target for generated frame `index`, made at the scene's size and format.
    struct Target;
    Target& EnsureTarget(u32 index);
    /// The scene image's view (DLSS reads it through one), kept for the image handle.
    vk::ImageView ColorView(vk::Image image, vk::Format format);
    void RecordFsr(vk::Image color, u64 id, bool reset, Target& target);
    void RecordDlss(vk::Image color, u64 id, bool reset, u32 count);
    /// The provider failed for good: off, with the reason in the settings (the menu shows it).
    void Fail(int provider, const char* reason);

    const Instance& instance;
    Scheduler& scheduler;
    bool supported = false;     ///< FSR 3.1 frame generation (the GPU's capabilities)
    int dlss_max_count = 0;     ///< DLSS Frame Generation: frames per pair (0: unavailable)
    std::atomic<bool> fsr_failed{false};
    std::atomic<bool> dlss_failed{false};
    int last_provider = 0; ///< of the last captured scene (a change starts over)

    FfxVkPortableFrameGenerationContext* context = nullptr;
    u32 context_render_w = 0, context_render_h = 0, context_w = 0, context_h = 0;
    vk::Format context_format{};
    /// DLSS: the feature made (or to be made in the next recorded generation).
    std::optional<Dlss::FrameGenDesc> dlss_desc;
    bool dlss_create_pending = false;

    Scene scene{};
    bool scene_captured = false; ///< this frame's, not yet interpolated
    u64 frame_id = 0;
    bool reset_next = true;
    u64 scene_id = 0;        ///< the captured scene's frame id (its Prepare's and Dispatch's)
    bool scene_reset = true; ///< it starts over (nothing before it to interpolate from)
    /// The scene's depth, copied at the capture (the game reuses its depth buffer before the
    /// display pass, where the generation runs). `depth_copy_view`: its depth aspect (DLSS).
    VideoCore::HostImage depth_copy;
    vk::UniqueImageView depth_copy_view;
    VideoCore::HostImage hudless;     ///< the frame without its UI (CaptureHudless)
    vk::UniqueImageView hudless_view;
    bool hudless_ready = false;       ///< taken for the captured scene
    vk::Format depth_copy_format{};
    u32 depth_copy_w = 0, depth_copy_h = 0;
    vk::Image color_viewed{};
    vk::Format color_view_format{};
    vk::UniqueImageView color_view;

    struct Target {
        VideoCore::HostImage image;
        vk::UniqueImageView view;
        vk::Format format{};
        u32 width = 0, height = 0;
        u64 frame = 0;
        bool ready = false;
    };
    std::mutex targets_mutex;
    std::array<Target, MaxGenerated> targets;
};

} // namespace Vulkan
