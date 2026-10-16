// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: ray-traced AO, pipeline variant: the hit distance into the payload.
#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT float hit_t;

void main() {
    hit_t = gl_HitTEXT;
}
