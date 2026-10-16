// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: ray-traced AO, the trace at half resolution, shared by rtao_trace.comp (ray queries in
// compute) and rtao_trace.rgen (ray-tracing pipeline): cosine-weighted rays from the surface
// against the frame's acceleration structure (view space), distance falloff, temporal blend with
// the previous result reprojected by the camera motion vectors. Needs GL_EXT_ray_query (the
// surface snap and the camera-ray test) and GL_EXT_samplerless_texture_functions.

layout(binding = 0) uniform texture2D depth_tex;   // full res
layout(binding = 1) uniform texture2D normal_tex;  // full res, world normal * 0.5 + 0.5
layout(binding = 2) uniform texture2D motion_tex;  // full res, pixels, previous - current
layout(binding = 3) uniform texture2D prev_tex;    // half res, previous output
layout(binding = 4, rg16f) uniform writeonly image2D out_img; // half res: ao, view z
layout(binding = 5) uniform accelerationStructureEXT scene;

#include "rtao_trace_constants.glsl"
layout(binding = 7) uniform texture2D blue_noise; // 64x64, four independent channels

const float PI = 3.14159265;

float ViewZ(float depth) { return proj.w / min(depth - proj.z, -1e-8); }
vec3 ViewPos(vec2 pixel, float depth) {
    const vec2 uv = (pixel - jitter) / full_size;
    const vec2 ndc = uv * 2.0 - 1.0;
    const float z = ViewZ(depth);
    return vec3(ndc.x * z / proj.x, ndc.y * z / proj.y, z);
}
float IGN(vec2 p) { return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y)); }

// Geometric normal from the depth buffer (four taps, the side with the smaller depth step per
// axis, as GTAO does): no normal map tilting rays into flat walls and floors, and no BVH fetch.
vec3 DepthNormal(ivec2 fp, vec3 P) {
    const ivec2 size = ivec2(full_size);
    vec3 dx = vec3(0.0), dy = vec3(0.0);
    float best_x = 1e30, best_y = 1e30;
    for (int s = -1; s <= 1; s += 2) {
        const ivec2 px = clamp(fp + ivec2(s, 0), ivec2(0), size - 1);
        const float dpx = texelFetch(depth_tex, px, 0).r;
        if (dpx < 1.0) {
            const vec3 Q = ViewPos(vec2(px) + 0.5, dpx);
            const float step_x = abs(Q.z - P.z);
            if (step_x < best_x) {
                best_x = step_x;
                dx = (Q - P) * float(s);
            }
        }
        const ivec2 py = clamp(fp + ivec2(0, s), ivec2(0), size - 1);
        const float dpy = texelFetch(depth_tex, py, 0).r;
        if (dpy < 1.0) {
            const vec3 Q = ViewPos(vec2(py) + 0.5, dpy);
            const float step_y = abs(Q.z - P.z);
            if (step_y < best_y) {
                best_y = step_y;
                dy = (Q - P) * float(s);
            }
        }
    }
    const vec3 n = cross(dx, dy);
    const float len = length(n);
    return len > 1e-12 ? n / len : vec3(0.0);
}

// Self-intersection-safe origin (Waechter and Binder, Ray Tracing Gems ch. 6): an offset of a few
// ulps of the position along the normal, so it scales with the distance from the origin.
vec3 OffsetRay(vec3 p, vec3 n) {
    const float origin = 1.0 / 32.0, float_scale = 1.0 / 65536.0, int_scale = 256.0;
    const ivec3 of_i = ivec3(int_scale * n);
    const vec3 p_i = vec3(intBitsToFloat(floatBitsToInt(p.x) + (p.x < 0.0 ? -of_i.x : of_i.x)),
                          intBitsToFloat(floatBitsToInt(p.y) + (p.y < 0.0 ? -of_i.y : of_i.y)),
                          intBitsToFloat(floatBitsToInt(p.z) + (p.z < 0.0 ? -of_i.z : of_i.z)));
    return vec3(abs(p.x) < origin ? p.x + float_scale * n.x : p_i.x,
                abs(p.y) < origin ? p.y + float_scale * n.y : p_i.y,
                abs(p.z) < origin ? p.z + float_scale * n.z : p_i.z);
}

