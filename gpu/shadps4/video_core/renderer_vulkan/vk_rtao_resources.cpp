// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: ray-traced AO, buffers, pipelines, acceleration structures and the static cache's bookkeeping (see vk_rtao.cpp).
#include "video_core/renderer_vulkan/vk_rtao.h"
#include "video_core/renderer_vulkan/rtao_bluenoise.h"
#include "video_core/renderer_vulkan/vk_rtao_internal.h"
#include "bbport_threads.h"
#include "common/thread.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <vk_mem_alloc.h>

#include "video_core/host_shaders/rtao_trace_rahit.h"
#include "video_core/host_shaders/rtao_trace_rchit.h"
#include "video_core/host_shaders/rtao_trace_rgen.h"
#include "video_core/host_shaders/rtao_trace_rmiss.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_object_motion.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {
using namespace RtaoInternal;

void RayTracedAO::CreateTracePipeline() {
    const auto device = instance.GetDevice();
    const std::array modules = {CompileSPV(RTAO_TRACE_RGEN, device), CompileSPV(RTAO_TRACE_RMISS, device),
                                CompileSPV(RTAO_TRACE_RCHIT, device), CompileSPV(RTAO_TRACE_RAHIT, device)};
    const std::array stages = {
        vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eRaygenKHR, .module = modules[0], .pName = "main"},
        vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eMissKHR, .module = modules[1], .pName = "main"},
        vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eClosestHitKHR, .module = modules[2], .pName = "main"},
        vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eAnyHitKHR, .module = modules[3], .pName = "main"},
    };
    const std::array groups = {
        vk::RayTracingShaderGroupCreateInfoKHR{.type = vk::RayTracingShaderGroupTypeKHR::eGeneral, .generalShader = 0,
                                               .closestHitShader = VK_SHADER_UNUSED_KHR,
                                               .anyHitShader = VK_SHADER_UNUSED_KHR,
                                               .intersectionShader = VK_SHADER_UNUSED_KHR},
        vk::RayTracingShaderGroupCreateInfoKHR{.type = vk::RayTracingShaderGroupTypeKHR::eGeneral, .generalShader = 1,
                                               .closestHitShader = VK_SHADER_UNUSED_KHR,
                                               .anyHitShader = VK_SHADER_UNUSED_KHR,
                                               .intersectionShader = VK_SHADER_UNUSED_KHR},
        vk::RayTracingShaderGroupCreateInfoKHR{.type = vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup,
                                               .generalShader = VK_SHADER_UNUSED_KHR, .closestHitShader = 2,
                                               .anyHitShader = 3, // the foliage (non-opaque geometry)
                                               .intersectionShader = VK_SHADER_UNUSED_KHR},
    };
    const auto result = device.createRayTracingPipelinesKHR(
        {}, {}, vk::RayTracingPipelineCreateInfoKHR{
                    .stageCount = u32(stages.size()),
                    .pStages = stages.data(),
                    .groupCount = u32(groups.size()),
                    .pGroups = groups.data(),
                    .maxPipelineRayRecursionDepth = 1,
                    .layout = *trace_layout,
                });
    for (const auto module : modules) {
        device.destroyShaderModule(module);
    }
    if (result.result != vk::Result::eSuccess || result.value.empty()) {
        std::printf("RTAO: the ray-tracing-pipeline trace could not be created (%s)\n",
                    vk::to_string(result.result).c_str());
        return;
    }
    rt_trace_pipeline = vk::UniquePipeline(result.value[0], device);
    // Shader binding table: raygen, miss, hit, each at a base-aligned offset.
    const auto props = instance.GetPhysicalDevice()
                           .getProperties2<vk::PhysicalDeviceProperties2,
                                           vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>()
                           .get<vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();
    const u32 handle_size = props.shaderGroupHandleSize;
    const u32 stride = (handle_size + props.shaderGroupHandleAlignment - 1) / props.shaderGroupHandleAlignment *
                       props.shaderGroupHandleAlignment;
    const u32 base = std::max(props.shaderGroupBaseAlignment, stride);
    std::vector<u8> handles(size_t(handle_size) * groups.size());
    if (device.getRayTracingShaderGroupHandlesKHR(*rt_trace_pipeline, 0, u32(groups.size()), handles.size(),
                                                  handles.data()) != vk::Result::eSuccess ||
        !CreateBuffer(sbt_buffer, vk::DeviceSize(base) * groups.size(),
                      vk::BufferUsageFlagBits::eShaderBindingTableKHR, true, props.shaderGroupBaseAlignment)) {
        std::puts("RTAO: the shader binding table could not be created; the pipeline trace is off");
        rt_trace_pipeline.reset();
        return;
    }
    for (size_t i = 0; i < groups.size(); ++i) {
        std::memcpy(static_cast<u8*>(sbt_buffer.mapped) + i * base, handles.data() + i * handle_size, handle_size);
    }
    vmaFlushAllocation(instance.GetAllocator(), sbt_buffer.allocation, 0, VK_WHOLE_SIZE);
    sbt_rgen = vk::StridedDeviceAddressRegionKHR{.deviceAddress = sbt_buffer.address, .stride = stride, .size = stride};
    sbt_miss = vk::StridedDeviceAddressRegionKHR{.deviceAddress = sbt_buffer.address + base, .stride = stride, .size = stride};
    sbt_hit = vk::StridedDeviceAddressRegionKHR{.deviceAddress = sbt_buffer.address + 2 * base, .stride = stride, .size = stride};
    SetObjectName(device, *rt_trace_pipeline, "rtao trace (ray-tracing pipeline)");
    std::puts("RTAO: the trace runs as a ray-tracing pipeline (pipeline=0 in rtao.txt: ray queries)");
}

