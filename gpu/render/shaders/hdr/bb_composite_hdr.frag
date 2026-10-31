// SPDX-FileCopyrightText: Copyright 2026 bbport
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport HDR, generic pass (BB_HDR_TONEMAP=4, docs/HDR.md step 3c): the HDR side channel for any
// of the 1380 standard variants of Bloodborne's final composite (bb_tonemap_variants.h). The game
// draws its composite unchanged; this shader replays the same draw into bbport's RGBA16F channel
// image. Its interface is the game shader's: when bbport recompiles a composite variant,
// HdrChannel copies that variant's descriptor bindings, input locations and constant-buffer slot
// onto the decorations named bb_* here, and sets the variant's options as specialization
// constants (vk_hdr_channel.cpp). The math is hdr/bb_composite.glsl (checked against the game's
// code, see there).
//
// Output (RGBA16F), as hdr/bb_tonemap_hdr.frag: r, b, a = gains of red, green and blue from the
// game's color to the untonemapped one (post_process.frag grades and tonemaps it live), g =
// luminance of the game's encoded output (to tell the scene from the UI drawn over it). Additive
// variants (no curve: HDR output tonemapped by a later pass) and curves without a knee get
// gains of 1.

#version 450
#ifndef BB_HW_INTERP
#extension GL_EXT_fragment_shader_barycentric : require
#endif
#ifdef BB_CB_UBO
#extension GL_EXT_scalar_block_layout : require
#endif
#extension GL_GOOGLE_include_directive : require

#include "bb_inputs.glsl"

layout(constant_id = 1) const float BB_HDR_PEAK = 4.926;
layout(constant_id = 2) const float BB_HDR_KNEE = 0.6;
layout(constant_id = 10) const int BB_CURVE = 0;
layout(constant_id = 11) const bool BB_LUMINANCE = false;
layout(constant_id = 12) const int BB_PRE = 0;
layout(constant_id = 13) const bool BB_PRE_LUM = false;
layout(constant_id = 14) const bool BB_COS4 = false;
layout(constant_id = 15) const bool BB_VIG888 = false;
layout(constant_id = 16) const bool BB_GAMMA = true;
layout(constant_id = 17) const bool BB_ADDITIVE = false;
layout(constant_id = 18) const uint BB_BUFFER_INDEX = 0u; // index of the constant buffer in buf_offsets
layout(constant_id = 20) const bool BB_GAIN = true;
layout(constant_id = 21) const bool BB_FLOOR_ON = true;
layout(constant_id = 22) const bool BB_SCENE_MAT = false;
layout(constant_id = 23) const bool BB_GLOW_MAT = false;
layout(constant_id = 24) const bool BB_GRADE = true;
// Mode 5 (the 16-bit chain): drawn right after the game's composite into its own (RGBA16F)
// target with additive blending and color writes only: frag_color0 = the HDR output minus the
// game's (both encoded as the game encodes, before its grain), so the target ends up with the
// HDR color plus the game's grain and alpha.
layout(constant_id = 25) const bool BB_OUTPUT_DELTA = false;

// The game shader's constant buffer (decorations copied from the variant): a storage buffer from
// the legacy shader compiler (ssbo_N); BB_CB_UBO, a uniform buffer of 16384 dwords with a 4-byte
// stride from Lantern's native one (ubo_N, Profile::uniform_dword_layout).
#ifdef BB_CB_UBO
layout(set = 0, binding = 0, scalar) uniform bb_ssbo_block {
    uint data[16384];
} bb_ssbo;
#else
layout(set = 0, binding = 0, std430) readonly buffer bb_ssbo_block {
    uint data[];
} bb_ssbo;
#endif

