// SPDX-FileCopyrightText: Copyright 2026 bbport
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport HDR, mode 5 (the 16-bit chain, docs/HDR.md): replaces the game's PointSampleColor1
// (f7229091), a plain copy with the channel swap, for the chain probe of the in-game menu: with
// probe 5 (bb_hdr.z, set by bbport for this draw), magenta where the copied image is above the
// game's white. Otherwise the same copy. The names are the recompiled shader's:
// vk_hdr_channel.cpp copies its bindings and locations onto these.

#version 450
#ifndef BB_HW_INTERP
#extension GL_EXT_fragment_shader_barycentric : require
#endif
#extension GL_GOOGLE_include_directive : require

#include "bb_inputs.glsl"

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
} push_data;

layout(set = 0, binding = 0) uniform texture2D fs_img0;
layout(set = 0, binding = 1) uniform sampler fs_samp0;

BB_INPUT(0, fs_in_attr0);
layout(location = 0) out vec4 frag_color0;

void main() {
    vec4 c = textureLod(sampler2D(fs_img0, fs_samp0), BB_UV(fs_in_attr0), 0.0);
    // The game's shader is a plain copy WITH the channel swap (its header): ours must convert
    // the same way, or every pass after it carries red and blue exchanged.
    frag_color0 = vec4(c.zyx, c.w);
    if (push_data.bb_hdr.z == 5u && max(max(c.x, c.y), c.z) > 1.001) {
        frag_color0 = vec4(1.0, 0.0, 1.0, c.w);
    }
}
