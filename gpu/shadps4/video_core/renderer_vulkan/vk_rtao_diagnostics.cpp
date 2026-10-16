// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: ray-traced AO, debug output: the dump= frames and the expanded-triangle readback (see vk_rtao.cpp).
#include "video_core/renderer_vulkan/vk_rtao.h"
#include "video_core/renderer_vulkan/vk_rtao_internal.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <vk_mem_alloc.h>

#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {
using namespace RtaoInternal;

void RayTracedAO::WriteDump() {
    if (!(dump_pending && dump_index == DumpFrames && dump_buffer.mapped && scheduler.IsFree(dump_tick))) {
        return;
    }
        dump_pending = false;
        vmaInvalidateAllocation(instance.GetAllocator(), dump_buffer.allocation, 0, VK_WHOLE_SIZE);
        const u32 dw = (width + 1) / 2, dh = (height + 1) / 2;
        const size_t texels_per_frame = size_t(dw) * dh;
        const auto* texels = static_cast<const u16*>(dump_buffer.mapped);
        const auto half_to_float = [](u16 hf) {
            const u32 sign = (hf >> 15) & 1, exp = (hf >> 10) & 0x1f, mant = hf & 0x3ff;
            float value;
            if (exp == 0) value = std::ldexp(float(mant), -24);
            else if (exp == 31) value = mant ? NAN : INFINITY;
            else value = std::ldexp(float(mant | 0x400), int(exp) - 25);
            return sign ? -value : value;
        };
        std::vector<float> prev(texels_per_frame), cur(texels_per_frame);
        double mean = 0.0, change = 0.0;
        u64 changed = 0, zeros = 0;
        for (u32 k = 0; k < DumpFrames; ++k) {
            const u16* frame_texels = texels + k * texels_per_frame * 2;
            for (size_t i = 0; i < texels_per_frame; ++i) {
                const float r = half_to_float(frame_texels[i * 2]);
                cur[i] = std::isfinite(r) ? std::clamp(r, 0.0f, 2.0f) : 0.0f;
                mean += cur[i];
                zeros += cur[i] == 0.0f;
                if (k) {
                    const float d = std::abs(cur[i] - prev[i]);
                    change += d;
                    changed += d > 0.05f;
                }
            }
            const std::string path =
                "out/rtao_dump_" + std::to_string(dump_frame) + "_" + std::to_string(k) + ".pgm";
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fprintf(f, "P5\n%u %u\n255\n", dw, dh);
                std::vector<u8> row(dw);
                for (u32 y = 0; y < dh; ++y) {
                    for (u32 x = 0; x < dw; ++x) {
                        row[x] = u8(cur[size_t(y) * dw + x] * 127.5f + 0.5f); // 1.0 -> 128
                    }
                    std::fwrite(row.data(), 1, dw, f);
                }
                std::fclose(f);
            }
            prev.swap(cur);
        }
        const double all = double(texels_per_frame) * DumpFrames, pairs = double(texels_per_frame) * (DumpFrames - 1);
        std::printf("RTAO: dumped out/rtao_dump_%u_0..%u.pgm (%u consecutive frames, %ux%u, 1.0 = 128): mean %.3f, "
                    "%.1f%% zero; frame to frame: mean |change| %.4f, %.2f%% of pixels change > 0.05\n",
                    dump_frame, DumpFrames - 1, DumpFrames, dw, dh, mean / all, 100.0 * zeros / all,
                    change / pairs, 100.0 * changed / pairs);
}

void RayTracedAO::ReportReadback() {
    if (!(readback_pending && scheduler.IsFree(readback_tick))) {
        return;
    }
        // Copied 120 frames ago: long finished. How do the expanded triangles look?
        readback_pending = false;
        vmaInvalidateAllocation(instance.GetAllocator(), readback_buffer.allocation, 0, VK_WHOLE_SIZE);
        const float* v = static_cast<const float*>(readback_buffer.mapped);
        u32 valid = 0, finite = 0;
        const u32 count = std::min<u32>(ReadbackTriangles, readback_triangles);
        float zmin = 1e30f, zmax = -1e30f, xymax = 0.0f;
        for (u32 i = 0; i < count; ++i) {
            const float* tri = v + i * 9;
            bool nonzero = false, all_finite = true;
            for (u32 k = 0; k < 9; ++k) {
                nonzero |= tri[k] != 0.0f;
                all_finite &= std::isfinite(tri[k]);
            }
            if (!nonzero) continue;
            ++valid;
            if (!all_finite) continue;
            ++finite;
            for (u32 k = 0; k < 3; ++k) {
                zmin = std::min(zmin, tri[k * 3 + 2]);
                zmax = std::max(zmax, tri[k * 3 + 2]);
                xymax = std::max({xymax, std::abs(tri[k * 3]), std::abs(tri[k * 3 + 1])});
            }
        }
        std::printf("RTAO: readback (frame %u): of the first %u triangles %u non-zero, %u valid (finite), "
                    "%u invalid (NaN); z %.2f..%.2f, |xy| up to %.2f; first: (%.2f %.2f %.2f) (%.2f %.2f %.2f) "
                    "(%.2f %.2f %.2f)\n",
                    readback_frame, count, valid, finite, valid - finite, zmin, zmax, xymax, v[0], v[1], v[2],
                    v[3], v[4], v[5], v[6], v[7], v[8]);
}