// The push constants of every shader bbport recompiles.
// The recompiler's push constants (shader_recompiler/resource.h, PushData) at the C++ struct's
// offsets (checked in vk_rasterizer.cpp); bb_hdr is bbport's, set per draw (vk_rasterizer.cpp).
layout(push_constant, std430) uniform AuxData {
    layout(offset = 0) float xoffset;
    layout(offset = 4) float yoffset;
    layout(offset = 8) float xscale;
    layout(offset = 12) float yscale;
    layout(offset = 80) uvec4 buf_offsets0;
    layout(offset = 96) uvec4 buf_offsets1;
    layout(offset = 112) uvec2 buf_offsets2;
    layout(offset = 120) uint motion_param;
    layout(offset = 124) uint scene_size;
    layout(offset = 128) uvec4 bb_hdr;
    // Mode 5, PsychoV30 (scene): x the display's peak (game white units), y exposure,
    // z highlights, w shadows; bb_hdr2b: x contrast, y saturation (bits plus one, 0: not set),
    // z the target gamut (PsychoV30's gamut_compression_mode: 0 BT.709, 1 BT.2020), + 2 for
    // PsychoV30 (pure), + 4 for Vanilla+, plus one (0: not set); w PsychoV30's gamut
    // compression and compression.
    layout(offset = 144) uvec4 bb_hdr2a;
    layout(offset = 160) uvec4 bb_hdr2b;
} push_data;

layout(set = 0, binding = 1) uniform texture2D bb_img_scene;
layout(set = 0, binding = 2) uniform texture2D bb_img_mult;
layout(set = 0, binding = 3) uniform texture2D bb_img_bloom;
layout(set = 0, binding = 4) uniform sampler bb_samp_scene;
layout(set = 0, binding = 5) uniform sampler bb_samp_mult;
layout(set = 0, binding = 6) uniform sampler bb_samp_bloom;

// The game's inputs, per vertex or interpolated by the GPU (bb_inputs.glsl); their locations are
// the variant's (vk_hdr_channel.cpp).
BB_INPUT(0, bb_in_scene);
BB_INPUT(1, bb_in_mult);
BB_INPUT(2, bb_in_bloom);
BB_INPUT(3, bb_in_vig);

layout(location = 0) out vec4 frag_color0;
// The game's effects as ratios, for the live sliders of the final pass: rgb = the untonemapped
// color without the bloom (glow) over the color (per row: red, green, blue), a = the luminance
// without the vignettes over with (>= 1). 1 where there is nothing to change.
layout(location = 1) out vec4 frag_aux;

uint buf_off;

float bb_c(uint word) {
    return uintBitsToFloat(bb_ssbo.data[word + buf_off]);
}

#include "bb_composite.glsl"
#include "psychov30.glsl"

float bb_bits(uint bits, float fallback) {
    return bits == 0u ? fallback : uintBitsToFloat(bits - 1u);
}

// The chain's gamma encoding (`power`, the game's 1/2.2) with the sign kept for channels below 0
// (BT.2020 colors outside BT.709): exactly the unsigned encoding for the others.
vec3 bb_encode_signed(vec3 v, float power) {
    vec3 positive = exp2(power * log2(max(v, 0.0) + BB_EPS));
    vec3 negative = exp2(power * log2(max(-v, 0.0) + BB_EPS)) - exp2(power * log2(BB_EPS));
    return positive - negative;
}

// The value plus one in `word` (0: not set, `fallback`).
int bb_choice(uint word, int fallback) {
    return word == 0u ? fallback : int(word - 1u);
}

