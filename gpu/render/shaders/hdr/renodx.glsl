// SPDX-FileCopyrightText: Copyright (C) 2026 Carlos Lopez (RenoDX), bbport
// SPDX-License-Identifier: MIT
//
// bbport: GLSL port of the parts of RenoDX's shader library (https://github.com/clshortfuse/renodx,
// src/shaders, MIT) used by the HDR tonemap of Bloodborne:
//   - colorimetry: BT.709/BT.2020 luminance, BT.709 <-> BT.2020 / AP1, OKLab, PQ (ST 2084);
//   - tonemap: Neutwo (all forms) and its inverse, per luminance, max channel or channel;
//   - UpgradeToneMap: carries the game's SDR grade over to the untonemapped HDR color.
// Matrices are written row by row as in RenoDX's HLSL (mul(M, v) = rows dot v): rd_mul() keeps
// that order, so the numbers can be compared with the original files one to one.

#ifndef BBPORT_RENODX_GLSL
#define BBPORT_RENODX_GLSL

// ---------------------------------------------------------------- math

vec3 rd_mul(vec3 r0, vec3 r1, vec3 r2, vec3 v) {
    return vec3(dot(r0, v), dot(r1, v), dot(r2, v));
}

float rd_divide_safe(float a, float b, float fallback) {
    return b == 0.0 ? fallback : a / b;
}

vec3 rd_cbrt(vec3 x) {
    return sign(x) * pow(abs(x), vec3(1.0 / 3.0));
}

float rd_max3(vec3 v) {
    return max(max(v.r, v.g), v.b);
}

// ---------------------------------------------------------------- colorimetry (color/rgb.hlsl)

// Second rows of BT709_TO_XYZ_MAT and BT2020_TO_XYZ_MAT.
float rd_y_bt709(vec3 c) {
    return dot(c, vec3(0.2126390059, 0.7151686788, 0.0721923154));
}
float rd_y_bt2020(vec3 c) {
    return dot(c, vec3(0.2627002120, 0.6779980715, 0.0593017165));
}

// BT709_TO_BT2020_MAT and BT2020_TO_BT709_MAT, products of RenoDX's XYZ matrices.
vec3 rd_bt2020_from_bt709(vec3 c) {
    return rd_mul(vec3(0.6274038960, 0.3292830384, 0.0433130657),
                  vec3(0.0690972894, 0.9195403951, 0.0113623156),
                  vec3(0.0163914388, 0.0880133078, 0.8955952532), c);
}
vec3 rd_bt709_from_bt2020(vec3 c) {
    return rd_mul(vec3(1.6604910021, -0.5876411387, -0.0728498633),
                  vec3(-0.1245504746, 1.1328998971, -0.0083494226),
                  vec3(-0.0181507634, -0.1005788981, 1.1187296613), c);
}
vec3 rd_ap1_from_bt709(vec3 c) {
    return rd_mul(vec3(0.6130974024, 0.3395231462, 0.0473794514),
                  vec3(0.0701937225, 0.9163538791, 0.0134523985),
                  vec3(0.0206155929, 0.1095697729, 0.8698146342), c);
}
vec3 rd_bt709_from_ap1(vec3 c) {
    return rd_mul(vec3(1.7050509927, -0.6217921207, -0.0832588720),
                  vec3(-0.1302564175, 1.1408047366, -0.0105483191),
                  vec3(-0.0240033568, -0.1289689761, 1.1529723329), c);
}

// color/clamp.hlsl: clamp a BT.709 color to the AP1 gamut.
vec3 rd_bt709_clamp_ap1(vec3 bt709) {
    return rd_bt709_from_ap1(max(rd_ap1_from_bt709(bt709), 0.0));
}

// color/oklab.hlsl
vec3 rd_oklab_from_bt709(vec3 bt709) {
    vec3 lms = rd_mul(vec3(0.4122214708, 0.5363325363, 0.0514459929),
                      vec3(0.2119034982, 0.6806995451, 0.1073969566),
                      vec3(0.0883024619, 0.2817188376, 0.6299787005), bt709);
    lms = rd_cbrt(lms);
    return rd_mul(vec3(0.2104542553, 0.7936177850, -0.0040720468),
                  vec3(1.9779984951, -2.4285922050, 0.4505937099),
                  vec3(0.0259040371, 0.7827717662, -0.8086757660), lms);
}
vec3 rd_bt709_from_oklab(vec3 oklab) {
    vec3 lms = rd_mul(vec3(1.0, 0.3963377774, 0.2158037573),
                      vec3(1.0, -0.1055613458, -0.0638541728),
                      vec3(1.0, -0.0894841775, -1.2914855480), oklab);
    lms = lms * lms * lms;
    return rd_mul(vec3(4.0767416621, -3.3077115913, 0.2309699292),
                  vec3(-1.2684380046, 2.6097574011, -0.3413193965),
                  vec3(-0.0041960863, -0.7034186147, 1.7076147010), lms);
}

// colorcorrect.hlsl HueOKLab: hue of correct_color, chrominance of incorrect_color.
vec3 rd_hue_oklab(vec3 incorrect_color, vec3 correct_color, float strength) {
    if (strength == 0.0) return incorrect_color;
    vec3 incorrect_lab = rd_oklab_from_bt709(incorrect_color);
    vec3 correct_lab = rd_oklab_from_bt709(correct_color);
    float chrominance_pre = length(incorrect_lab.yz);
    vec2 blended_ab = mix(incorrect_lab.yz, correct_lab.yz, strength);
    float chrominance_post = length(blended_ab);
    blended_ab *= rd_divide_safe(chrominance_pre, chrominance_post, 1.0);
    incorrect_lab.yz = blended_ab;
    return rd_bt709_clamp_ap1(rd_bt709_from_oklab(incorrect_lab));
}

