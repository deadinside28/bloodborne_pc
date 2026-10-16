// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: ray-traced AO, construction (pipelines, settings), the settings re-read and
// destruction (see vk_rtao.cpp and docs/rtao.md).
#include "video_core/renderer_vulkan/vk_rtao.h"
#include "video_core/renderer_vulkan/vk_rtao_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include "common/path_util.h"

#include <vk_mem_alloc.h>

#include "video_core/host_shaders/rtao_apply_comp.h"
#include "video_core/host_shaders/rtao_blur_comp.h"
#include "video_core/host_shaders/rtao_expand_comp.h"
#include "video_core/host_shaders/rtao_probe_comp.h"
#include "video_core/host_shaders/rtao_temporal_comp.h"
#include "video_core/host_shaders/rtao_trace_comp.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "core/memory.h"
#include "video_core/renderer_vulkan/vk_object_motion.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

using namespace RtaoInternal;

RayTracedAO::RayTracedAO(const Instance& instance_, Scheduler& scheduler_,
                         VideoCore::TextureCache& texture_cache_, VideoCore::BufferCache& buffer_cache_,
                         Runtime& runtime_, CameraMotion& camera_motion_, ObjectMotion& object_motion_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_}, buffer_cache{buffer_cache_},
      runtime{runtime_}, camera_motion{camera_motion_}, object_motion{object_motion_} {
    if (!Rtao::Requested()) {
        return;
    }
    if (!object_motion.Enabled() || !camera_motion.Enabled()) {
        std::puts("RTAO: needs the object motion vectors (menu: object motion, with an upscaler), off");
        return;
    }
    if (!instance.IsRayQuerySupported() || !ObjectMotion::RtaoCapture()) {
        std::puts("RTAO: ray queries unsupported by the driver, off");
        return;
    }
    settings.debug = std::min(EnvU32("BB_RTAO_DEBUG", 0), 4u);
    settings.strength = std::clamp(EnvFloat("BB_RTAO_STRENGTH", 1.0f), 0.0f, 1.0f);
    settings.radius = std::max(EnvFloat("BB_RTAO_RADIUS", 1.5f), 0.05f);
    settings.power = std::clamp(EnvFloat("BB_RTAO_POWER", 2.0f), 0.1f, 8.0f);
    settings.history = std::clamp(EnvFloat("BB_RTAO_HISTORY", 0.5f), 0.0f, 0.98f);
    settings.sigma = std::clamp(EnvFloat("BB_RTAO_SIGMA", 1.5f), 0.0f, 4.0f);
    settings.rays = std::clamp(EnvU32("BB_RTAO_RAYS", 2), 1u, 32u);
    max_triangles = std::clamp(EnvU32("BB_RTAO_MAX_TRIANGLES", 1500000), 10000u, 16000000u);
    settings.stage = std::clamp(EnvU32("BB_RTAO_STAGE", 5), 0u, 5u);
    settings.cache_enabled = EnvU32("BB_RTAO_CACHE", 1) != 0;
    settings.denoise = std::clamp(EnvU32("BB_RTAO_DENOISE", 2), 1u, 2u);
    // ALLOW_DATA_ACCESS on the per-frame BLAS although nothing reads positions back: on RADV the
    // flag turns RDNA4's compressed triangle-pair leaves off, and their build cost more than
    // their traversal saved (Doll, three sessions: build 1.65 vs 2.41 ms, trace 1.79 vs 1.73 ms).
    // The static BLAS: staticbuild= (fast build with the flag by default: a rebuild costs 1.4 ms
    // instead of 2.75 for fast trace, and traces 0.03 ms slower).
    // The flag is VK_KHR_ray_tracing_position_fetch's: without the extension, builds go without it.
    data_access = EnvU32("BB_RTAO_DATA_ACCESS", 1) != 0 && instance.IsRayTracingPositionFetchSupported();
    hold_enabled = EnvU32("BB_RTAO_HOLD", 1) != 0;
    test_move = EnvU32("BB_RTAO_TEST_MOVE", 0) != 0;

    const auto device = instance.GetDevice();
    const auto make_layouts = [&](std::span<const vk::DescriptorSetLayoutBinding> bindings,
                                  u32 push_size, vk::UniqueDescriptorSetLayout& desc_layout,
                                  vk::UniquePipelineLayout& layout) {
        desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                          .offset = 0,
                                          .size = push_size};
        layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*desc_layout,
            .pushConstantRangeCount = push_size ? 1u : 0u,
            .pPushConstantRanges = push_size ? &range : nullptr,
        }));
    };
    const auto make_pipeline = [&](std::span<const u32> code, vk::PipelineLayout layout) {
        const auto module = CompileSPV(code, device);
        auto pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = layout,
                }));
        device.destroyShaderModule(module);
        return pipeline;
    };
    const auto binding = [](u32 index, vk::DescriptorType type) {
        return vk::DescriptorSetLayoutBinding{.binding = index,
                                              .descriptorType = type,
                                              .descriptorCount = 1,
                                              .stageFlags = vk::ShaderStageFlagBits::eCompute};
    };
    using DT = vk::DescriptorType;
    const std::array expand_bindings = {binding(0, DT::eStorageBuffer), binding(1, DT::eStorageBuffer)};
    const bool rt_pipeline_supported = instance.IsRayTracingPipelineSupported();
    const auto trace_binding = [&](u32 index, vk::DescriptorType type) {
        auto b = binding(index, type);
        if (rt_pipeline_supported) {
            b.stageFlags |= vk::ShaderStageFlagBits::eRaygenKHR | vk::ShaderStageFlagBits::eAnyHitKHR;
        }
        return b;
    };
    const std::array trace_bindings = {trace_binding(0, DT::eSampledImage), trace_binding(1, DT::eSampledImage),
                                       trace_binding(2, DT::eSampledImage), trace_binding(3, DT::eSampledImage),
                                       trace_binding(4, DT::eStorageImage),
                                       trace_binding(5, DT::eAccelerationStructureKHR),
                                       trace_binding(6, DT::eUniformBuffer), trace_binding(7, DT::eSampledImage)};
    const std::array apply_bindings = {binding(0, DT::eSampledImage), binding(1, DT::eSampledImage),
                                       binding(2, DT::eStorageImage)};
    make_layouts(expand_bindings, sizeof(ExpandConstants), expand_desc_layout, expand_layout);
    make_layouts(trace_bindings, 0, trace_desc_layout, trace_layout); // constants: a uniform buffer
    const std::array blur_bindings = {binding(0, DT::eSampledImage), binding(1, DT::eStorageImage),
                                      binding(2, DT::eSampledImage)};
    const std::array temporal_bindings = {binding(0, DT::eSampledImage), binding(1, DT::eSampledImage),
                                          binding(2, DT::eSampledImage), binding(3, DT::eStorageImage),
                                          binding(4, DT::eStorageImage)};
    make_layouts(temporal_bindings, sizeof(TemporalConstants), temporal_desc_layout, temporal_layout);
    temporal_pipeline = make_pipeline(RTAO_TEMPORAL_COMP, *temporal_layout);
    make_layouts(apply_bindings, sizeof(ApplyConstants), apply_desc_layout, apply_layout);
    make_layouts(blur_bindings, sizeof(BlurConstants), blur_desc_layout, blur_layout);
    blur_pipeline = make_pipeline(RTAO_BLUR_COMP, *blur_layout);
    const std::array probe_bindings = {binding(0, DT::eStorageBuffer), binding(1, DT::eStorageBuffer),
                                       binding(2, DT::eStorageBuffer), binding(3, DT::eStorageBuffer),
                                       binding(4, DT::eStorageBuffer)};
    make_layouts(probe_bindings, sizeof(ProbeConstants), probe_desc_layout, probe_layout);
    probe_pipeline = make_pipeline(RTAO_PROBE_COMP, *probe_layout);
    expand_pipeline = make_pipeline(RTAO_EXPAND_COMP, *expand_layout);
    trace_pipeline = make_pipeline(RTAO_TRACE_COMP, *trace_layout);
    apply_pipeline = make_pipeline(RTAO_APPLY_COMP, *apply_layout);
    if (rt_pipeline_supported) {
        CreateTracePipeline();
        // Measured at the Doll: trace 1.40-1.44 ms as a pipeline vs 1.62-1.64 ms with ray queries.
        settings.use_rt_pipeline = rt_trace_pipeline && EnvU32("BB_RTAO_PIPELINE", 1) != 0;
    }
    // Names for validation messages and captures.
    SetObjectName(device, *expand_pipeline, "rtao expand");
    SetObjectName(device, *probe_pipeline, "rtao probe");
    SetObjectName(device, *trace_pipeline, "rtao trace");
    SetObjectName(device, *blur_pipeline, "rtao blur");
    SetObjectName(device, *apply_pipeline, "rtao apply");
    SetObjectName(device, *temporal_pipeline, "rtao temporal");

    // The buffers (over a GiB) are made at the first frame with a scene camera, not here: made
    // here, before the game's own resources, past ~1.3 GiB the game's lights went out (lanterns,
    // candles; reproducibly, VRAM far from full). See docs/rtao.md.
    enabled = true;
    std::printf("RTAO: on, %u rays, denoiser %u, radius %.2f, strength %.2f, up to %u triangles%s\n",
                settings.rays, settings.denoise, settings.radius, settings.strength, max_triangles, settings.debug ? ", debug view" : "");
    if (const char* file = std::getenv("BB_RTAO_FILE")) {
        settings_path = file;
    } else {
        settings_path = (Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "rtao.txt").string();
    }
    std::printf("RTAO: settings re-read from %s while the game runs\n", settings_path.c_str());
}