// Mode 5, tone mapper PsychoV30 (scene): PsychoV30 alone on the scene's linear color, in place of
// the game's curve, to the display's peak (the final pass then only encodes). Anchored at
// the game's mid grey, with its slope there as contrast. The menu's exposure, highlights,
// shadows, contrast and saturation are PsychoV30's own parameters, and its target gamut
// (BT.2020 by default, as RenoDX's HDR mods; BT.709). PsychoV30 (pure): the same, without the
// match of the shadows to the game's.
bool bb_curve_psychov_scene(BbComposite o, vec3 x, out vec3 t) {
    float grey_in, slope;
    if (!bb_knee(o.curve, 0.18, grey_in, slope)) {
        t = bb_curve(o, x);
        return false;
    }
    float peak = max(bb_bits(push_data.bb_hdr2a.x, BB_HDR_PEAK), 1.0);
    float exposure = bb_bits(push_data.bb_hdr2a.y, 1.0);
    float highlights = bb_bits(push_data.bb_hdr2a.z, 1.0);
    float shadows = bb_bits(push_data.bb_hdr2a.w, 1.0);
    float game_contrast = clamp(grey_in * slope / 0.18, 0.3, 3.0);
    float user_contrast = bb_bits(push_data.bb_hdr2b.x, 1.0);
    float contrast = game_contrast * user_contrast;
    float purity = bb_bits(push_data.bb_hdr2b.y, 1.0);
    // Both calls below take the same gamut and compressions: the shadow match's ratio compares
    // them. bb_hdr2b.z: the gamut + 2 for the pure mode, + 1; bb_hdr2b.w: gamut compression and
    // compression in 1/10000, bit 31 set (0: RenoDX's defaults, 1 and 1).
    int choice = bb_choice(push_data.bb_hdr2b.z, 1);
    int gamut = clamp(choice & 1, 0, 1);
    bool pure = (choice & 2) != 0;
    uint packed = push_data.bb_hdr2b.w;
    float gamut_compression = packed == 0u ? 1.0 : float(packed & 0xFFFFu) / 10000.0;
    float compression = packed == 0u ? 1.0 : float((packed >> 16u) & 0x7FFFu) / 10000.0;
    vec3 game = bb_curve(o, x);
    if (o.curve != 3) {
        x = max(x, BB_FLOOR); // PsychoV30 needs a positive input
    }
    t = rd_psychov30(x, peak, exposure, highlights, shadows, max(contrast, 0.05), purity, 1.0,
                     vec3(grey_in), vec3(0.18), gamut_compression, gamut, compression,
                     RD_PSY30_SOURCE_BOUNDARY_BT709);
    // BT.2020: a color outside BT.709 has BT.709 channels below 0. They are kept, signed, through
    // the 16-bit chain to the final pass, which converts to BT.2020 before it drops what is still
    // negative (as RenoDX's mods). BT.709: within the cube, as before.
    if (gamut == 0) {
        t = max(t, 0.0);
    }
    if (pure) {
        return true; // PsychoV30 (pure): its own shadows
    }
    // Below mid grey, the game's own luminance (its curve's toe lifts the shadows, PsychoV30's
    // single power at mid grey does not), PsychoV30's color kept; up to mid grey, PsychoV30 alone.
    // The sliders still apply (the game's value scaled as they move PsychoV30's).
    const vec3 luma = vec3(0.2126390059, 0.7151686788, 0.0721923154);
    float l_game = dot(max(game, 0.0), luma);
    float l_t = dot(t, luma);
    if (l_t > 1e-6 && l_game < 0.18) {
        // The sliders' effect (exposure, shadows, contrast) as a ratio on PsychoV30's own output.
        float grade = 1.0;
        if (exposure != 1.0 || highlights != 1.0 || shadows != 1.0 || purity != 1.0 ||
            user_contrast != 1.0) {
            vec3 base = rd_psychov30(x, peak, 1.0, 1.0, 1.0, game_contrast, 1.0, 1.0,
                                     vec3(grey_in), vec3(0.18), gamut_compression, gamut,
                                     compression, RD_PSY30_SOURCE_BOUNDARY_BT709);
            float l_base = dot(max(base, 0.0), luma);
            grade = l_base > 1e-6 ? l_t / l_base : 1.0;
        }
        float match = l_game * grade / l_t;
        t *= mix(match, 1.0, smoothstep(0.05, 0.18, l_game));
    }
    return true;
}

