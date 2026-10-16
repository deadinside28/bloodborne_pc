// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: ray-traced AO, what the pass's translation units share (vk_rtao.cpp, vk_rtao_resources.cpp,
// vk_rtao_diagnostics.cpp): the GPU constant blocks and small helpers.

#pragma once

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/rtao_geometry.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::RtaoInternal {
struct ExpandConstants {
    u64 positions_address;
    u64 extent_address; // the size diagnostic's buffer this pass (0: off; RayTracedAO::SampleExtents)
    std::array<float, 4> proj;
    u32 num_geometries;
    u32 total_triangles;
    float far_cut;
    u32 pad1;
    std::array<float, 12> inv_view; // view -> world rows (world-space entries)
};
struct ProbeConstants {
    u64 positions_address;
    u32 num_geometries;
    u32 id;
    std::array<float, 4> proj;
    std::array<float, 12> inv_view;
    u32 pass_index;
    u32 num_chunks;
};
static_assert(sizeof(ProbeConstants) == 88 && offsetof(ProbeConstants, pass_index) == 80);
struct TraceConstants {
    std::array<float, 12> view;
    std::array<float, 4> proj;
    std::array<float, 2> full_size;
    std::array<float, 2> half_size;
    std::array<float, 2> jitter;
    float radius;
    float history;
    u32 rays;
    u32 frame;
    u32 mode; // 0 ao, 2 primary-ray test
    float snap_far;
    float ao_far;
    u32 normals;
    float offset;
    u32 denoise;
    u32 subpixel;
    u32 falloff;
    u32 period;
    u32 noise; // 1: blue-noise texture, 0: interleaved gradient noise
    float foliage;
};
struct TemporalConstants {
    std::array<float, 2> half_size;
    std::array<s32, 2> sub;
    float max_n;
    float clip_k;
    u32 history_ok;
    float clip_floor;
};
static_assert(sizeof(TemporalConstants) == 32);
struct ApplyConstants {
    std::array<float, 4> proj;
    std::array<float, 2> full_size;
    std::array<float, 2> half_size;
    float strength;
    u32 debug;
    float power;
    u32 blur;
    u32 replace;
    u32 pad1;
};
static_assert(sizeof(ExpandConstants) == 96 && offsetof(ExpandConstants, inv_view) == 48);
static_assert(sizeof(TraceConstants) == 148 && offsetof(TraceConstants, foliage) == 144);
struct BlurConstants {
    std::array<float, 2> half_size;
    std::array<s32, 2> dir;
    float sigma;
    u32 use_n;
};
static_assert(sizeof(ApplyConstants) == 56);
static_assert(sizeof(BlurConstants) == 24);
/// The pass's one helper thread (bb:RtaoSplit, on otherwise idle cores): jobs run in order,
/// each hands its result back through a future. Started with the first job, joined on
/// destruction after the jobs left (the owner waits for its futures before freeing what they use).
class Worker {
public:
    ~Worker() {
        {
            std::scoped_lock lock{mutex};
            stop = true;
        }
        wake.notify_one();
        if (thread.joinable()) {
            thread.join();
        }
    }
    template <class F>
    auto Post(F&& job) -> std::future<decltype(job())> {
        using Result = decltype(job());
        auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<F>(job));
        auto future = task->get_future();
        {
            std::scoped_lock lock{mutex};
            jobs.emplace_back([task] { (*task)(); });
            if (!thread.joinable()) {
                thread = std::thread([this] { Run(); });
            }
        }
        wake.notify_one();
        return future;
    }

private:
    void Run();
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    bool stop = false;
    std::thread thread;
};
/// A copy into the static pool: world-space triangles from the triangle list (promotions) or the
/// split staging buffer (pieces of big meshes).
struct PoolCopy {
    bool from_staging;
    vk::DeviceSize src, dst, bytes;
};
/// A frame's draws after the static cache's classification (RayTracedAO::ClassifyDraws).
struct FrameDraws {
    std::vector<Rtao::GpuGeometry> dynamic_list, verify_list, promo_list; // per frame, probed only, new pool entries
    std::vector<u64> dynamic_keys, verify_keys;
    std::vector<PoolCopy> pool_copies;
    u32 promo_used = 0;           // triangles expanded for new pool entries
    u32 opaque_triangles = 0;     // the per-frame triangles before the foliage
    bool split_fetch_now = false; // a batch of big meshes goes to the host this pass
};
/// Everything Record() prepared that the pass's commands read (RayTracedAO::RecordPasses): views,
/// buffers and offsets of this frame's ring slots, counts, and the stages' constants.
struct PassInputs {
    vk::ImageView ao_read, ao_write, blur_rw, depth_view, hist_read, hist_write, normal_view, mask_view, motion, raw_rw;
    u32 w, h, write, dispatch_triangles, num_chunks, num_geometries, num_instances, total_triangles;
    u32 opaque_triangles; // the rest of total_triangles: the foliage, non-opaque
    bool do_dump, do_readback, split_fetch_now, v2;
    vk::Buffer chunks_handle, ranges_handle, partials_handle, constants_handle, dump_handle, probe_handle, readback_handle, split_fetch_handle, split_staging_handle, pool_handle, table_handle, triangles_handle;
    vk::DeviceSize chunks_offset, ranges_offset, constants_offset, dump_offset, probe_bytes, probe_offset, split_fetch_bytes, split_fetch_src, table_bytes, table_offset, triangles_bytes;
    vk::DeviceAddress blas_scratch_address, tlas_scratch_address, instances_address, pool_address, triangles_address;
    vk::DeviceAddress extent_address = 0;
    ApplyConstants apply;
    BlurConstants blur_h, blur_v;
    ExpandConstants expand;
    ProbeConstants probe, probe_combine;
    TemporalConstants temporal;
    TraceConstants trace;
    std::vector<PoolCopy> pool_copies;
    std::vector<std::array<u32, 2>> static_parts;
};
inline float EnvFloat(const char* name, float fallback) {
    const char* value = std::getenv(name);
    return value ? std::strtof(value, nullptr) : fallback;
}
inline u32 EnvU32(const char* name, u32 fallback) {
    const char* value = std::getenv(name);
    return value ? u32(std::strtoul(value, nullptr, 10)) : fallback;
}
constexpr auto AsBuild = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR;
constexpr auto Compute = vk::PipelineStageFlagBits2::eComputeShader;
// The trace runs in compute (ray queries) or as a ray-generation shader (pipeline=1).
constexpr auto TraceStages = Compute | vk::PipelineStageFlagBits2::eRayTracingShaderKHR;
} // namespace Vulkan::RtaoInternal
