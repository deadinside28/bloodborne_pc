// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport render: the pass between the game's display buffer and the presented frame: a
// full-screen draw into the frame image, encoding for the display (sRGB curve with the user's
// gamma, or HDR passed through); and the HDR output pass (presenter.cpp RecordHdrPresent): the
// SDR composite and the menu's layer encoded into an HDR10 (PQ) or scRGB swapchain image.
#pragma once

#include "common/types.h"
#include "vulkan/vk_common.h"

namespace Vulkan {

class EncodePass {
public:
    /// Push constants of present_encode.frag.
    struct Settings {
        float gamma = 1.0f;
        u32 hdr = 0;
        u32 srgb_input = 0;
        /// RCAS strength (0: none) on the finished frame: sharpening without an upscaler.
        float sharpen = 0.0f;
    };

    EncodePass(vk::Device device, vk::Format output_format);
    ~EncodePass();

    /// Draws `input` (shader read layout) over all of `output` (color attachment layout, left
    /// in it).
    void Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::ImageView output,
                vk::Extent2D output_size, const Settings& settings);

private:
    vk::UniqueSampler sampler;
    vk::UniqueDescriptorSetLayout set_layout;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
};

/// HDR output (bbport.ini hdr=1): encodes the swapchain-sized SDR composite (the game's frame
/// letterboxed, plus the port's menu layer) into the HDR swapchain image: HDR10 PQ or scRGB.
/// Expand (hdr_mode=expand, and native+ frames until the scene chain lands) stretches the
/// picture's highlights towards the peak (shaders/hdr_output.frag).
class HdrOutputPass {
public:
    /// Push constants of hdr_output.frag.
    struct Settings {
        float paper_white = 203.0f; ///< nits of SDR white
        float peak = 1000.0f;       ///< nits the display reaches
        float boost = 0.25f;        ///< highlight expansion, 0..1
        u32 flags = 0;              ///< FlagScRgb | FlagOverlay | FlagDebug | FlagCompare |
                                    ///< FlagNative16 | FlagUiMask
        /// Native+: how far the darkest greys are taken down to true black, in units of the
        /// game's white (hdr_black_floor nits / paper white).
        float black_floor = 0.0f;
        /// Native+: the HUD's white as a factor of paper white (hdr_ui_nits / paper white);
        /// the masked pixels are brightened by it.
        float ui_white = 1.0f;
        /// Native+: the RenoDX-style user grading, neutral at 1: exposure, contrast,
        /// highlights, shadows, saturation.
        float exposure = 1.0f;
        float contrast = 1.0f;
        float highlights = 1.0f;
        float shadows = 1.0f;
        float saturation = 1.0f;
    };
    static constexpr u32 FlagScRgb = 1;     ///< scRGB (linear BT.709) output, else HDR10 PQ
    static constexpr u32 FlagOverlay = 2;   ///< the menu layer is drawn
    static constexpr u32 FlagDebug = 4;     ///< BB_HDR_DEBUG: the expansion as a heat map
    static constexpr u32 FlagCompare = 8;   ///< the left half as SDR (the menu's comparison)
    static constexpr u32 FlagNative16 = 16; ///< Native+: the composite carries the scene's HDR
    static constexpr u32 FlagUiMask = 32;   ///< Native+: the snapshot's alpha is the HUD's mask
    static constexpr u32 FlagLinearFrame = 64; ///< Native+: the frame is linear already (sRGB view)

    /// `output_format`: the HDR swapchain's format.
    void Create(vk::Device device, vk::Format output_format);

    bool IsCreated() const {
        return static_cast<bool>(pipeline);
    }

    /// Draws into `output` (layout ColorAttachmentOptimal) of `extent`; `game` and `overlay`
    /// (both ShaderReadOnlyOptimal, `extent` big) are read texel for texel; `snapshot` (the
    /// masked pre-UI frame) wherever its alpha is the HUD's mask.
    void Render(vk::CommandBuffer cmdbuf, vk::ImageView game, vk::ImageView overlay,
                vk::ImageView snapshot, vk::ImageView output, vk::Extent2D extent,
                const Settings& settings);

private:
    vk::UniquePipeline pipeline{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniqueSampler sampler{};
};

} // namespace Vulkan
