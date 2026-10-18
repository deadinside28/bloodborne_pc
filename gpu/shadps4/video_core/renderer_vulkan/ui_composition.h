// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace Vulkan::UiComposition {

enum class Background { None, Copy, Temporal };

inline constexpr uint32_t LogicalWidth = 1920;
inline constexpr uint32_t LogicalHeight = 1080;

constexpr std::array<uint32_t, 2> LogicalSize(uint32_t output_width, uint32_t output_height) {
    if (!output_width || !output_height) return {LogicalWidth, LogicalHeight};
    const uint32_t height = uint32_t((uint64_t(LogicalWidth) * output_height + output_width / 2) /
                                     output_width);
    return {LogicalWidth, height};
}

// UI composition must not require scene depth/camera or an active FSR context.
constexpr Background Choose(bool scaled, bool ui_draw, bool scene_ready, bool fsr_active) {
    if (!scaled || !ui_draw) {
        return Background::None;
    }
    return scene_ready && fsr_active ? Background::Temporal : Background::Copy;
}

inline bool NativeViewport(float width, float height, uint32_t logical_width = LogicalWidth,
                           uint32_t logical_height = LogicalHeight) {
    return std::abs(std::abs(width) - float(logical_width)) < 0.5f &&
           std::abs(std::abs(height) - float(logical_height)) < 0.5f;
}

// Scaleform draws, including the first stencil/movie pass. A native-size viewport
// alone also matches every fullscreen post pass when guest targets stay at 1080p.
constexpr bool MovieShader(uint64_t hash) {
    return hash == 0x34e8a281 || hash == 0x81d336ce || hash == 0x09957251 ||
           hash == 0x24042a9b || hash == 0xa400228b ||
           // The title menu while it animates (its highlight): without these its frames were
           // shown at the game's 1080p between output-size ones, a flicker (BB_UI_TRACE=1).
           hash == 0xbb4d5f8c || hash == 0xe94b5b06 || hash == 0x9951d199 ||
           hash == 0x44072322 || hash == 0xa334a845;
}

inline std::array<float, 2> Scale(uint32_t guest_width, uint32_t guest_height,
                                  uint32_t output_width, uint32_t output_height,
                                  bool native_coordinates, uint32_t logical_width = LogicalWidth,
                                  uint32_t logical_height = LogicalHeight) {
    return {float(output_width) / float(native_coordinates ? logical_width : guest_width),
            float(output_height) / float(native_coordinates ? logical_height : guest_height)};
}

} // namespace Vulkan::UiComposition
