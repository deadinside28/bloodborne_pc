// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <mutex>
#include <unordered_map>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Runtime;
class CameraMotion;
class SceneTargets;
class UpscalerDiagnostics;

struct Target {
    vk::ImageView view;
    vk::ImageLayout layout;
    u32 width, height;
    bool native_ui = false;
};

struct Display {
    vk::Image image;
    vk::Format format;
    u32 width, height;
};

struct MirrorView {
    vk::Format format;
    vk::ComponentMapping mapping;
    vk::UniqueImageView view;
};

struct DisplayImage {
    VideoCore::UniqueImage image;
    std::vector<MirrorView> views;
    vk::Format format{};
    u32 width = 0, height = 0;
    bool valid = false;
};

class UiCompositionPass {
public:
    UiCompositionPass(const Instance& instance, Scheduler& scheduler);
    ~UiCompositionPass();

    void EnsureResources(u32 w, u32 h, vk::Format color, vk::Format depth,
                         u32 render_w, u32 render_h);
    void PrepareDepth(VideoCore::ImageId depth_id, VideoCore::TextureCache& texture_cache,
                      Runtime& runtime);
    void RunUiOnly(VideoCore::ImageId color_id, VideoCore::ImageId depth_id,
                   VideoCore::TextureCache& texture_cache, Runtime& runtime,
                   CameraMotion& camera_motion, SceneTargets& scene_targets,
                   bool scaled_session, u32 render_w, u32 render_h,
                   u32 target_w, u32 target_h);

    bool RedirectColor(VideoCore::ImageId color, const VideoCore::ImageViewInfo& view_info,
                       Target& target, VideoCore::TextureCache& texture_cache);
    bool RedirectDepth(VideoCore::ImageId depth, Target& target);
    bool RedirectSampled(VideoCore::ImageId image, const VideoCore::ImageViewInfo& info,
                         vk::ImageView& view, vk::ImageLayout& layout);
    bool DisplayOverride(VAddr address, Display& display, UpscalerDiagnostics& diagnostics);

    vk::ImageView Mirror(vk::Image image, std::vector<MirrorView>& views, vk::Format format,
                         vk::ComponentMapping mapping);

    void ResetFrame() noexcept {
        ui_read_barrier = false;
    }

    [[nodiscard]] bool IsUiPhase() const noexcept { return ui_phase; }
    void SetUiPhase(bool phase) noexcept { ui_phase = phase; }
    [[nodiscard]] bool IsDisplayRedirect() const noexcept { return display_redirect; }
    void SetDisplayRedirect(bool redirect) noexcept { display_redirect = redirect; }

    [[nodiscard]] VideoCore::ImageId GetUiColor() const noexcept { return ui_color; }
    void SetUiColor(VideoCore::ImageId id) noexcept { ui_color = id; }
    [[nodiscard]] VideoCore::ImageId GetUiDepth() const noexcept { return ui_depth; }
    void SetUiDepth(VideoCore::ImageId id) noexcept { ui_depth = id; }
    [[nodiscard]] VideoCore::ImageId GetLdrTarget() const noexcept { return ldr_target; }
    void SetLdrTarget(VideoCore::ImageId id) noexcept { ldr_target = id; }

    [[nodiscard]] vk::Image GetUiImage() const noexcept { return vk::Image(ui_image); }
    [[nodiscard]] vk::ImageView GetUiView() const noexcept { return *ui_view; }
    [[nodiscard]] vk::UniqueImageView& GetUiStorageView() noexcept { return ui_storage_view; }
    [[nodiscard]] u32 GetUiWidth() const noexcept { return ui_width; }
    [[nodiscard]] u32 GetUiHeight() const noexcept { return ui_height; }
    [[nodiscard]] vk::Format GetUiFormat() const noexcept { return ui_format; }

private:
    const Instance& instance;
    Scheduler& scheduler;

    bool depth_blit{false};
    bool ui_phase{false};
    bool display_redirect{false};
    bool ui_read_barrier{false};

    VideoCore::ImageId ldr_target{};
    VideoCore::ImageId ui_color{}, ui_depth{};

    u32 ui_width{0}, ui_height{0};
    vk::Format ui_format{vk::Format::eUndefined};
    vk::Format ui_depth_format{vk::Format::eUndefined};

    VideoCore::UniqueImage ui_image;
    VideoCore::UniqueImage ui_depth_image;
    vk::UniqueImageView ui_view;
    vk::UniqueImageView ui_depth_view;
    vk::UniqueImageView ui_storage_view;
    std::vector<MirrorView> ui_views;

    std::mutex display_mutex;
    std::unordered_map<VAddr, DisplayImage> displays;
};

} // namespace Vulkan