// Mode 5, tone mapper Vanilla+, as RenoDX's Bloodborne mod (souperman9) sets it: the game's curve
// below an input of mid grey (0.18), its tangent above it (curve 0: the tangent of 1 - e^-kv, the
// toe after it, as the mod), the bloom added (no screen blend), then RenoDX's user color grading
// and CInfinityRollOff per channel to the display's peak (bb_vanilla_plus_rows).
float bb_vanilla_plus_f(int curve, float v) {
    const float anchor = 0.18;
    if (v < anchor) {
        return bb_f(curve, v);
    }
    if (curve == 0) {
        float k = bb_c(832u);
        float e = exp2(-(anchor * k) * 1.44269502162933349609375);
        float y = (1.0 - e) + (v - anchor) * k * e;
        float c = bb_c(833u) * (y - 1.0) + 1.0;
        return y * c * c;
    }
    const float h = 1e-3;
    float slope = (bb_f(curve, anchor + h) - bb_f(curve, anchor - h)) / (2.0 * h);
    return bb_f(curve, anchor) + (v - anchor) * max(slope, 0.0);
}

vec3 bb_curve_vanilla_plus(BbComposite o, vec3 x) {
    if (o.curve == 3) {
        return max(x, 0.0);
    }
    x = max(x, o.floor ? BB_FLOOR : 0.0);
    if (o.luminance) {
        float l = bb_mean(x);
        return x * (bb_vanilla_plus_f(o.curve, l) / (l + BB_EPS));
    }
    return vec3(bb_vanilla_plus_f(o.curve, x.r), bb_vanilla_plus_f(o.curve, x.g),
                bb_vanilla_plus_f(o.curve, x.b));
}

// RenoDX's CInfinityRollOff (tonemap/c_infinity_rolloff.hlsl): the value below `anchor`,
// tending to `peak` above it.
vec3 bb_c_infinity_rolloff(vec3 c, float peak) {
    const float anchor = 0.18, strength = 1.5;
    float range = peak - anchor;
    vec3 t;
    for (int i = 0; i < 3; ++i) {
        float d = max(c[i] - anchor, 0.0);
        if (d <= 0.0) {
            t[i] = c[i];
            continue;
        }
        float inverse_position = range / (strength * d);
        t[i] = range / (strength * inverse_position + exp2(-inverse_position)) + c[i] - d;
    }
    return t;
}

vec3 bb_oklab(vec3 rgb) {
    vec3 lms = vec3(dot(rgb, vec3(0.4122214615, 0.5363325477, 0.0514459945)),
                    dot(rgb, vec3(0.2119034976, 0.6806995273, 0.1073969603)),
                    dot(rgb, vec3(0.0883024633, 0.2817188501, 0.6299787164)));
    lms = sign(lms) * pow(abs(lms), vec3(1.0 / 3.0));
    return vec3(dot(lms, vec3(0.2104542553, 0.7936177850, -0.0040720468)),
                dot(lms, vec3(1.9779984951, -2.4285922050, 0.4505937099)),
                dot(lms, vec3(0.0259040371, 0.7827717662, -0.8086757660)));
}

vec3 bb_oklab_to_rgb(vec3 lab) {
    vec3 lms = vec3(lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z,
                    lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z,
                    lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z);
    lms = lms * lms * lms;
    return vec3(dot(lms, vec3(4.0767416621, -3.3077115913, 0.2309699292)),
                dot(lms, vec3(-1.2684380046, 2.6097574011, -0.3413193965)),
                dot(lms, vec3(-0.0041960863, -0.7034186147, 1.7076147010)));
}

