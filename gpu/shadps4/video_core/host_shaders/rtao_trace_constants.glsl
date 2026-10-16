// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: ray-traced AO, the trace's per-frame constants and the foliage test, shared by the trace
// (rtao_trace_body.glsl) and the pipeline variant's any-hit shader (rtao_trace.rahit).

// Per-frame constants: a uniform buffer (the push-constant minimum is 128 bytes).
layout(binding = 6, std140) uniform Constants {
    vec4 view[3];     // world -> view rows
    vec4 proj;
    vec2 full_size;
    vec2 half_size;
    vec2 jitter;      // viewport shift in render pixels (unjitter the depth position)
    float radius;
    float history;
    uint rays;
    uint frame;
    uint mode;        // 2: primary-ray test t/|P| (0 = miss); 3: facing ratio of the normal;
                      // 4: AO with the rays starting 0.1 further out (self-hit test)
    float snap_far;   // the surface snap runs up to this distance (0 = never)
    float ao_far;     // no AO beyond this distance (the BVH ends a radius later)
    uint normals;     // 0: the G-buffer's shading normal, 1: geometric, from the depth buffer
    float offset;     // extra origin offset along the normal, metres (on top of the ulp offset)
    uint denoise;     // 1: temporal blend here; 2: raw output for rtao_temporal.comp
    uint subpixel;    // 1: the sampled full-res pixel cycles through the 2x2 block per frame
    uint falloff;     // 0: a hit counts 1 - 0.5 t/r; 1: binary hits within a random length in
                      // [0, r] (linear falloff to 0 at the radius, expected value)
    uint period;      // frames before the per-pixel noise repeats (IGN)
    uint noise;       // 1: the blue-noise texture, shifted per frame by the golden ratio
    float foliage;    // > 0: alpha-tested draws (the BLAS's non-opaque geometry) stop a ray with
                      // this probability per hit: a grass card occludes at about its coverage
};
// Foliage (alpha-tested draws, no texture to test against): a hit stops the ray with probability
// `foliage`, drawn anew per pixel, frame, ray and triangle; over the temporal accumulation a card
// occludes at that average coverage instead of as a solid quad.
bool FoliageBlocks(uvec2 pixel, uint primitive, vec3 dir) {
    uint h = pixel.x * 0x8da6b343u ^ pixel.y * 0xd8163841u ^ frame * 0xcb1ab31fu ^ primitive * 0x165667b1u ^
             floatBitsToUint(dir.x) ^ (floatBitsToUint(dir.y) << 7u) ^ (floatBitsToUint(dir.z) << 13u);
    h = (h ^ (h >> 16u)) * 0x7feb352du; // a 32-bit hash finalizer (Wellons' lowbias32)
    h = (h ^ (h >> 15u)) * 0x846ca68bu;
    h ^= h >> 16u;
    return float(h >> 8u) * (1.0 / 16777216.0) < foliage;
}