void RayTracedAO::SampleExtents(PassInputs& in, const FrameDraws& classified) {
    in.extent_address = 0;
    if (extent_pending || frame % 120 != 0 || geometries.empty()) {
        return;
    }
    auto* words = static_cast<u32*>(extent_buffer.mapped);
    for (size_t i = 0; i < geometries.size(); ++i) {
        words[i * 3] = 0;
        words[i * 3 + 1] = 0;
        words[i * 3 + 2] = ~0u;
    }
    vmaFlushAllocation(instance.GetAllocator(), extent_buffer.allocation, 0, geometries.size() * 12);
    std::unordered_map<u64, const Draw*> by_key;
    for (const Draw& d : draws) {
        by_key.emplace(d.key, &d);
    }
    extent_entries.clear();
    for (size_t i = 0; i < geometries.size(); ++i) {
        const bool dynamic = i < classified.dynamic_keys.size();
        const auto it = dynamic ? by_key.find(classified.dynamic_keys[i]) : by_key.end();
        const Draw* d = it != by_key.end() ? it->second : nullptr;
        extent_entries.push_back({.key = d ? d->key : 0, .triangles = geometries[i].triangles,
                                  .skinned = d && d->skinned, .foliage = d && d->foliage,
                                  .world = (geometries[i].flags & GeoWorld) != 0});
    }
    in.extent_address = extent_buffer.address;
    extent_pending = true;
    extent_tick = scheduler.CurrentTick();
    extent_frame = frame;
}

void RayTracedAO::ReportExtents() {
    if (!extent_pending || !scheduler.IsFree(extent_tick)) {
        return;
    }
    extent_pending = false;
    vmaInvalidateAllocation(instance.GetAllocator(), extent_buffer.allocation, 0, extent_entries.size() * 12);
    const auto* words = static_cast<const u32*>(extent_buffer.mapped);
    std::vector<u32> order;
    u32 oversized = 0;
    for (u32 i = 0; i < extent_entries.size(); ++i) {
        // World copies are the static cache's (far buildings, in the static BLAS only within reach);
        // per frame a few long skinned parts (chains, cloth) are normal.
        if (words[i * 3 + 1] && !extent_entries[i].world) {
            order.push_back(i);
            oversized += words[i * 3 + 1];
        }
    }
    if (oversized < OversizedReport) {
        return;
    }
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) { return words[a * 3 + 1] > words[b * 3 + 1]; });
    std::printf("RTAO: frame %u: %u triangles over 4 m (centre to vertex) in %zu draws; largest:", extent_frame,
                oversized, order.size());
    for (size_t k = 0; k < std::min<size_t>(order.size(), 4); ++k) {
        const u32 i = order[k];
        const ExtentEntry& e = extent_entries[i];
        std::printf(" [key %016llx%s%s%s, %u triangles, %u over 4 m, max %.1f m, nearest %.1f m]",
                    (unsigned long long)e.key, e.skinned ? " skinned" : "", e.foliage ? " foliage" : "",
                    e.world ? " world copy" : "", e.triangles, words[i * 3 + 1], words[i * 3] / 1000.0,
                    words[i * 3 + 2] / 100.0);
    }
    std::printf("\n");
}

/// The first passes after start: the camera, the table size and the first geometries.
void RayTracedAO::LogFirstPasses(u32 num_geometries, u32 total_triangles, u32 w, u32 h) {
    const auto cam_proj = camera_motion.Projection();
    std::printf("RTAO: pass %u: proj %g %g %g %g, %u geometries, %u triangles, %ux%u\n",
                recorded_total, cam_proj[0], cam_proj[1], cam_proj[2], cam_proj[3], num_geometries,
                total_triangles, w, h);
    for (u32 i = 0; i < std::min<u32>(num_geometries, 4); ++i) {
        const auto& g = geometries[i];
        std::printf("RTAO:   geometry %u: store %u, %u vertices x %u instances, first %u, indices 0x%llx "
                    "count %u %s base %d restart %u, triangles %u at %u\n",
                    i, g.store, g.vertices, g.instances, g.first_vertex,
                    (unsigned long long)g.index_address, g.index_count,
                    (g.index16 & 1) ? "16-bit" : "32-bit", g.base_vertex, g.restart, g.triangles,
                    g.triangle_offset);
    }
}

} // namespace Vulkan