// RenoDX's user color grading as the Bloodborne mod applies it before its roll-off (exposure,
// contrast, highlights, shadows on the luminance around mid grey; the new lightness with the
// original hue in OKLab; saturation), then no color below 0 in BT.2020.
vec3 bb_renodx_user_grade(vec3 c, float exposure, float highlights, float shadows,
                          float contrast, float saturation) {
    vec3 exposed = c * exposure;
    float y = dot(exposed, vec3(0.2126390059, 0.7151686788, 0.0721923154));
    float scale = 0.0;
    if (y > 0.0) {
        float a = pow(y / 0.18, contrast);
        float b = mix(a, pow(a, highlights), clamp(a, 0.0, 1.0));
        scale = mix(pow(b, 2.0 - shadows), b, clamp(b, 0.0, 1.0)) * 0.18 / y;
    }
    vec3 graded = bb_oklab(exposed * scale);
    vec3 original = bb_oklab(c);
    float chroma = length(original.yz);
    vec2 ab = chroma > 0.0 ? original.yz * (length(graded.yz) / chroma) : graded.yz;
    vec3 rgb = bb_oklab_to_rgb(vec3(graded.x, ab * saturation));
    vec3 bt2020 = max(vec3(dot(rgb, vec3(0.6130974024, 0.3395231462, 0.0473794514)),
                           dot(rgb, vec3(0.0701937225, 0.9163538791, 0.0134523985)),
                           dot(rgb, vec3(0.0206155929, 0.1095697729, 0.8698146343))),
                      0.0);
    return vec3(dot(bt2020, vec3(1.7050509927, -0.6217921207, -0.0832588720)),
                dot(bt2020, vec3(-0.1302564175, 1.1408047366, -0.0105483191)),
                dot(bt2020, vec3(-0.0240033568, -0.1289689761, 1.1529723430)));
}

// Vanilla+ after the game's color matrix (rows in RGB order): the menu's grading, then the
// roll-off to the display's peak (in units of the game's white).
vec3 bb_vanilla_plus_rows(vec3 rows) {
    rows = max(rows, 0.0);
    float exposure = bb_bits(push_data.bb_hdr2a.y, 1.0);
    float highlights = bb_bits(push_data.bb_hdr2a.z, 1.0);
    float shadows = bb_bits(push_data.bb_hdr2a.w, 1.0);
    float contrast = bb_bits(push_data.bb_hdr2b.x, 1.0);
    float saturation = bb_bits(push_data.bb_hdr2b.y, 1.0);
    if (exposure != 1.0 || highlights != 1.0 || shadows != 1.0 || contrast != 1.0 ||
        saturation != 1.0) {
        rows = bb_renodx_user_grade(rows, exposure, highlights, shadows, contrast, saturation);
    }
    float peak = max(bb_bits(push_data.bb_hdr2a.x, BB_HDR_PEAK), 1.0);
    return bb_c_infinity_rolloff(rows, peak);
}

#ifndef BB_HW_INTERP
// The game's interpolation, as recompiled: attribute with the barycentrics.
vec2 interp(vec4 a0, vec4 a1, vec4 a2) {
    vec3 b = gl_BaryCoordEXT;
    return vec2(fma(a2.x - a0.x, b.z, fma(a1.x - a0.x, b.y, a0.x)),
                fma(a2.y - a0.y, b.z, fma(a1.y - a0.y, b.y, a0.y)));
}
#define BB_IN_UV(NAME) interp(NAME[0], NAME[1], NAME[2])
#else
#define BB_IN_UV(NAME) BB_UV(NAME)
#endif