void RtaoInternal::Worker::Run() {
    Common::SetCurrentThreadName("bb:RtaoSplit");
    BbThreads::MakeBackground();
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock{mutex};
            wake.wait(lock, [this] { return stop || !jobs.empty(); });
            if (jobs.empty()) {
                return; // stop, nothing left
            }
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        job();
    }
}

vk::BuildAccelerationStructureFlagsKHR RayTracedAO::StaticFlags(u32 mode) const {
    using F = vk::BuildAccelerationStructureFlagBitsKHR;
    return mode == 0 ? vk::BuildAccelerationStructureFlagsKHR{F::ePreferFastTrace}
           : mode == 1 || !instance.IsRayTracingPositionFetchSupported()
               ? vk::BuildAccelerationStructureFlagsKHR{F::ePreferFastBuild}
               : F::ePreferFastBuild | F::eAllowDataAccess;
}

bool RayTracedAO::CreateBuffer(Buffer& out, vk::DeviceSize size, vk::BufferUsageFlags usage,
                               bool host, vk::DeviceSize alignment, bool quiet, bool readback) {
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VkBufferUsageFlags(usage | vk::BufferUsageFlagBits::eShaderDeviceAddress),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VmaAllocationCreateInfo alloc_ci{
        .flags = host ? (readback ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                                  : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT) |
                            VMA_ALLOCATION_CREATE_MAPPED_BIT
                      : VmaAllocationCreateFlags{},
        .usage = host ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = host ? VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                              : VkMemoryPropertyFlags{},
        // Read back by the CPU (megabytes at a time): cached memory, not write-combined.
        .preferredFlags = readback ? VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_CACHED_BIT) : VkMemoryPropertyFlags{},
    };
    VkBuffer raw{};
    VmaAllocationInfo info{};
    const VkResult result =
        alignment ? vmaCreateBufferWithAlignment(instance.GetAllocator(), &buffer_ci, &alloc_ci,
                                                 alignment, &raw, &out.allocation, &info)
                  : vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &raw,
                                    &out.allocation, &info);
    if (result != VK_SUCCESS) {
        if (!quiet) {
            std::printf("RTAO: buffer of %llu MiB failed (%d)\n", (unsigned long long)(size >> 20), result);
        }
        return false;
    }
    out.buffer = raw;
    out.size = size;
    out.mapped = info.pMappedData;
    out.address = instance.GetDevice().getBufferAddress({.buffer = out.buffer});
    return true;
}

void RayTracedAO::DestroyBuffer(Buffer& buffer) {
    if (buffer.buffer) {
        vmaDestroyBuffer(instance.GetAllocator(), buffer.buffer, buffer.allocation);
        buffer = {};
    }
}