void RayTracedAO::PollSettings() {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_poll < std::chrono::seconds(1)) {
        return;
    }
    last_poll = now;
    struct stat st{};
    if (::stat(settings_path.c_str(), &st) != 0) {
        return;
    }
    const long long mtime = static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
    if (mtime == settings_mtime) {
        return;
    }
    settings_mtime = mtime;
    std::ifstream file(settings_path);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    bool history_reset = false;
    Rtao::ApplyText(settings, text, history_reset);
    if (history_reset) {
        history_valid = false; // the two denoisers' histories differ
    }
    settings.use_rt_pipeline = settings.use_rt_pipeline && rt_trace_pipeline;
    settings.build_fast_trace = settings.build_fast_trace && fast_trace_allowed;
    std::printf("RTAO: settings: enabled %d, %u rays, denoiser %u (maxn %.0f, clip %.1f / %.2f), radius %.2f, "
                "strength %.2f, power %.2f, far %.0f m, snap %.0f m, normals %s, trace %s, cache %d (maxsize %.0f m), "
                "debug %u, stage %u\n",
                settings.active ? 1 : 0, settings.rays, settings.denoise, settings.max_n, settings.clip_k,
                settings.clip_floor, settings.radius, settings.strength, settings.power, settings.ao_far,
                settings.snap_far, settings.depth_normals ? "depth" : "shading",
                settings.use_rt_pipeline ? "pipeline" : "ray query", settings.cache_enabled ? 1 : 0,
                settings.cache_max_size, settings.debug, settings.stage);
}

RayTracedAO::~RayTracedAO() {
    if (split_result.valid()) {
        split_result.wait(); // it writes into the staging buffer
    }
    const auto device = instance.GetDevice();
    for (auto& entry : view_cache) {
        if (entry.view) {
            device.destroyImageView(entry.view);
        }
    }
    if (tlas) {
        device.destroyAccelerationStructureKHR(tlas);
    }
    if (blas) {
        device.destroyAccelerationStructureKHR(blas);
    }
    if (static_blas) {
        device.destroyAccelerationStructureKHR(static_blas);
    }
    for (auto* buffer : {&table_buffer, &triangle_buffer, &blas_buffer, &blas_scratch, &tlas_buffer,
                         &tlas_scratch, &instance_buffer, &readback_buffer, &dump_buffer, &probe_buffer, &extent_buffer,
                         &static_pool, &static_blas_buffer, &noise_staging, &constants_buffer, &sbt_buffer, &index_ring, &probe_chunks,
                         &probe_ranges, &probe_partials, &split_fetch, &split_staging}) {
        DestroyBuffer(*buffer);
    }
}

} // namespace Vulkan