// The occlusion ray: the hit distance, or -1 on a miss. Each variant (ray query in compute, or a
// ray-tracing pipeline) defines it after including this file.
float OcclusionHit(vec3 origin, vec3 dir, float t_min, float t_max);

void TracePixel(ivec2 hp) {
    if (hp.x >= int(half_size.x) || hp.y >= int(half_size.y)) {
        return;
    }
    // The full-res pixel this half-res texel samples: fixed, or one of the 2x2 block per frame
    // (the temporal accumulation then covers all four and thin features stop aliasing).
    const ivec2 fp = hp * 2 + (subpixel != 0u ? ivec2(frame & 1u, (frame >> 1) & 1u) : ivec2(1));
    const float depth = texelFetch(depth_tex, fp, 0).r;
    if (depth >= 1.0) {
        imageStore(out_img, hp, vec4(1.0, 1e9, 0.0, 0.0));
        return;
    }
    const vec3 P = ViewPos(vec2(fp) + 0.5, depth);
    // Distance from the camera, not view depth: a depth cut is a plane that slid across distant
    // walls as the camera turned.
    const float dist = length(P);
    if (ao_far > 0.0 && dist > ao_far) {
        imageStore(out_img, hp, vec4(1.0, P.z, 0.0, 0.0));
        return;
    }
    vec3 N;
    if (normals == 1u) {
        N = DepthNormal(fp, P);
    } else {
        const vec3 nw = normalize(texelFetch(normal_tex, fp, 0).xyz * 2.0 - 1.0);
        N = normalize(vec3(dot(view[0].xyz, nw), dot(view[1].xyz, nw), dot(view[2].xyz, nw)));
    }
    if (dot(N, N) < 0.5) { // depth normal undefined (isolated pixel): the shading normal
        const vec3 nw = normalize(texelFetch(normal_tex, fp, 0).xyz * 2.0 - 1.0);
        N = normalize(vec3(dot(view[0].xyz, nw), dot(view[1].xyz, nw), dot(view[2].xyz, nw)));
    }
    if (any(isnan(N)) || any(isnan(P)) || any(isinf(P))) { // no normal (sky, UI): nothing to trace
        imageStore(out_img, hp, vec4(1.0, 1e9, 0.0, 0.0));
        return;
    }
    if (mode == 3u) { // how much the stored normal faces the camera: ~1 expected on visible surfaces
        imageStore(out_img, hp, vec4(0.5 + 0.5 * dot(N, -normalize(P)), P.z, 0.0, 0.0));
        return;
    }
    if (dot(N, -P) < 0.0) {
        N = -N;
    }
    if (mode == 2u) {
        const float len = length(P);
        rayQueryEXT probe;
        rayQueryInitializeEXT(probe, scene, gl_RayFlagsCullNoOpaqueEXT,
                              0xFF, vec3(0.0), 0.0, P / len, len * 1.5);
        while (rayQueryProceedEXT(probe)) {
        }
        float hit = 0.0;
        if (rayQueryGetIntersectionTypeEXT(probe, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
            hit = rayQueryGetIntersectionTEXT(probe, true) / len; // ~1 when the BVH matches the depth
        }
        imageStore(out_img, hp, vec4(hit, P.z, 0.0, 0.0));
        return;
    }
    // (Both helper rays skip the foliage: gl_RayFlagsCullNoOpaqueEXT.)
    // Snap the origin onto the BVH where the depth surface and the triangles disagree (depth-biased
    // draws: floor tiles a few centimetres off): a short ray along the view direction around the
    // depth finds the real surface. Optional (snap=0), up to snap_far metres.
    vec3 origin = P;
    if (dist < snap_far) {
        const vec3 dir = P / dist;
        const float window = 0.1 + 0.01 * dist;
        rayQueryEXT snap;
        rayQueryInitializeEXT(snap, scene, gl_RayFlagsCullNoOpaqueEXT, 0xFF, P - dir * window, 0.0, dir,
                              2.0 * window);
        while (rayQueryProceedEXT(snap)) {
        }
        if (rayQueryGetIntersectionTypeEXT(snap, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
            origin = P + dir * (rayQueryGetIntersectionTEXT(snap, true) - window);
        }
    }
    origin = OffsetRay(origin, N) + N * offset;
    // Tangent frame around the final normal.
    const vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    const vec3 T = normalize(cross(up, N));
    const vec3 B = cross(N, T);
    const float t_min = mode == 4u ? 0.1 : 0.0;
    // Noise per pixel, shifted per frame (64 frames for denoiser 2, whose history is longer).
    // (period: from the settings; the shift 5.588 is Jimenez' interleaved gradient noise step.)
    float n0 = IGN(vec2(fp) + float(frame % period) * 5.588238);
    float n1 = IGN(vec2(fp.yx) + float(frame % period) * 1.618034);
    float n2 = IGN(vec2(fp) * 1.37 + float(frame % period) * 3.141593);
    if (noise == 1u) {
        // Blue noise per half-res pixel, each frame shifted by the golden ratio (a low-discrepancy
        // sequence per pixel over time; spatially still blue).
        const vec4 bn = texelFetch(blue_noise, hp & 63, 0);
        const float shift = float(frame & 4095u) * 0.6180339887;
        n0 = fract(bn.r + shift);
        n1 = fract(bn.g + shift);
        n2 = fract(bn.b + shift);
    }

    float occlusion = 0.0;
    for (uint r = 0u; r < rays; ++r) {
        // Cosine-weighted hemisphere, stratified by ray index.
        const float u = fract(n0 + float(r) / float(rays));
        const float v = fract(n1 + float(r) * 0.7548776662);
        const float phi = 2.0 * PI * v;
        const float cos_theta = sqrt(1.0 - u);
        const float sin_theta = sqrt(u);
        const vec3 dir = T * (cos(phi) * sin_theta) + B * (sin(phi) * sin_theta) + N * cos_theta;
        if (falloff == 1u) {
            // A random ray length in [0, r]: a hit at distance t occludes with probability 1 - t/r,
            // and the first hit is all that is needed (TerminateOnFirstHit stays exact).
            const float len = radius * max(fract(n2 + float(r) * 0.6180340), 0.02);
            if (OcclusionHit(origin, dir, t_min, len) >= 0.0) {
                occlusion += 1.0;
            }
        } else {
            const float t = OcclusionHit(origin, dir, t_min, radius);
            if (t >= 0.0) {
                occlusion += 1.0 - clamp(t / radius, 0.0, 1.0) * 0.5; // near hits count more
            }
        }
    }
    float ao = 1.0 - occlusion / float(rays);
    if (ao_far > 0.0) { // fade out over the last quarter: no visible edge
        ao = mix(ao, 1.0, smoothstep(0.75 * ao_far, ao_far, dist));
    }

    if (denoise == 2u) { // rtao_temporal.comp does the rest
        imageStore(out_img, hp, vec4(ao, P.z, 0.0, 0.0));
        return;
    }
    // Temporal: last frame's value where this pixel was. Bilinear over the four half-res
    // texels around the reprojected position, each accepted only where its depth agrees
    // (nearest-texel fetches smeared sharp shadows into a trail), and the result is kept within
    // a band around this frame's value so a stale history cannot linger.
    const vec2 motion = texelFetch(motion_tex, fp, 0).rg;
    const vec2 prev_hpf = (vec2(fp) + 0.5 + motion) / 2.0 - 0.5;
    const ivec2 p0 = ivec2(floor(prev_hpf));
    const vec2 pf = prev_hpf - vec2(p0);
    float hist = 0.0, hist_w = 0.0;
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            const ivec2 pp = p0 + ivec2(i, j);
            if (any(lessThan(pp, ivec2(0))) || any(greaterThanEqual(pp, ivec2(half_size)))) {
                continue;
            }
            const vec2 prev = texelFetch(prev_tex, pp, 0).rg;
            const float wb = (i == 0 ? 1.0 - pf.x : pf.x) * (j == 0 ? 1.0 - pf.y : pf.y);
            const float wd = abs(prev.g - P.z) < 0.03 * P.z ? 1.0 : 0.0;
            hist += prev.r * wb * wd;
            hist_w += wb * wd;
        }
    }
    if (hist_w > 0.5) {
        hist = clamp(hist / hist_w, ao - 0.3, ao + 0.3);
        // Less history where the image moves fast (8 px of motion halves it).
        const float h = history * (1.0 - 0.5 * clamp(length(motion) / 8.0, 0.0, 1.0));
        ao = mix(ao, hist, h);
    }
    imageStore(out_img, hp, vec4(ao, P.z, 0.0, 0.0));
}
