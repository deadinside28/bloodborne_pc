// SPDX-License-Identifier: MIT
// bbport: compile check of hdr/renodx.glsl (every function is used once). Not part of the game;
// compiled by .github/workflows/shader-check.yml.

#version 450
#extension GL_GOOGLE_include_directive : require

#include "renodx.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D tex;

void main() {
    vec3 c = texture(tex, uv).rgb * 4.0;
    vec3 sdr = clamp(c, 0.0, 1.0);
    float x = c.r;
    float n = rd_neutwo(x) + rd_neutwo(x, 10.0) + rd_neutwo(x, 10.0, 100.0) +
              rd_neutwo(x, 10.0, 100.0, 0.18) + rd_neutwo(x, 10.0, 100.0, 0.18, 0.2) +
              rd_neutwo(x, 10.0, 100.0, 0.18, 0.2, 0.001) + rd_neutwo_inverse(0.5) +
              rd_neutwo_inverse(0.5, 10.0) + rd_neutwo_inverse(0.5, 10.0, 100.0);
    vec3 t = rd_neutwo_bt709(c, 10.0, 100.0) + rd_neutwo_max_channel(c, 10.0, 100.0) +
             rd_neutwo_per_channel(c, vec3(10.0), vec3(100.0));
    vec3 g = rd_upgrade_tonemap(c, sdr, sdr * 0.9, 1.0, 0.0);
    vec3 s = rd_bt709_from_bt2020(rd_bt2020_from_bt709(g)) + rd_y_bt2020(t);
    vec3 pq = rd_pq_encode(s * 100.0);
    color = vec4(rd_pq_decode(pq) / 1000.0 + vec3(n), 1.0);
}
