// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: HDR output (render/presenter.cpp RecordHdrPresent). The SDR composite (the frame
// letterboxed into a swapchain-sized sRGB-encoded image; the bars black) and the menu's layer
// (premultiplied alpha, sRGB encoded) become the HDR swapchain's signal: HDR10 (BT.2020
// primaries, PQ) or scRGB (linear BT.709, 1.0 = 80 nits). SDR white is shown at paper_white;
// nothing goes above the peak. Expand (hdr_mode=expand, and native+ frames until the scene
// chain lands): the picture's brightest parts are stretched towards the peak. The game's
// picture is made for SDR displays, which show it with a 2.2 power curve; the piecewise sRGB
// curve lifts Bloodborne's shadows into grey, so 2.2 it is (as RenoDX's Gamma Correction 2.2).
#version 450

layout(location = 0) out vec4 color;

layout(binding = 0) uniform sampler2D game;     // the letterboxed SDR frame, swapchain-sized
layout(binding = 1) uniform sampler2D overlay;  // the menu layer (FLAG_OVERLAY)
layout(binding = 2) uniform sampler2D snapshot; // the frame before the UI (FLAG_UI_MASK)

layout(push_constant) uniform Settings {
    float paper_white; // nits of SDR white
    float peak;        // nits the display reaches
    float boost;       // 0: SDR white at paper white .. 1: SDR white at the peak
    uint flags;
    float black_floor; // Native+: the game's darkest greys taken down to true black (of its white)
    float ui_white;    // Native+: the HUD's white as a factor of paper white
} hdr;

const uint FLAG_SCRGB = 1u;   // scRGB output, else HDR10
const uint FLAG_OVERLAY = 2u; // the menu layer has something drawn
const uint FLAG_DEBUG = 4u;   // BB_HDR_DEBUG: the expansion as a heat map
const uint FLAG_COMPARE = 8u; // the left half as SDR (at paper white), for comparison
// Native+ (render/hdr_channel.cpp): the composite already carries the scene's HDR — the game's
// encoded values extended above its white by the chain's delta. Nothing is expanded; the frame
// decodes with the same 2.2 and is shown as it is (the game's look at paper white where a frame
// has nothing above it).
const uint FLAG_NATIVE16 = 16u;
// Native+: the snapshot's alpha is the HUD's mask (hdr_ui_mask.comp); the HUD is shown at UI
// Brightness (ui_white).
const uint FLAG_UI_MASK = 32u;

// Linear brightness (SDR white = 1) above which the highlights are expanded: high, so large
// bright areas (fog, sky) stay near paper white and only lamps, fire and sparks reach the peak.
const float KNEE = 0.8;

// BT.709 -> BT.2020 primaries (columns).
const mat3 BT709_TO_BT2020 = mat3(
    0.6274, 0.0691, 0.0164,
    0.3293, 0.9195, 0.0880,
    0.0433, 0.0114, 0.8956);

// The game's picture is made for SDR displays, which show it with a 2.2 power curve. The
// piecewise sRGB curve (what Windows uses for SDR content in HDR) lifts the shadows: dark
// scenes turn grey and foggy. Bloodborne is mostly shadows, so 2.2 it is.
vec3 SdrToLinear(vec3 c) {
    return pow(max(c, vec3(0.0)), vec3(2.2));
}

// SMPTE ST 2084 inverse EOTF; y: luminance / 10000 nits.
vec3 PqEncode(vec3 y) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    vec3 ym = pow(clamp(y, 0.0, 1.0), vec3(m1));
    return pow((c1 + c2 * ym) / (1.0 + c3 * ym), vec3(m2));
}

// Inverse tone map of the brightest channel (hue and saturation kept): unchanged up to KNEE,
// then a curve with the same slope there that reaches `white` at SDR white.
vec3 ExpandHighlights(vec3 rgb, float white) {
    float m = max(rgb.r, max(rgb.g, rgb.b));
    if (white <= 1.0 || m <= KNEE) {
        return rgb;
    }
    float t = (m - KNEE) / (1.0 - KNEE);
    float expanded = m + (white - 1.0) * t * t;
    return rgb * (expanded / m);
}