// color/pq.hlsl: ST 2084, input in nits.
vec3 rd_pq_encode(vec3 nits) {
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(m1));
    return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
}
vec3 rd_pq_decode(vec3 pq) {
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    vec3 e = pow(clamp(pq, 0.0, 1.0), vec3(1.0 / m2));
    return pow(max(e - c1, 0.0) / (c2 - c3 * e), vec3(1.0 / m1)) * 10000.0;
}

// ---------------------------------------------------------------- tonemap/neutwo.hlsl
// x / sqrt(x^2 + 1) and its peak / clip / gray / minimum forms; inverse forms below.

float rd_neutwo(float x) {
    return x * inversesqrt(fma(x, x, 1.0));
}
float rd_neutwo(float x, float peak) {
    return peak * x * inversesqrt(fma(x, x, peak * peak));
}
float rd_neutwo(float x, float peak, float clip) {
    float cc = clip * clip, pp = peak * peak;
    return clip * peak * x * inversesqrt(fma(x * x, cc - pp, cc * pp));
}
float rd_neutwo(float x, float peak, float clip, float gray) {
    float cc = clip * clip, pp = peak * peak, gg = gray * gray;
    float cc_minus_gg = cc - gg;
    float denominator_squared = cc_minus_gg * gg * fma(x * x, cc - pp, cc * (pp - gg));
    return peak * gray * x * cc_minus_gg * inversesqrt(denominator_squared);
}
float rd_neutwo(float x, float peak, float clip, float gray_in, float gray_out) {
    float cc = clip * clip, pp = peak * peak, gg = gray_in * gray_in, oo = gray_out * gray_out;
    float cc_minus_gg = cc - gg;
    float denominator_squared = cc_minus_gg * fma(x * x, cc * oo - pp * gg, cc * gg * (pp - oo));
    return peak * gray_out * x * cc_minus_gg * inversesqrt(denominator_squared);
}
float rd_neutwo(float x, float peak, float clip, float gray_in, float gray_out, float minimum) {
    float z = gray_out - minimum, q = peak - minimum;
    float cc = clip * clip, gg = gray_in * gray_in, zz = z * z, qq = q * q;
    float cc_minus_gg = cc - gg;
    float denominator_squared = cc_minus_gg * fma(x * x, cc * zz - qq * gg, cc * gg * (qq - zz));
    return fma(q * z * x * cc_minus_gg, inversesqrt(denominator_squared), minimum);
}

// Exact inverses of the first three forms (RenoDX's gray forms of the inverse are not exact
// inverses of the forward ones, so they are not ported).
float rd_neutwo_inverse(float x) {
    return x * inversesqrt(fma(-x, x, 1.0));
}
float rd_neutwo_inverse(float x, float peak) {
    return peak * x * inversesqrt(fma(-x, x, peak * peak));
}
float rd_neutwo_inverse(float x, float peak, float clip) {
    float cc = clip * clip, pp = peak * peak;
    return clip * peak * x * inversesqrt(fma(-x * x, cc - pp, cc * pp));
}

// Scaled by luminance (hue and saturation kept), by the max channel, or per channel.
vec3 rd_neutwo_bt709(vec3 color, float peak, float clip) {
    float y = rd_y_bt709(color);
    float scale = y != 0.0 ? rd_neutwo(y, peak, clip) / y : 1.0;
    return color * scale;
}
vec3 rd_neutwo_max_channel(vec3 color, float peak, float clip) {
    float m = rd_max3(abs(color));
    float scale = m != 0.0 ? rd_neutwo(m, peak, clip) / m : 1.0;
    return color * scale;
}
vec3 rd_neutwo_per_channel(vec3 color, vec3 peak, vec3 clip) {
    return vec3(rd_neutwo(color.r, peak.r, clip.r), rd_neutwo(color.g, peak.g, clip.g),
                rd_neutwo(color.b, peak.b, clip.b));
}

// ---------------------------------------------------------------- tonemap.hlsl UpgradeToneMap
// untonemapped: linear HDR scene color; tonemapped: the game's (or a neutral) SDR tonemap of it;
// tonemapped_graded: the game's final SDR color. Returns the graded color with the HDR
// luminance the tonemap removed added back, hue matched to the grade.
vec3 rd_upgrade_tonemap(vec3 color_untonemapped, vec3 color_tonemapped,
                        vec3 color_tonemapped_graded, float post_process_strength,
                        float auto_correction) {
    float ratio = 1.0;
    float y_untonemapped = rd_y_bt709(color_untonemapped);
    float y_tonemapped = rd_y_bt709(color_tonemapped);
    float y_tonemapped_graded = rd_y_bt709(color_tonemapped_graded);

    if (y_untonemapped < y_tonemapped) {
        ratio = y_untonemapped / y_tonemapped;
    } else {
        float y_delta = max(0.0, y_untonemapped - y_tonemapped);
        float y_new = y_tonemapped_graded + y_delta;
        ratio = y_tonemapped_graded > 0.0 ? (y_new / y_tonemapped_graded) : 0.0;
    }
    float auto_correct_ratio = mix(1.0, ratio, clamp(y_untonemapped, 0.0, 1.0));
    ratio = mix(ratio, auto_correct_ratio, auto_correction);

    vec3 color_scaled = color_tonemapped_graded * ratio;
    color_scaled = rd_hue_oklab(color_scaled, color_tonemapped_graded, 1.0);
    return mix(color_untonemapped, color_scaled, post_process_strength);
}

#endif // BBPORT_RENODX_GLSL