bool RayTracedAO::CreateAccelerationStructures() {
    const auto device = instance.GetDevice();
    const auto as_props = instance.GetPhysicalDevice()
                              .getProperties2<vk::PhysicalDeviceProperties2,
                                              vk::PhysicalDeviceAccelerationStructurePropertiesKHR>()
                              .get<vk::PhysicalDeviceAccelerationStructurePropertiesKHR>();
    scratch_alignment = std::max<vk::DeviceSize>(as_props.minAccelerationStructureScratchOffsetAlignment, 1);

    using BU = vk::BufferUsageFlagBits;
    // Sizes of a BLAS over one triangle list, or (foliage > 0: the per-frame BLAS) a second
    // non-opaque one of up to `foliage` triangles.
    const auto query = [&](vk::BuildAccelerationStructureFlagsKHR flags, u32 triangles, u32 foliage = 0) {
        const auto geometry = [&](u32 count, vk::GeometryFlagsKHR geometry_flags) {
            return vk::AccelerationStructureGeometryKHR{
                .geometryType = vk::GeometryTypeKHR::eTriangles,
                .geometry = {.triangles = vk::AccelerationStructureGeometryTrianglesDataKHR{
                                 .vertexFormat = vk::Format::eR32G32B32Sfloat,
                                 .vertexStride = 3 * sizeof(float),
                                 .maxVertex = count * 3 - 1,
                                 .indexType = vk::IndexType::eNoneKHR,
                             }},
                .flags = geometry_flags,
            };
        };
        const std::array geometries = {
            geometry(triangles, vk::GeometryFlagBitsKHR::eOpaque),
            geometry(std::max(foliage, 1u), vk::GeometryFlagBitsKHR::eNoDuplicateAnyHitInvocation)};
        const vk::AccelerationStructureBuildGeometryInfoKHR info{
            .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
            .flags = flags,
            .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
            .geometryCount = foliage ? 2u : 1u,
            .pGeometries = geometries.data(),
        };
        const std::array counts = {triangles, foliage};
        return device.getAccelerationStructureBuildSizesKHR(vk::AccelerationStructureBuildTypeKHR::eDevice,
                                                            info, vk::ArrayProxy<const u32>(info.geometryCount,
                                                                                            counts.data()));
    };
    // The per-frame BLAS: fast build (a fast-trace build measured no different and is larger);
    // BB_RTAO_FAST_TRACE=1 sizes for both and allows build=1.
    fast_trace_allowed = EnvU32("BB_RTAO_FAST_TRACE", 0) != 0;
    const vk::BuildAccelerationStructureFlagsKHR access =
        data_access ? vk::BuildAccelerationStructureFlagBitsKHR::eAllowDataAccess : vk::BuildAccelerationStructureFlagsKHR{};
    // The opaque geometry may take all of the budget, the foliage a quarter (FoliageMax()).
    const auto per_frame_sizes = [&](u32 triangles) {
        auto sizes = query(vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild | access, triangles,
                           triangles / 4);
        if (fast_trace_allowed) {
            const auto trace = query(vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace | access, triangles,
                                     triangles / 4);
            sizes.accelerationStructureSize = std::max(sizes.accelerationStructureSize, trace.accelerationStructureSize);
            sizes.buildScratchSize = std::max(sizes.buildScratchSize, trace.buildScratchSize);
        }
        return sizes;
    };
    // The static BLAS: built rarely (staticbuild=, sized for the mode at creation; another mode
    // set live may leave the furthest parts out); it shares the scratch (built before the
    // per-frame BLAS, with a barrier between). A tenth more room: its builds have many geometries
    // (one per part), sized again per build.
    const auto static_sizes_for = [&](u32 triangles) {
        auto sizes = query(StaticFlags(settings.static_build), triangles);
        sizes.accelerationStructureSize += sizes.accelerationStructureSize / 10;
        sizes.buildScratchSize += sizes.buildScratchSize / 10;
        return sizes;
    };
    // Device memory at a scale of the default budgets (all of it scales with them); the per-frame
    // budget by another factor (`growth`, Rtao::FrameGrowth).
    const auto footprint = [&](double scale, double growth = 1.0) {
        const auto frame_sizes = per_frame_sizes(u32(max_triangles * scale * growth));
        const auto static_sizes = static_sizes_for(u32(static_max * scale));
        return frame_sizes.accelerationStructureSize + static_sizes.accelerationStructureSize +
               std::max(frame_sizes.buildScratchSize, static_sizes.buildScratchSize) +
               u64(pool_triangles * scale) * 36 + u64(max_triangles * scale * growth + PromoTriangles) * 36 +
               u64(object_motion.CapturePerFrame()) * 2 * 16;
    };
    // At most Rtao::HeapShare of the device heap's budget (BB_RTAO_HEAP_MB: pretend a budget, to
    // test a smaller card); below a third of the defaults the RTAO stays off.
    {
        const auto& props = instance.GetPhysicalDevice().getMemoryProperties();
        std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
        vmaGetHeapBudgets(instance.GetAllocator(), budgets.data());
        u64 heap = 0;
        for (u32 i = 0; i < props.memoryHeapCount; ++i) {
            if (props.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
                heap = std::max<u64>(heap, budgets[i].budget);
            }
        }
        if (const u32 forced = EnvU32("BB_RTAO_HEAP_MB", 0)) {
            heap = u64(forced) << 20;
        }
        const u64 full = footprint(1.0);
        const double scale = Rtao::HeapScale(heap, full);
        if (scale == 0.0) {
            std::printf("RTAO: off: its buffers need at least %llu MiB, %.0f%% of a device heap budget of "
                        "%llu MiB is %llu MiB\n", (unsigned long long)((u64(full * Rtao::MinScale)) >> 20),
                        Rtao::HeapShare * 100.0, (unsigned long long)(heap >> 20),
                        (unsigned long long)(u64(heap * Rtao::HeapShare) >> 20));
            return false;
        }
        if (scale < 1.0) {
            max_triangles = u32(max_triangles * scale);
            static_max = u32(static_max * scale);
            pool_triangles = u32(pool_triangles * scale);
        } else if (EnvU32("BB_RTAO_MAX_TRIANGLES", 0) == 0) {
            // Room left (16 GB cards): a larger per-frame budget for crowded scenes.
            const double growth = Rtao::FrameGrowth(heap, full, footprint(1.0, 2.0) - full);
            max_triangles = u32(max_triangles * growth);
        }
        std::printf("RTAO: device heap budget %llu MiB: buffers %llu MiB (scale %.2f: per-frame %u, static %u, "
                    "pool %u triangles)\n", (unsigned long long)(heap >> 20),
                    (unsigned long long)(footprint(1.0) >> 20), std::min(scale, 1.0), max_triangles, static_max,
                    pool_triangles);
    }
    if (!CreateBuffer(table_buffer, vk::DeviceSize(TableSlots) * max_geometries * sizeof(GpuGeometry),
                      BU::eStorageBuffer, true) ||
        !CreateBuffer(triangle_buffer, vk::DeviceSize(max_triangles + PromoTriangles) * 9 * sizeof(float),
                      BU::eStorageBuffer | BU::eAccelerationStructureBuildInputReadOnlyKHR |
                          BU::eTransferSrc, // the debug readback copies from it
                      false) ||
        !CreateBuffer(instance_buffer,
                      vk::DeviceSize(TableSlots) * MaxInstances * sizeof(vk::AccelerationStructureInstanceKHR),
                      BU::eAccelerationStructureBuildInputReadOnlyKHR, true) ||
        !CreateBuffer(probe_buffer, vk::DeviceSize(ProbeSlots) * max_geometries * sizeof(ProbeStats),
                      BU::eStorageBuffer, true) ||
        !CreateBuffer(extent_buffer, vk::DeviceSize(max_geometries) * 12, BU::eStorageBuffer, true, 0, false, true) ||
        !CreateBuffer(constants_buffer, vk::DeviceSize(TableSlots) * ConstantsStride, BU::eUniformBuffer, true) ||
        !CreateBuffer(index_ring, vk::DeviceSize(TableSlots) * IndexRingSlot, BU::eStorageBuffer, true) ||
        !CreateBuffer(probe_chunks, vk::DeviceSize(TableSlots) * MaxProbeChunks * 8, BU::eStorageBuffer, true) ||
        !CreateBuffer(probe_ranges, vk::DeviceSize(TableSlots) * max_geometries * 8, BU::eStorageBuffer, true) ||
        !CreateBuffer(probe_partials, vk::DeviceSize(MaxProbeChunks) * 48, BU::eStorageBuffer, false) ||
        !CreateBuffer(readback_buffer, vk::DeviceSize(ReadbackTriangles) * 9 * sizeof(float),
                      BU::eTransferDst, true) ||
        !CreateBuffer(split_fetch, vk::DeviceSize(PromoTriangles) * 36, BU::eTransferDst, true, 0, false, true) ||
        !CreateBuffer(split_staging, vk::DeviceSize(PromoTriangles) * 36, BU::eTransferSrc, true) ||
        !CreateBuffer(static_pool, vk::DeviceSize(pool_triangles) * 36,
                      BU::eAccelerationStructureBuildInputReadOnlyKHR | BU::eTransferDst, false)) {
        return false;
    }
    const auto blas_sizes = per_frame_sizes(max_triangles);
    const auto static_sizes = static_sizes_for(static_max);
    if (!CreateBuffer(blas_buffer, blas_sizes.accelerationStructureSize,
                      BU::eAccelerationStructureStorageKHR, false) ||
        !CreateBuffer(blas_scratch, std::max(blas_sizes.buildScratchSize, static_sizes.buildScratchSize),
                      BU::eStorageBuffer, false, scratch_alignment) ||
        !CreateBuffer(static_blas_buffer, static_sizes.accelerationStructureSize,
                      BU::eAccelerationStructureStorageKHR, false)) {
        return false;
    }
    static_blas = Check(device.createAccelerationStructureKHR({
        .buffer = static_blas_buffer.buffer,
        .size = static_sizes.accelerationStructureSize,
        .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
    }));
    static_blas_address = device.getAccelerationStructureAddressKHR({.accelerationStructure = static_blas});
    pool.Reset(pool_triangles);
    // The rays' blue-noise samples (tools/rtao_bluenoise.py): copied in the first pass.
    if (!CreateBuffer(noise_staging, Rtao::BlueNoise.size(), BU::eTransferSrc, true)) {
        return false;
    }
    std::memcpy(noise_staging.mapped, Rtao::BlueNoise.data(), Rtao::BlueNoise.size());
    noise_image = VideoCore::UniqueImage(device, instance.GetAllocator());
    noise_image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .extent = {Rtao::BlueNoiseSize, Rtao::BlueNoiseSize, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    noise_view = Check(device.createImageViewUnique({
        .image = vk::Image(noise_image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    SetObjectName(device, vk::Image(noise_image), "rtao blue noise");
    noise_uploaded = false;
    blas = Check(device.createAccelerationStructureKHR({
        .buffer = blas_buffer.buffer,
        .size = blas_sizes.accelerationStructureSize,
        .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
    }));
    blas_address = device.getAccelerationStructureAddressKHR({.accelerationStructure = blas});

    // TLAS: one instance, identity (the triangles are already in view space).
    vk::AccelerationStructureGeometryKHR tlas_geometry{
        .geometryType = vk::GeometryTypeKHR::eInstances,
        .geometry = {.instances = vk::AccelerationStructureGeometryInstancesDataKHR{
                         .arrayOfPointers = false,
                         .data = {.deviceAddress = instance_buffer.address},
                     }},
    };
    vk::AccelerationStructureBuildGeometryInfoKHR tlas_info{
        .type = vk::AccelerationStructureTypeKHR::eTopLevel,
        .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild,
        .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
        .geometryCount = 1,
        .pGeometries = &tlas_geometry,
    };
    const auto tlas_sizes = device.getAccelerationStructureBuildSizesKHR(
        vk::AccelerationStructureBuildTypeKHR::eDevice, tlas_info, MaxInstances);
    if (!CreateBuffer(tlas_buffer, tlas_sizes.accelerationStructureSize,
                      BU::eAccelerationStructureStorageKHR, false) ||
        !CreateBuffer(tlas_scratch, tlas_sizes.buildScratchSize, BU::eStorageBuffer, false,
                      scratch_alignment)) {
        return false;
    }
    tlas = Check(device.createAccelerationStructureKHR({
        .buffer = tlas_buffer.buffer,
        .size = tlas_sizes.accelerationStructureSize,
        .type = vk::AccelerationStructureTypeKHR::eTopLevel,
    }));
    for (const auto& [buffer, name] : std::initializer_list<std::pair<const Buffer*, const char*>>{
             {&table_buffer, "rtao table"}, {&triangle_buffer, "rtao triangles"},
             {&instance_buffer, "rtao instances"}, {&probe_buffer, "rtao probe results"},
             {&constants_buffer, "rtao constants"}, {&blas_buffer, "rtao per-frame BLAS"},
             {&blas_scratch, "rtao BLAS scratch"}, {&static_pool, "rtao static pool"},
             {&static_blas_buffer, "rtao static BLAS"},
             {&tlas_buffer, "rtao TLAS"}, {&tlas_scratch, "rtao TLAS scratch"},
             {&readback_buffer, "rtao readback"}, {&index_ring, "rtao index ring"}}) {
        SetObjectName(device, buffer->buffer, name);
    }
    SetObjectName(device, blas, "rtao per-frame BLAS");
    SetObjectName(device, static_blas, "rtao static BLAS");
    SetObjectName(device, tlas, "rtao TLAS");
    for (const Buffer* buffer : {&table_buffer, &triangle_buffer, &instance_buffer, &probe_buffer, &constants_buffer,
                                 &index_ring, &probe_chunks, &probe_ranges, &probe_partials, &split_fetch, &split_staging, &blas_buffer,
                                 &blas_scratch, &static_pool, &static_blas_buffer, &tlas_buffer, &tlas_scratch,
                                 &readback_buffer}) {
        static_bytes += buffer->size;
    }
    std::printf("RTAO: fixed buffers %llu MiB (with the object-motion capture's %llu MiB of positions)\n",
                (unsigned long long)(static_bytes >> 20),
                (unsigned long long)((u64(object_motion.CapturePerFrame()) * 2 * 16) >> 20));
    std::printf("RTAO: acceleration structures: BLAS %llu MiB (+%llu scratch), triangles %llu MiB\n",
                (unsigned long long)(blas_sizes.accelerationStructureSize >> 20),
                (unsigned long long)(blas_sizes.buildScratchSize >> 20),
                (unsigned long long)(triangle_buffer.size >> 20));
    return true;
}

void RayTracedAO::Evict(CacheEntry& entry) {
    // The pool's ranges are free at once: a static BLAS keeps no reference to its input, and the
    // next copy into the pool waits for the builds recorded before it.
    for (const auto& part : entry.parts) {
        pool.Free(part.pool_offset, part.triangles);
    }
    if (entry.in_static) {
        static_dirty = true; // its old triangles must leave the static BLAS now
    }
    entry.parts.clear();
    entry.chunked = false;
    entry.cached = false;
    entry.in_static = false;
    entry.static_skip = false;
    entry.triangles = 0;
}

void RayTracedAO::RebuildStatic(float reach) {
    // Every pooled draw seen lately; its parts within reach, nearest first within the budget.
    struct Candidate {
        float d2;
        u32 offset, triangles;
        u64 key;
    };
    std::vector<Candidate> parts;
    for (auto& [key, e] : cache) {
        e.static_skip = false;
        e.in_static = false;
        if (e.parts.empty() || frame < e.ready_from || frame - e.last_seen > StaticSeenFrames) {
            continue;
        }
        for (const auto& part : e.parts) {
            const float d2 = part.box.Distance2(camera_world);
            if (reach <= 0.0f || d2 <= reach * reach) {
                parts.push_back({d2, part.pool_offset, part.triangles, key});
            }
        }
        e.in_static = true; // unless a part is left out below
    }
    std::sort(parts.begin(), parts.end(), [](const Candidate& a, const Candidate& b) { return a.d2 < b.d2; });
    const auto leave_out = [&](const Candidate& part) {
        CacheEntry& e = cache[part.key];
        e.in_static = false; // stays per frame
        e.static_skip = true;
    };
    std::vector<Candidate> chosen;
    u32 triangles = 0;
    for (const auto& part : parts) {
        if (triangles + part.triangles > static_max) {
            leave_out(part);
            continue;
        }
        triangles += part.triangles;
        chosen.push_back(part);
    }
    // Many geometries need more room than one of the same triangles: the furthest parts go until
    // the build fits the buffers.
    const auto device = instance.GetDevice();
    while (!chosen.empty()) {
        std::vector<vk::AccelerationStructureGeometryKHR> geos(chosen.size());
        std::vector<u32> counts(chosen.size());
        for (size_t i = 0; i < chosen.size(); ++i) {
            geos[i] = vk::AccelerationStructureGeometryKHR{
                .geometryType = vk::GeometryTypeKHR::eTriangles,
                .geometry = {.triangles = vk::AccelerationStructureGeometryTrianglesDataKHR{
                                 .vertexFormat = vk::Format::eR32G32B32Sfloat,
                                 .vertexStride = 3 * sizeof(float),
                                 .maxVertex = chosen[i].triangles * 3 - 1,
                                 .indexType = vk::IndexType::eNoneKHR,
                             }},
                .flags = vk::GeometryFlagBitsKHR::eOpaque,
            };
            counts[i] = chosen[i].triangles;
        }
        const vk::AccelerationStructureBuildGeometryInfoKHR info{
            .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
            .flags = StaticFlags(settings.static_build),
            .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
            .geometryCount = u32(geos.size()),
            .pGeometries = geos.data(),
        };
        const auto sizes = device.getAccelerationStructureBuildSizesKHR(
            vk::AccelerationStructureBuildTypeKHR::eDevice, info, counts);
        if (sizes.accelerationStructureSize <= static_blas_buffer.size && sizes.buildScratchSize <= blas_scratch.size) {
            break;
        }
        const size_t keep = chosen.size() * 9 / 10;
        for (size_t i = keep; i < chosen.size(); ++i) {
            triangles -= chosen[i].triangles;
            leave_out(chosen[i]);
        }
        chosen.resize(keep);
    }
    static_build.clear();
    for (const auto& part : chosen) {
        static_build.push_back({part.offset, part.triangles});
    }
    static_triangles = triangles;
    static_geometries = u32(static_build.size());
    static_valid = !static_build.empty();
    static_dirty = false;
    static_anchor = camera_world;
    static_frame = frame;
    ++static_rebuilds;
    if (!static_valid) {
        for (auto& [key, e] : cache) {
            e.in_static = false;
        }
    }
}

void RayTracedAO::UpdateSplit() {
    if (split_state == SplitState::Fetching && scheduler.IsFree(split_tick)) {
        // The batch is on the host: split it on a worker, which writes the staging buffer (nothing
        // reads it until the batch is built, and no new batch starts before that is done).
        vmaInvalidateAllocation(instance.GetAllocator(), split_fetch.allocation, 0, VK_WHOLE_SIZE);
        split_state = SplitState::Splitting;
        if (!worker) {
            worker = std::make_unique<RtaoInternal::Worker>();
        }
        split_result = worker->Post([meshes = split_meshes,
                                                       fetch = static_cast<const float*>(split_fetch.mapped),
                                                       staging = static_cast<float*>(split_staging.mapped),
                                                       cell = settings.chunk_cell]() mutable {
            const auto start = std::chrono::steady_clock::now();
            for (auto& mesh : meshes) {
                mesh.staging_offset = mesh.offset; // a piece never grows: the same layout
                mesh.groups = Rtao::SplitTriangles(fetch + size_t(mesh.offset) * 9, mesh.triangles, cell,
                                                   ChunkGroupTriangles, MinPieceTriangles,
                                                   staging + size_t(mesh.staging_offset) * 9);
            }
            const float ms =
                std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
            return std::make_pair(std::move(meshes), ms);
        });
    }
    if (split_state == SplitState::Splitting &&
        split_result.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto [meshes, ms] = split_result.get();
        split_meshes = std::move(meshes);
        split_ms += ms;
        split_state = SplitState::Building;
    }
    if (split_state == SplitState::Building &&
        std::all_of(split_meshes.begin(), split_meshes.end(), [](const SplitMesh& m) { return m.done; }) &&
        scheduler.IsFree(split_tick)) {
        split_state = SplitState::Idle; // the staging buffer is free again
        split_meshes.clear();
    }
}

void RayTracedAO::ReadProbes(u32 ring) {
    const u32 id = probe_ids[ring];
    if (!id) {
        return;
    }
    probe_ids[ring] = 0;
    vmaInvalidateAllocation(instance.GetAllocator(), probe_buffer.allocation,
                            vk::DeviceSize(ring) * max_geometries * sizeof(ProbeStats),
                            vk::DeviceSize(max_geometries) * sizeof(ProbeStats));
    const auto* stats = static_cast<const ProbeStats*>(probe_buffer.mapped) + size_t(ring) * max_geometries;
    const auto& keys = probe_keys[ring];
    for (size_t i = 0; i < keys.size(); ++i) {
        if (!keys[i]) {
            continue;
        }
        if (stats[i].id != id) {
            ++probes_stale; // the GPU has not written it (yet)
            continue;
        }
        ++probes_read;
        ProbeStats s;
        std::memcpy(&s, &stats[i], sizeof(s));
        if (s.id != id) {
            continue;
        }
        const auto it = cache.find(keys[i]);
        if (it == cache.end()) {
            continue;
        }
        CacheEntry& e = it->second;
        // Another copy of the same mesh in this slot (the game sorts draws by distance): a reorder.
        bool sibling = false;
        if (e.have_stats && !Rtao::StatsMatch(e.stats, s, camera_world)) {
            const auto copies = max_copies.find(e.base);
            for (u32 o = 0; copies != max_copies.end() && o < copies->second && !sibling; ++o) {
                const auto other = cache.find(Rtao::KeyOf(e.base, o));
                sibling = other != cache.end() && &other->second != &e && other->second.have_stats &&
                          Rtao::StatsMatch(other->second.stats, s, camera_world);
            }
        }
        const auto copies = max_copies.find(e.base);
        const bool multi_copy = copies != max_copies.end() && copies->second > 1;
        switch (Rtao::Observe(e, s, sibling, multi_copy, camera_world)) {
        case Rtao::Observation::First:
            break;
        case Rtao::Observation::Same:
            ++probes_matched;
            break;
        case Rtao::Observation::Reordered:
            ++probes_reordered;
            break;
        case Rtao::Observation::Changed:
            ++probes_changed;
            break;
        case Rtao::Observation::ChangedEvict: // cached, then it moved (a door, a lift)
            ++probes_changed;
            ++evictions_total;
            Evict(e);
            break;
        }
    }
}

void RayTracedAO::TrimCache() {
    if (frame % 600 == 0) { // forget draws not seen for a minute
        for (auto it = cache.begin(); it != cache.end();) {
            if (frame - it->second.last_seen > 3600) {
                Evict(it->second);
                it = cache.erase(it);
            } else {
                ++it;
            }
        }
    }
}

bool RayTracedAO::EnsureResources(u32 w, u32 h) {
    if (width == w && height == h && ao_images[0]) {
        return true;
    }
    const auto device = instance.GetDevice();
    const auto allocator = instance.GetAllocator();
    const auto make_image = [&](VideoCore::UniqueImage& image, vk::UniqueImageView& view,
                                vk::Format format, u32 iw, u32 ih) {
        view.reset();
        image = VideoCore::UniqueImage(device, allocator);
        image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {iw, ih, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                     vk::ImageUsageFlagBits::eTransferSrc, // the dump= copy
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        view = Check(device.createImageViewUnique({
            .image = vk::Image(image),
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
    };
    // A new size (live presets, the upscaler's render size): the old images and the dump buffer
    // go once the GPU is past the commands recorded so far; no full wait (history restarts).
    if (ao_images[0]) {
        struct Old {
            std::vector<VideoCore::UniqueImage> images;
            std::vector<vk::UniqueImageView> views;
            Buffer dump;
            VmaAllocator allocator;
        };
        auto old = std::make_unique<Old>();
        old->allocator = allocator;
        for (const auto& pair : std::initializer_list<std::pair<VideoCore::UniqueImage*, vk::UniqueImageView*>>{
                 {&ao_images[0], &ao_views[0]}, {&ao_images[1], &ao_views[1]}, {&motion_image, &motion_view},
                 {&blur_image, &blur_view}, {&raw_image, &raw_view}, {&hist_images[0], &hist_views[0]},
                 {&hist_images[1], &hist_views[1]}}) {
            old->views.push_back(std::move(*pair.second));
            old->images.push_back(std::move(*pair.first));
        }
        old->dump = std::exchange(dump_buffer, Buffer{});
        scheduler.DeferOperation([old = std::move(old)]() mutable {
            if (old->dump.buffer) {
                vmaDestroyBuffer(old->allocator, old->dump.buffer, old->dump.allocation);
            }
            old.reset();
        });
    }
    for (u32 i = 0; i < 2; ++i) {
        make_image(ao_images[i], ao_views[i], vk::Format::eR16G16Sfloat, (w + 1) / 2, (h + 1) / 2);
    }
    make_image(motion_image, motion_view, vk::Format::eR16G16Sfloat, w, h);
    make_image(blur_image, blur_view, vk::Format::eR16G16Sfloat, (w + 1) / 2, (h + 1) / 2);
    make_image(raw_image, raw_view, vk::Format::eR16G16Sfloat, (w + 1) / 2, (h + 1) / 2);
    for (u32 i = 0; i < 2; ++i) {
        make_image(hist_images[i], hist_views[i], vk::Format::eR16G16B16A16Sfloat, (w + 1) / 2, (h + 1) / 2);
    }
    SetObjectName(device, vk::Image(raw_image), "rtao raw");
    SetObjectName(device, vk::Image(hist_images[0]), "rtao history 0");
    SetObjectName(device, vk::Image(hist_images[1]), "rtao history 1");
    SetObjectName(device, vk::Image(ao_images[0]), "rtao ao 0");
    SetObjectName(device, vk::Image(ao_images[1]), "rtao ao 1");
    SetObjectName(device, vk::Image(motion_image), "rtao motion");
    SetObjectName(device, vk::Image(blur_image), "rtao blur");
    CreateBuffer(dump_buffer, vk::DeviceSize(DumpFrames) * ((w + 1) / 2) * ((h + 1) / 2) * 4,
                 vk::BufferUsageFlagBits::eTransferDst,
                 true);
    dump_pending = false;
    width = w;
    height = h;
    history_valid = false;
    ao_index = 0;
    return true;
}

vk::ImageView RayTracedAO::CachedView(const VideoCore::Image& image, vk::Format format,
                                      vk::ImageAspectFlags aspect) {
    const vk::Image handle = image.GetImage();
    ViewEntry* oldest = &view_cache[0];
    for (auto& entry : view_cache) {
        if (entry.view && entry.image == handle && entry.uid == image.image_uid &&
            entry.format == format && entry.aspect == aspect) {
            entry.last_use = ++view_uses;
            return entry.view;
        }
        if (entry.last_use < oldest->last_use) {
            oldest = &entry;
        }
    }
    const auto device = instance.GetDevice();
    if (oldest->view) {
        scheduler.DeferOperation([device, view = oldest->view] { device.destroyImageView(view); });
    }
    *oldest = {
        .image = handle,
        .uid = image.image_uid,
        .format = format,
        .aspect = aspect,
        .view = Check(device.createImageView({
            .image = handle,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        })),
        .last_use = ++view_uses,
    };
    return oldest->view;
}

} // namespace Vulkan