void main() {
    // buf_offsets holds one byte per buffer binding, as the recompiler reads it:
    // buf_offsets0 = buffers 0..15, buf_offsets1 = 16..31.
    uvec4 offsets = BB_BUFFER_INDEX < 16u ? push_data.buf_offsets0 : push_data.buf_offsets1;
    uint slot = BB_BUFFER_INDEX & 15u;
    buf_off = bitfieldExtract(offsets[slot >> 2u], int((slot & 3u) * 8u), 8) >> 2u;

    BbComposite o = BbComposite(BB_CURVE, BB_LUMINANCE, BB_PRE, BB_PRE_LUM, BB_COS4, BB_VIG888,
                                BB_GAMMA, BB_ADDITIVE, BB_GAIN, BB_FLOOR_ON, BB_SCENE_MAT,
                                BB_GLOW_MAT, BB_GRADE);
    // hdr_nograde (a diagnostic): bb_hdr.x bit 4 skips the game's colour matrix, to see whether a
    // channel it drives to zero is what removes part of the image (the scene losing its red).
    if ((push_data.bb_hdr.x & 16u) != 0u) {
        o.grade = false;
    }

    vec4 scene = textureLod(sampler2D(bb_img_scene, bb_samp_scene), BB_IN_UV(bb_in_scene), 0.0);
    vec3 mult = texture(sampler2D(bb_img_mult, bb_samp_mult), BB_IN_UV(bb_in_mult)).rgb;
    vec3 bloom = textureLod(sampler2D(bb_img_bloom, bb_samp_bloom), BB_IN_UV(bb_in_bloom), 0.0).rgb;
    vec2 vig_uv = (BB_COS4 || BB_VIG888) ? BB_IN_UV(bb_in_vig) : vec2(0.0);

    // The game's output, and the same with the HDR curve.
    vec3 ms = bb_scene_tint(mult);
    vec3 x = bb_exposed(o, scene, ms, vig_uv);
    vec3 base = bb_bloom_base(mult);
    vec3 sdr_t = bb_curve(o, x);
    vec3 hdr_t;
    // Mode 5: the knee live (in-game menu, Highlight range; bb_hdr.y set for this draw by
    // bbport, the fraction's bits plus one; 0 = the default).
    float knee = BB_HDR_KNEE;
    if (BB_OUTPUT_DELTA && push_data.bb_hdr.y != 0u) {
        knee = clamp(uintBitsToFloat(push_data.bb_hdr.y - 1u), 0.1, 0.95);
    }
    bool psycho_scene = BB_OUTPUT_DELTA && push_data.bb_hdr2a.x != 0u;
    bool vanilla_plus = psycho_scene && (bb_choice(push_data.bb_hdr2b.z, 1) & 4) != 0;
    bool has_hdr = false;
    if (!BB_ADDITIVE && vanilla_plus) {
        hdr_t = bb_curve_vanilla_plus(o, x);
        has_hdr = true;
    } else if (!BB_ADDITIVE) {
        has_hdr = psycho_scene ? bb_curve_psychov_scene(o, x, hdr_t)
                               : bb_curve_hdr(o, x, knee, BB_HDR_PEAK, hdr_t);
    }
    vec3 g = bb_glow(o, base, bloom, ms);
    vec3 glow = clamp(g, 0.0, 1.0);
    vec3 sdr_screen = BB_ADDITIVE ? x + g : sdr_t + glow - sdr_t * glow;
    // Screen blend on the SDR part only (above 1 it would subtract); Vanilla+ adds the bloom.
    vec3 hdr_screen = !has_hdr      ? sdr_screen
                      : vanilla_plus ? max(hdr_t + g, 0.0)
                                     : hdr_t + glow * (1.0 - min(hdr_t, 1.0));

    // The grading matrix result is in the game's export order: components as the target wants.
    vec3 sdr_graded = bb_grade(o, sdr_screen);
    vec3 hdr_graded = vanilla_plus ? bb_vanilla_plus_rows(bb_grade(o, hdr_screen))
                                   : bb_grade(o, hdr_screen);
    if (BB_OUTPUT_DELTA) {
        // The game's bloom, live (in-game menu; bb_hdr.w set for this draw by bbport: the
        // strength's bits plus one, 0 = the game's): the target is the composite with its glow
        // scaled, so the difference can also take light away.
        uint bloom_bits = push_data.bb_hdr.w;
        float k = (bloom_bits == 0u || BB_ADDITIVE) ? 1.0
                                                    : clamp(uintBitsToFloat(bloom_bits - 1u), 0.0, 4.0);
        bool scaled = abs(k - 1.0) > 1e-3;
        vec3 target = has_hdr ? hdr_graded : clamp(sdr_graded, 0.0, 1.0);
        if (scaled) {
            vec3 glow_k = clamp(g * k, 0.0, 1.0);
            vec3 sdr_screen_k = sdr_t + glow_k - sdr_t * glow_k;
            vec3 hdr_screen_k = !has_hdr      ? sdr_screen_k
                                : vanilla_plus ? max(hdr_t + g * k, 0.0)
                                               : hdr_t + glow_k * (1.0 - min(hdr_t, 1.0));
            target = !has_hdr      ? clamp(bb_grade(o, sdr_screen_k), 0.0, 1.0)
                     : vanilla_plus ? bb_vanilla_plus_rows(bb_grade(o, hdr_screen_k))
                                    : bb_grade(o, hdr_screen_k);
        }
        // The game's own output in the target (the delta goes over it), and the HDR one.
        vec3 game_out = BB_GAMMA ? exp2(bb_c(808u) * log2(clamp(sdr_graded + BB_EPS, 0.0, 1.0)))
                                 : clamp(sdr_graded, 0.0, 1.0);
        vec3 hdr_out = game_out;
        vec3 delta = vec3(0.0);
        if (has_hdr || scaled) {
            // PsychoV30 (scene) with a BT.2020 target: channels below 0 kept, signed (the
            // target holds the game's color plus this delta: RGBA16F, additive blending).
            if (BB_GAMMA) {
                hdr_out = psycho_scene ? bb_encode_signed(target, bb_c(808u))
                                       : exp2(bb_c(808u) * log2(max(target, 0.0) + BB_EPS));
            } else {
                hdr_out = psycho_scene ? target : max(target, 0.0);
            }
            delta = hdr_out - game_out;
        }
        // Chain probe 6 (test): the delta itself. The target already holds the game's own colour,
        // so writing 0.5 - game_out lands on mid grey where our reconstruction matches it, and
        // shows the error (bright, dark, coloured) everywhere else — amplified 4x.
        if (push_data.bb_hdr.z == 6u) {
            frag_color0 = vec4(vec3(0.5) - game_out + 4.0 * delta, 0.0);
            return;
        }
        // Chain probe 4 (test): the chain's own input colour in place of the scene. Red missing
        // here = the sampled textures are wrong; red present = the maths after them is.
        if (push_data.bb_hdr.z == 4u) {
            vec3 in_enc = BB_GAMMA ? exp2(bb_c(808u) * log2(max(x, 0.0) + BB_EPS))
                                   : clamp(x, 0.0, 1.0);
            frag_color0 = vec4(in_enc - game_out, 0.0);
            return;
        }
        // PsychoV30 (scene) replaces the game's curve, so it may also darken.
        // The guest target's component mapping (A8B8G8R8: red and blue exchange): the recompiler
        // swizzles the game shaders' writes, so this pass must too — written raw, the delta went
        // into the opposite channels and the whole scene looked blue while the UI (drawn by the
        // game's own shaders) stayed right.
        vec3 delta_out = (scaled || psycho_scene) ? delta : max(delta, 0.0);
        frag_color0 = vec4(delta_out.bgr, 0.0);
        // Chain probe (in-game menu, test; bb_hdr.z set for this draw by bbport): green where
        // the HDR image is above the game's white (PsychoV30), or where this draw adds light
        // to it (the other tone mappers). The delta makes the target exactly green at white: a
        // plain, positive color the passes after (the color table's gamut bridge, the upscalers)
        // keep green (pushing red and blue far below 0 turned it black through them).
        bool hdr_part = psycho_scene ? max(max(hdr_out.x, hdr_out.y), hdr_out.z) > 1.001
                                     : max(max(delta.x, delta.y), delta.z) > 0.01;
        if (push_data.bb_hdr.z == 1u && hdr_part) {
            frag_color0 = vec4(vec3(0.0, 1.0, 0.0) - game_out, 0.0);
        } else if (max(max(abs(frag_color0.x), abs(frag_color0.y)), abs(frag_color0.z)) <
                   1.0 / 4096.0) {
            // Nothing to add (below a sixteenth of an 8-bit step, encoded): no blend, so no
            // read and write of the 16-bit target for this pixel (the shadows, most of a frame).
            discard;
        }
        // Mode 5 has a single color target; the side-channel ratios below are not used.
        frag_aux = vec4(1.0);
        return;
    }

    // Mode 4 uses a separate gain channel instead of changing the game's target.
    const vec3 luma_bgr = vec3(0.0721923154, 0.7151686788, 0.2126390059);
    float encode = BB_GAMMA ? bb_c(808u) : 1.0 / 2.2;
    vec3 sdr_encoded = exp2(encode * log2(clamp(sdr_graded + BB_EPS, 0.0, 1.0)));
    float sdr_luma = dot(max(sdr_graded, 0.0), luma_bgr);
    float hdr_luma = dot(max(hdr_graded, 0.0), luma_bgr);
    // A gain per channel, from the color matrix's rows (red, green, blue): the untonemapped
    // color over the game's (a saturated light keeps its color above white); where the game's
    // channel is near zero, the luminance gain. The final pass grades and tonemaps (BB_HDR_PEAK
    // is 10000 nits here: the curve is the game's slope continued).
    float gain = (has_hdr && sdr_luma > 1e-4) ? hdr_luma / sdr_luma : 1.0;
    vec3 gains = vec3(clamp(gain, 1.0, 64.0));
    if (has_hdr) {
        vec3 sdr_rows = max(bb_grade(o, sdr_screen), 0.0);
        vec3 hdr_rows = max(bb_grade(o, hdr_screen), 0.0);
        for (int i = 0; i < 3; ++i) {
            gains[i] = sdr_rows[i] > 0.02 ? hdr_rows[i] / sdr_rows[i] : gains[i];
        }
        gains = clamp(gains, 0.0, 64.0);
    }
    // r, b, a: gains of red, green, blue; g: the reference luminance (scene / UI test).
    frag_color0 = vec4(gains.r, dot(sdr_encoded, luma_bgr), gains.g, gains.b);

    // Effect ratios, on the untonemapped color (the final pass scales it before its grading).
    vec4 aux = vec4(1.0);
    if (has_hdr) {
        vec3 rows = max(bb_grade(o, hdr_screen), 0.0);
        vec3 rows_no_bloom = max(bb_grade(o, hdr_t), 0.0);
        for (int i = 0; i < 3; ++i) {
            aux[i] = rows[i] > 1e-4 ? clamp(rows_no_bloom[i] / rows[i], 0.0, 1.0) : 1.0;
        }
        if (BB_COS4 || BB_VIG888) {
            BbComposite no_vig = o;
            no_vig.cos4 = false;
            no_vig.vig888 = false;
            vec3 x_nv = bb_exposed(no_vig, scene, ms, vig_uv);
            vec3 t_nv;
            if (bb_curve_hdr(o, x_nv, knee, BB_HDR_PEAK, t_nv)) {
                vec3 screen_nv = t_nv + glow * (1.0 - min(t_nv, 1.0));
                float l_nv = dot(max(bb_grade(o, screen_nv), 0.0), vec3(0.2126390059, 0.7151686788,
                                                                       0.0721923154));
                float l = dot(rows, vec3(0.2126390059, 0.7151686788, 0.0721923154));
                aux.a = l > 1e-4 ? clamp(l_nv / l, 1.0, 16.0) : 1.0;
            }
        }
    }
    frag_aux = aux;
}