// Debug view colours: blue (a little gain) .. cyan .. green .. yellow .. red (the peak).
vec3 Heat(float t) {
    t = clamp(t, 0.0, 1.0);
    return clamp(vec3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0),
                      1.5 - abs(4.0 * t - 1.0)), 0.0, 1.0);
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec3 game_srgb = texelFetch(game, pixel, 0).rgb;
    float peak_white = max(hdr.peak / hdr.paper_white, 1.0);
    float white = max(mix(1.0, peak_white, clamp(hdr.boost, 0.0, 1.0)), 1.0);
    vec3 sdr_linear = SdrToLinear(game_srgb);
    vec3 linear;
    if ((hdr.flags & FLAG_NATIVE16) != 0u) {
        // The 16-bit chain's frame: the game's encoded picture with the HDR delta above its
        // white. Decoded here with the same 2.2; nothing expanded (the scene is already as
        // bright as the chain made it). The black floor (the menu's slider, OLED): the darkest
        // greys down to true black, the rest of the range stretched to keep its white.
        linear = sdr_linear;
        if (hdr.black_floor > 0.0) {
            linear = max(linear - vec3(hdr.black_floor), vec3(0.0)) / (1.0 - hdr.black_floor);
        }
        // The HUD (the mask in the snapshot's alpha): shown at UI Brightness — its pixels
        // brightened by the HUD's white over the game's — while the scene stays as it is.
        if ((hdr.flags & FLAG_UI_MASK) != 0u) {
            float ui = texture(snapshot, gl_FragCoord.xy / vec2(textureSize(snapshot, 0))).a;
            ui = clamp(ui, 0.0, 1.0);
            linear = mix(linear, linear * max(hdr.ui_white, 0.0), ui);
        }
    } else {
        linear = ExpandHighlights(sdr_linear, white);
    }
    if ((hdr.flags & FLAG_DEBUG) != 0u) {
        // The picture in grey at a third of paper white, the expansion over it from blue (a
        // little) to red (the peak); in native+ the values above the game's white are the
        // chain's, shown from blue up the same way.
        float gain = max(linear.r, max(linear.g, linear.b)) /
                     max(max(sdr_linear.r, max(sdr_linear.g, sdr_linear.b)), 1e-4);
        vec3 shown = vec3(dot(sdr_linear, vec3(0.2126, 0.7152, 0.0722)) * 0.33);
        if (gain > 1.001) {
            shown = mix(shown, Heat(log2(gain) / max(log2(peak_white), 0.1)), 0.8);
        }
        linear = shown;
    }
    if ((hdr.flags & FLAG_COMPARE) != 0u && (hdr.flags & FLAG_DEBUG) == 0u) {
        // The left half as an SDR display shows the game (at paper white, sRGB's colours), a
        // thin grey line between the two. In native+ the values are clamped back into the
        // game's range: that is what an SDR display would show of it.
        const float middle = 0.5 * float(textureSize(game, 0).x);
        if (gl_FragCoord.x < middle) {
            linear = min(sdr_linear, vec3(1.0));
        }
        if (abs(gl_FragCoord.x - middle) < 1.5) {
            linear = vec3(0.5);
        }
    }
    if ((hdr.flags & FLAG_OVERLAY) != 0u) {
        // The menu over the frame at paper white: the menu is not expanded. In native+ the
        // frame under it is already linear (and possibly above white), so the menu's own sRGB
        // pixels are decoded and blended over it in the light.
        vec4 menu = texelFetch(overlay, pixel, 0);
        if (menu.a > 0.0) {
            vec3 under = (hdr.flags & FLAG_NATIVE16) != 0u ? linear : SdrToLinear(clamp(game_srgb, 0.0, 1.0));
            vec3 menu_linear = SdrToLinear(clamp(menu.rgb, 0.0, 1.0));
            linear = mix(under, menu_linear, clamp(menu.a * 4.0, 0.0, 1.0));
        }
    }
    if ((hdr.flags & FLAG_SCRGB) != 0u) {
        color = vec4(linear * (hdr.paper_white / 80.0), 1.0);
    } else {
        vec3 nits = max(BT709_TO_BT2020 * linear, vec3(0.0)) * hdr.paper_white;
        color = vec4(PqEncode(nits / 10000.0), 1.0);
    }
}
