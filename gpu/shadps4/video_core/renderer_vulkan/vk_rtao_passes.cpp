// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: ray-traced AO, the pass's GPU work in order (see vk_rtao.cpp): expand and probe, the
// static and per-frame BLAS, the TLAS, the trace, denoiser and blur, and the write into the game's
// mask. Record() prepares everything these commands read and hands it over in PassInputs.
#include "video_core/renderer_vulkan/vk_rtao.h"
#include "video_core/renderer_vulkan/rtao_bluenoise.h"
#include "video_core/renderer_vulkan/vk_rtao_internal.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <iterator>
#include <vk_mem_alloc.h>
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

void RayTracedAO::RecordPasses(const PassInputs& in) {
    // The prepared values under their names in Record() (the commands' lambdas copy them).
    const auto& ao_read = in.ao_read;
    const auto& ao_write = in.ao_write;
    const auto& blur_rw = in.blur_rw;
    const auto& depth_view = in.depth_view;
    const auto& hist_read = in.hist_read;
    const auto& hist_write = in.hist_write;
    const auto& normal_view = in.normal_view;
    const auto& mask_view = in.mask_view;
    const auto& motion = in.motion;
    const auto& raw_rw = in.raw_rw;
    const auto& w = in.w;
    const auto& h = in.h;
    const auto& write = in.write;
    const auto& dispatch_triangles = in.dispatch_triangles;
    const auto& num_chunks = in.num_chunks;
    const auto& num_geometries = in.num_geometries;
    const auto& num_instances = in.num_instances;
    const auto& total_triangles = in.total_triangles;
    const auto& opaque_triangles = in.opaque_triangles;
    const auto& do_dump = in.do_dump;
    const auto& do_readback = in.do_readback;
    const auto& split_fetch_now = in.split_fetch_now;
    const auto& v2 = in.v2;
    const auto& chunks_handle = in.chunks_handle;
    const auto& ranges_handle = in.ranges_handle;
    const auto& partials_handle = in.partials_handle;
    const auto& constants_handle = in.constants_handle;
    const auto& dump_handle = in.dump_handle;
    const auto& probe_handle = in.probe_handle;
    const auto& readback_handle = in.readback_handle;
    const auto& split_fetch_handle = in.split_fetch_handle;
    const auto& split_staging_handle = in.split_staging_handle;
    const auto& pool_handle = in.pool_handle;
    const auto& table_handle = in.table_handle;
    const auto& triangles_handle = in.triangles_handle;
    const auto& chunks_offset = in.chunks_offset;
    const auto& ranges_offset = in.ranges_offset;
    const auto& constants_offset = in.constants_offset;
    const auto& dump_offset = in.dump_offset;
    const auto& probe_bytes = in.probe_bytes;
    const auto& probe_offset = in.probe_offset;
    const auto& split_fetch_bytes = in.split_fetch_bytes;
    const auto& split_fetch_src = in.split_fetch_src;
    const auto& table_bytes = in.table_bytes;
    const auto& table_offset = in.table_offset;
    const auto& triangles_bytes = in.triangles_bytes;
    const auto& blas_scratch_address = in.blas_scratch_address;
    const auto& tlas_scratch_address = in.tlas_scratch_address;
    const auto& instances_address = in.instances_address;
    const auto& pool_address = in.pool_address;
    const auto& triangles_address = in.triangles_address;
    const auto& apply = in.apply;
    const auto& blur_h = in.blur_h;
    const auto& blur_v = in.blur_v;
    const auto& expand = in.expand;
    const auto& probe = in.probe;
    const auto& probe_combine = in.probe_combine;
    const auto& temporal = in.temporal;
    const auto& trace = in.trace;
    const auto& pool_copies = in.pool_copies;
    const auto& static_parts = in.static_parts;
    // Five crumbs, so a GPU hang names the stage; "stage" in the settings stops after that stage.
    const u32 last_stage = settings.stage;
    const bool fast_trace = settings.build_fast_trace;
    const vk::BuildAccelerationStructureFlagsKHR extra_flags =
        data_access ? vk::BuildAccelerationStructureFlagBitsKHR::eAllowDataAccess
                    : vk::BuildAccelerationStructureFlagsKHR{};
    const auto memory_barrier = [](vk::CommandBuffer cmdbuf, vk::PipelineStageFlags2 src_stage,
                                   vk::AccessFlags2 src, vk::PipelineStageFlags2 dst_stage,
                                   vk::AccessFlags2 dst) {
        const vk::MemoryBarrier2 b{.srcStageMask = src_stage,
                                   .srcAccessMask = src,
                                   .dstStageMask = dst_stage,
                                   .dstAccessMask = dst};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &b});
    };
    const auto info = [](vk::ImageView view) {
        return vk::DescriptorImageInfo{.imageView = view, .imageLayout = vk::ImageLayout::eGeneral};
    };
    // BB_GPU_PROFILE=1: one segment per stage.
    const auto profile = [](u64 key, const char* name) {
        if (auto* profiler = GpuProfiler::Get()) {
            profiler->Mark(key, [name] { return std::string{name}; });
        }
    };

    // 1. Expand: the vertex shaders wrote the positions (vertex stage), the table was written by
    // the host, the index buffers by uploads: wait for all of it.
    if (last_stage < 1) {
        return;
    }
    profile(0x47A01ull, "rtao expand");
    scheduler.RecordCrumb(
        {.name = "rtao expand"},
        [=, expand_pipeline = *expand_pipeline, expand_layout = *expand_layout,
         probe_pipeline = *probe_pipeline, probe_layout = *probe_layout](vk::CommandBuffer cmdbuf) {
            // The inputs: captured positions (vertex shaders), index buffers (uploads, or game
            // compute), the table and constants (host). Not ALL_COMMANDS: no render-target writes.
            memory_barrier(cmdbuf,
                           vk::PipelineStageFlagBits2::eVertexShader | Compute |
                               vk::PipelineStageFlagBits2::eTransfer | vk::PipelineStageFlagBits2::eHost,
                           vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite |
                               vk::AccessFlagBits2::eHostWrite,
                           Compute, vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
            if (dispatch_triangles) {
                const vk::DescriptorBufferInfo table_info{table_handle, table_offset, table_bytes};
                const vk::DescriptorBufferInfo tri_info{triangles_handle, 0, triangles_bytes};
                const std::array writes = {
                    vk::WriteDescriptorSet{.dstBinding = 0, .descriptorCount = 1,
                                           .descriptorType = vk::DescriptorType::eStorageBuffer,
                                           .pBufferInfo = &table_info},
                    vk::WriteDescriptorSet{.dstBinding = 1, .descriptorCount = 1,
                                           .descriptorType = vk::DescriptorType::eStorageBuffer,
                                           .pBufferInfo = &tri_info}};
                cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, expand_pipeline);
                cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, expand_layout, 0, writes);
                cmdbuf.pushConstants(expand_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(expand),
                                     &expand);
                cmdbuf.dispatch((dispatch_triangles + 63) / 64, 1, 1);
            }

            memory_barrier(cmdbuf, Compute, vk::AccessFlagBits2::eShaderWrite,
                           AsBuild | vk::PipelineStageFlagBits2::eTransfer,
                           vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead);
            if (do_readback) {
                const vk::BufferCopy region{0, 0, vk::DeviceSize(std::min(ReadbackTriangles, total_triangles)) * 9 * sizeof(float)};
                cmdbuf.copyBuffer(triangles_handle, readback_handle, region);
            }
            if (!pool_copies.empty()) {
                // Into the static pool: after the static builds recorded before (they read it).
                memory_barrier(cmdbuf, AsBuild, vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eTransfer,
                               vk::AccessFlagBits2::eTransferWrite);
                for (const auto& copy : pool_copies) {
                    const vk::BufferCopy region{copy.src, copy.dst, copy.bytes};
                    cmdbuf.copyBuffer(copy.from_staging ? split_staging_handle : triangles_handle, pool_handle, region);
                }
                memory_barrier(cmdbuf, vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                               AsBuild, vk::AccessFlagBits2::eShaderRead);
            }
            if (split_fetch_now) { // the world-space copies of this frame, big meshes among them
                const vk::BufferCopy region{split_fetch_src, 0, split_fetch_bytes};
                cmdbuf.copyBuffer(triangles_handle, split_fetch_handle, region);
                memory_barrier(cmdbuf, vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                               vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead);
            }
        });

    // 1b. The static cache's measurements (its own crumb and profiler segment).
    if (num_geometries && num_chunks) {
        profile(0x47A08ull, "rtao probe");
        scheduler.RecordCrumb(
            {.name = "rtao probe"},
            [=, probe_pipeline = *probe_pipeline, probe_layout = *probe_layout](vk::CommandBuffer cmdbuf) {
                const vk::DescriptorBufferInfo table_info{table_handle, table_offset, table_bytes};
                const vk::DescriptorBufferInfo probe_info{probe_handle, probe_offset, probe_bytes};
                const vk::DescriptorBufferInfo chunks_info{chunks_handle, chunks_offset,
                                                           vk::DeviceSize(MaxProbeChunks) * 8};
                const vk::DescriptorBufferInfo partials_info{partials_handle, 0, vk::DeviceSize(MaxProbeChunks) * 48};
                const vk::DescriptorBufferInfo ranges_info{ranges_handle, ranges_offset,
                                                           vk::DeviceSize(max_geometries) * 8};
                const auto storage = [](u32 binding_index, const vk::DescriptorBufferInfo* buffer_info) {
                    return vk::WriteDescriptorSet{.dstBinding = binding_index, .descriptorCount = 1,
                                                  .descriptorType = vk::DescriptorType::eStorageBuffer,
                                                  .pBufferInfo = buffer_info};
                };
                const std::array writes = {storage(0, &table_info), storage(1, &probe_info),
                                           storage(2, &chunks_info), storage(3, &partials_info),
                                           storage(4, &ranges_info)};
                cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, probe_pipeline);
                cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, probe_layout, 0, writes);
                cmdbuf.pushConstants(probe_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(probe), &probe);
                cmdbuf.dispatch(num_chunks, 1, 1); // pass 1: chunks
                memory_barrier(cmdbuf, Compute, vk::AccessFlagBits2::eShaderWrite, Compute,
                               vk::AccessFlagBits2::eShaderRead);
                cmdbuf.pushConstants(probe_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(probe_combine),
                                     &probe_combine);
                cmdbuf.dispatch(num_geometries, 1, 1); // pass 2: per draw
                memory_barrier(cmdbuf, Compute, vk::AccessFlagBits2::eShaderWrite,
                               vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead);
            });
    }

    // 2. BLAS over the triangle list.
    if (last_stage < 2) {
        return;
    }
    // The static BLAS, when it was rebuilt in this pass (its own profiler segment: a rebuild is a
    // one-frame cost).
    if (!static_parts.empty()) {
        profile(0x47A09ull, "rtao static");
        scheduler.RecordCrumb({.name = "rtao static"}, [=, static_blas = static_blas,
                                                        static_flags = StaticFlags(settings.static_build),
                                                        static_parts = std::move(static_parts)](vk::CommandBuffer cmdbuf) {
        {
            // The last pass's trace still reads the old tree.
            memory_barrier(cmdbuf, TraceStages, vk::AccessFlagBits2::eNone, AsBuild,
                           vk::AccessFlagBits2::eAccelerationStructureWriteKHR);
            std::vector<vk::AccelerationStructureGeometryKHR> geos(static_parts.size());
            std::vector<vk::AccelerationStructureBuildRangeInfoKHR> ranges(static_parts.size());
            for (size_t i = 0; i < static_parts.size(); ++i) {
                const auto [offset, count] = static_parts[i];
                geos[i] = vk::AccelerationStructureGeometryKHR{
                    .geometryType = vk::GeometryTypeKHR::eTriangles,
                    .geometry = {.triangles = vk::AccelerationStructureGeometryTrianglesDataKHR{
                                     .vertexFormat = vk::Format::eR32G32B32Sfloat,
                                     .vertexData = {.deviceAddress = pool_address + vk::DeviceSize(offset) * 36},
                                     .vertexStride = 3 * sizeof(float),
                                     .maxVertex = count * 3 - 1,
                                     .indexType = vk::IndexType::eNoneKHR,
                                 }},
                    .flags = vk::GeometryFlagBitsKHR::eOpaque,
                };
                ranges[i] = vk::AccelerationStructureBuildRangeInfoKHR{.primitiveCount = count};
            }
            const vk::AccelerationStructureBuildGeometryInfoKHR info{
                .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
                .flags = static_flags,
                .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
                .dstAccelerationStructure = static_blas,
                .geometryCount = u32(geos.size()),
                .pGeometries = geos.data(),
                .scratchData = {.deviceAddress = blas_scratch_address},
            };
            const vk::AccelerationStructureBuildRangeInfoKHR* range_ptr = ranges.data();
            cmdbuf.buildAccelerationStructuresKHR(1, &info, &range_ptr);
            // The scratch is the per-frame build's next.
            memory_barrier(cmdbuf, AsBuild, vk::AccessFlagBits2::eAccelerationStructureWriteKHR, AsBuild,
                           vk::AccessFlagBits2::eAccelerationStructureReadKHR |
                               vk::AccessFlagBits2::eAccelerationStructureWriteKHR);
        }
        });
    }
    profile(0x47A02ull, "rtao blas");
    scheduler.RecordCrumb({.name = "rtao blas"}, [=, blas = blas](vk::CommandBuffer cmdbuf) {
        if (!total_triangles) {
            memory_barrier(cmdbuf, AsBuild, vk::AccessFlagBits2::eAccelerationStructureWriteKHR, AsBuild,
                           vk::AccessFlagBits2::eAccelerationStructureReadKHR);
            return;
        }
        // Two geometries over the triangle list: the opaque draws, then the foliage (alpha-tested,
        // foliage=), non-opaque: the trace decides per hit (rtao_trace_constants.glsl).
        const u32 foliage = total_triangles - opaque_triangles;
        const auto geometry = [&](u32 first, u32 count, vk::GeometryFlagsKHR flags) {
            return vk::AccelerationStructureGeometryKHR{
                .geometryType = vk::GeometryTypeKHR::eTriangles,
                .geometry = {.triangles = vk::AccelerationStructureGeometryTrianglesDataKHR{
                                 .vertexFormat = vk::Format::eR32G32B32Sfloat,
                                 .vertexData = {.deviceAddress = triangles_address + vk::DeviceSize(first) * 36},
                                 .vertexStride = 3 * sizeof(float),
                                 .maxVertex = std::max<u32>(count * 3, 1) - 1,
                                 .indexType = vk::IndexType::eNoneKHR,
                             }},
                .flags = flags,
            };
        };
        const std::array blas_geometries = {
            geometry(0, opaque_triangles, vk::GeometryFlagBitsKHR::eOpaque),
            geometry(opaque_triangles, foliage, vk::GeometryFlagBitsKHR::eNoDuplicateAnyHitInvocation)};
        const std::array blas_ranges = {vk::AccelerationStructureBuildRangeInfoKHR{.primitiveCount = opaque_triangles},
                                        vk::AccelerationStructureBuildRangeInfoKHR{.primitiveCount = foliage}};
        const vk::AccelerationStructureBuildGeometryInfoKHR blas_info{
            .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
            .flags = (fast_trace ? vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace
                                 : vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild) |
                     extra_flags,
            .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
            .dstAccelerationStructure = blas,
            .geometryCount = foliage ? 2u : 1u,
            .pGeometries = blas_geometries.data(),
            .scratchData = {.deviceAddress = blas_scratch_address},
        };
        const vk::AccelerationStructureBuildRangeInfoKHR* range_ptr = blas_ranges.data();
        cmdbuf.buildAccelerationStructuresKHR(1, &blas_info, &range_ptr);
        memory_barrier(cmdbuf, AsBuild, vk::AccessFlagBits2::eAccelerationStructureWriteKHR, AsBuild,
                       vk::AccessFlagBits2::eAccelerationStructureReadKHR);
    });

    // 3. TLAS: the one instance.
    if (last_stage < 3) {
        return;
    }
    profile(0x47A03ull, "rtao tlas");
    scheduler.RecordCrumb({.name = "rtao tlas"}, [=, tlas = tlas](vk::CommandBuffer cmdbuf) {
        vk::AccelerationStructureGeometryKHR tlas_geometry{
            .geometryType = vk::GeometryTypeKHR::eInstances,
            .geometry = {.instances = vk::AccelerationStructureGeometryInstancesDataKHR{
                             .arrayOfPointers = false,
                             .data = {.deviceAddress = instances_address},
                         }},
        };
        const vk::AccelerationStructureBuildGeometryInfoKHR tlas_info{
            .type = vk::AccelerationStructureTypeKHR::eTopLevel,
            .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild,
            .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
            .dstAccelerationStructure = tlas,
            .geometryCount = 1,
            .pGeometries = &tlas_geometry,
            .scratchData = {.deviceAddress = tlas_scratch_address},
        };
        const vk::AccelerationStructureBuildRangeInfoKHR tlas_range{.primitiveCount = num_instances};
        cmdbuf.buildAccelerationStructuresKHR(tlas_info, &tlas_range);
        memory_barrier(cmdbuf, AsBuild, vk::AccessFlagBits2::eAccelerationStructureWriteKHR, TraceStages,
                       vk::AccessFlagBits2::eAccelerationStructureReadKHR);
    });

    // 4. Trace at half resolution.
    if (last_stage < 4) {
        return;
    }
    profile(0x47A04ull, "rtao trace");
    scheduler.RecordCrumb(
        {.name = "rtao trace"},
        [=, tlas = tlas, trace_pipeline = *trace_pipeline, trace_layout = *trace_layout,
         rt = settings.use_rt_pipeline && rt_trace_pipeline ? *rt_trace_pipeline : vk::Pipeline{},
         sbt_rgen = sbt_rgen, sbt_miss = sbt_miss, sbt_hit = sbt_hit, noise = *noise_view,
         ao_image = vk::Image(v2 ? raw_image : ao_images[write])](vk::CommandBuffer cmdbuf) {
            const auto d = info(depth_view), n = info(normal_view), m = info(motion), p = info(ao_read),
                       o = info(v2 ? raw_rw : ao_write);
            const vk::WriteDescriptorSetAccelerationStructureKHR as_write{
                .accelerationStructureCount = 1, .pAccelerationStructures = &tlas};
            const vk::DescriptorBufferInfo constants_info{constants_handle, constants_offset, sizeof(trace)};
            const auto bn = info(noise);
            const std::array writes = {
                vk::WriteDescriptorSet{.dstBinding = 0, .descriptorCount = 1,
                                       .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &d},
                vk::WriteDescriptorSet{.dstBinding = 1, .descriptorCount = 1,
                                       .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &n},
                vk::WriteDescriptorSet{.dstBinding = 2, .descriptorCount = 1,
                                       .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &m},
                vk::WriteDescriptorSet{.dstBinding = 3, .descriptorCount = 1,
                                       .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &p},
                vk::WriteDescriptorSet{.dstBinding = 4, .descriptorCount = 1,
                                       .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &o},
                vk::WriteDescriptorSet{.pNext = &as_write, .dstBinding = 5, .descriptorCount = 1,
                                       .descriptorType = vk::DescriptorType::eAccelerationStructureKHR},
                vk::WriteDescriptorSet{.dstBinding = 6, .descriptorCount = 1,
                                       .descriptorType = vk::DescriptorType::eUniformBuffer,
                                       .pBufferInfo = &constants_info},
                vk::WriteDescriptorSet{.dstBinding = 7, .descriptorCount = 1,
                                       .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &bn}};
            if (rt) {
                cmdbuf.bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, rt);
                cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eRayTracingKHR, trace_layout, 0, writes);
                cmdbuf.traceRaysKHR(sbt_rgen, sbt_miss, sbt_hit, vk::StridedDeviceAddressRegionKHR{},
                                    (w + 1) / 2, (h + 1) / 2, 1);
            } else {
                cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, trace_pipeline);
                cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, trace_layout, 0, writes);
                cmdbuf.dispatch(((w + 1) / 2 + 7) / 8, ((h + 1) / 2 + 3) / 4, 1); // 8x4 groups
            }
            const vk::ImageMemoryBarrier2 ao_barrier{
                .srcStageMask = TraceStages,
                .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = Compute | vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = ao_image,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ao_barrier});
        });

    // 4a. Denoiser 2: temporal accumulation into the history and the AO image.
    if (v2) {
        profile(0x47A07ull, "rtao temporal");
        scheduler.RecordCrumb(
            {.name = "rtao temporal"},
            [=, temporal_pipeline = *temporal_pipeline, temporal_layout = *temporal_layout,
             ao_image = vk::Image(ao_images[write]), hist_image = vk::Image(hist_images[write])](
                vk::CommandBuffer cmdbuf) {
                const auto r = info(raw_rw), hr = info(hist_read), m = info(motion), hw = info(hist_write),
                           a = info(ao_write);
                const std::array writes = {
                    vk::WriteDescriptorSet{.dstBinding = 0, .descriptorCount = 1,
                                           .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &r},
                    vk::WriteDescriptorSet{.dstBinding = 1, .descriptorCount = 1,
                                           .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &hr},
                    vk::WriteDescriptorSet{.dstBinding = 2, .descriptorCount = 1,
                                           .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &m},
                    vk::WriteDescriptorSet{.dstBinding = 3, .descriptorCount = 1,
                                           .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &hw},
                    vk::WriteDescriptorSet{.dstBinding = 4, .descriptorCount = 1,
                                           .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &a}};
                cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, temporal_pipeline);
                cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, temporal_layout, 0, writes);
                cmdbuf.pushConstants(temporal_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(temporal),
                                     &temporal);
                cmdbuf.dispatch(((w + 1) / 2 + 7) / 8, ((h + 1) / 2 + 7) / 8, 1);
                std::array<vk::ImageMemoryBarrier2, 2> barriers;
                for (int i = 0; i < 2; ++i) {
                    barriers[i] = vk::ImageMemoryBarrier2{
                        .srcStageMask = Compute,
                        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                        .dstStageMask = Compute | vk::PipelineStageFlagBits2::eTransfer,
                        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead,
                        .oldLayout = vk::ImageLayout::eGeneral,
                        .newLayout = vk::ImageLayout::eGeneral,
                        .image = i == 0 ? ao_image : hist_image,
                        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
                    };
                }
                cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = barriers.data()});
            });
    }

    ao_index = write;
    history_valid = true;

    // 4b. Separable depth-aware blur of the AO image (in place through blur_image).
    if (settings.sigma > 0.0f && settings.debug < 2) { // the debug views stay unblurred
        profile(0x47A06ull, "rtao blur");
        scheduler.RecordCrumb(
            {.name = "rtao blur"},
            [=, blur_pipeline = *blur_pipeline, blur_layout = *blur_layout, ao_image = vk::Image(ao_images[write]),
             blur_img = vk::Image(blur_image)](vk::CommandBuffer cmdbuf) {
                const auto barrier = [&](vk::Image image) {
                    const vk::ImageMemoryBarrier2 b{
                        .srcStageMask = Compute,
                        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                        .dstStageMask = Compute | vk::PipelineStageFlagBits2::eTransfer, // + dump copy
                        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead,
                        .oldLayout = vk::ImageLayout::eGeneral,
                        .newLayout = vk::ImageLayout::eGeneral,
                        .image = image,
                        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
                    };
                    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
                };
                const auto pass = [&](vk::ImageView src, vk::ImageView dst, const BlurConstants& constants) {
                    // Denoiser 2: the history's frame count steers the width (else any image).
                    const auto si = info(src), di = info(dst), ni = info(v2 ? hist_write : ao_read);
                    const std::array writes = {
                        vk::WriteDescriptorSet{.dstBinding = 0, .descriptorCount = 1,
                                               .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &si},
                        vk::WriteDescriptorSet{.dstBinding = 1, .descriptorCount = 1,
                                               .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &di},
                        vk::WriteDescriptorSet{.dstBinding = 2, .descriptorCount = 1,
                                               .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &ni}};
                    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, blur_pipeline);
                    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, blur_layout, 0, writes);
                    cmdbuf.pushConstants(blur_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(constants),
                                         &constants);
                    cmdbuf.dispatch(((w + 1) / 2 + 7) / 8, ((h + 1) / 2 + 7) / 8, 1);
                };
                pass(ao_write, blur_rw, blur_h);
                barrier(blur_img);
                pass(blur_rw, ao_write, blur_v);
                barrier(ao_image);
            });
    }

    // The AO the apply gets, for dump= (DumpFrames consecutive frames).
    if (do_dump) {
        scheduler.Record([=, ao_image = vk::Image(ao_images[write])](vk::CommandBuffer cmdbuf) {
            const vk::BufferImageCopy region{
                .bufferOffset = dump_offset,
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .imageExtent = {(w + 1) / 2, (h + 1) / 2, 1},
            };
            cmdbuf.copyImageToBuffer(ao_image, vk::ImageLayout::eGeneral, dump_handle, region);
        });
        if (++dump_index == DumpFrames) {
            dump_tick = scheduler.CurrentTick();
        }
    }

    // 5. Into the game's mask.
    if (last_stage < 5) {
        return;
    }
    profile(0x47A05ull, "rtao apply");
    scheduler.RecordCrumb(
        {.name = "rtao apply"},
        [=, apply_pipeline = *apply_pipeline, apply_layout = *apply_layout](vk::CommandBuffer cmdbuf) {
            const auto a = info(ao_write), d = info(depth_view), mk = info(mask_view);
            const std::array writes = {
                vk::WriteDescriptorSet{.dstBinding = 0, .descriptorCount = 1,
                                       .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &a},
                vk::WriteDescriptorSet{.dstBinding = 1, .descriptorCount = 1,
                                       .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &d},
                vk::WriteDescriptorSet{.dstBinding = 2, .descriptorCount = 1,
                                       .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &mk}};
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, apply_pipeline);
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, apply_layout, 0, writes);
            cmdbuf.pushConstants(apply_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(apply), &apply);
            cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
        });
}


/// Record(): the layout transitions of the inputs and the RTAO images, the blue-noise upload
/// (once) and the camera motion the denoiser reprojects with.
void RayTracedAO::RecordImageSetup(VideoCore::Image& depth, VideoCore::Image& normal,
                                   VideoCore::Image& mask, vk::ImageView depth_view, u32 read,
                                   u32 write, u32 w, u32 h) {
    scheduler.EndRendering();
    const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    runtime.Transit(&depth, vk::ImageLayout::eGeneral, TraceStages, vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&normal, vk::ImageLayout::eGeneral, TraceStages, vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&mask, vk::ImageLayout::eGeneral, Compute, rw);
    runtime.FlushBarriers();

    const auto image_barrier = [&](vk::Image image, vk::ImageLayout old_layout,
                                   vk::AccessFlags2 src_access, vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = src_access,
            .dstStageMask = TraceStages,
            .dstAccessMask = dst_access,
            .oldLayout = old_layout,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        scheduler.Record([b](vk::CommandBuffer cmdbuf) {
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
        });
    };
    image_barrier(vk::Image(motion_image), vk::ImageLayout::eUndefined, vk::AccessFlagBits2::eNone,
                  vk::AccessFlagBits2::eShaderWrite);
    image_barrier(vk::Image(ao_images[write]), vk::ImageLayout::eUndefined, vk::AccessFlagBits2::eNone,
                  vk::AccessFlagBits2::eShaderWrite);
    image_barrier(vk::Image(blur_image), vk::ImageLayout::eUndefined, vk::AccessFlagBits2::eNone,
                  vk::AccessFlagBits2::eShaderWrite);
    // Denoiser 2: this frame's raw trace and the history pair (same read/write indices).
    image_barrier(vk::Image(raw_image), vk::ImageLayout::eUndefined, vk::AccessFlagBits2::eNone,
                  vk::AccessFlagBits2::eShaderWrite);
    image_barrier(vk::Image(hist_images[write]), vk::ImageLayout::eUndefined, vk::AccessFlagBits2::eNone,
                  vk::AccessFlagBits2::eShaderWrite);
    image_barrier(vk::Image(hist_images[read]),
                  history_valid ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined, rw,
                  vk::AccessFlagBits2::eShaderRead);
    image_barrier(vk::Image(ao_images[read]),
                  history_valid ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined, rw,
                  vk::AccessFlagBits2::eShaderRead);
    if (!noise_uploaded) { // the blue-noise texture, once (rtao_bluenoise.h)
        noise_uploaded = true;
        scheduler.Record([image = vk::Image(noise_image), staging = noise_staging.buffer](vk::CommandBuffer cmdbuf) {
            const auto to = [&](vk::ImageLayout from_layout, vk::ImageLayout layout, vk::PipelineStageFlags2 src,
                                vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst, vk::AccessFlags2 dst_access) {
                const vk::ImageMemoryBarrier2 b{.srcStageMask = src, .srcAccessMask = src_access, .dstStageMask = dst,
                                                .dstAccessMask = dst_access, .oldLayout = from_layout, .newLayout = layout,
                                                .image = image,
                                                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
                cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
            };
            to(vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eNone,
               vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
            const vk::BufferImageCopy region{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                             .imageExtent = {Rtao::BlueNoiseSize, Rtao::BlueNoiseSize, 1}};
            cmdbuf.copyBufferToImage(staging, image, vk::ImageLayout::eTransferDstOptimal, region);
            to(vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eTransfer,
               vk::AccessFlagBits2::eTransferWrite, TraceStages, vk::AccessFlagBits2::eShaderRead);
        });
    }
    camera_motion.RecordMotion(depth_view, *motion_view, w, h);
    image_barrier(vk::Image(motion_image), vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eShaderWrite,
                  vk::AccessFlagBits2::eShaderRead);
}


/// Record(): the shaders' constants and the RTAO images' views for this pass (the trace's
/// constants also go into this ring slot's uniform buffer). Reads in.w, h, v2, write and the counts.
void RayTracedAO::FillConstants(PassInputs& in, u32 slot, u32 read, u32 probe_id) {
    const u32 w = in.w, h = in.h, write = in.write;
    const bool v2 = in.v2;
    const auto inv_view = camera_motion.InvView();
    in.expand = ExpandConstants{
        .positions_address = instance.GetDevice().getBufferAddress({.buffer = object_motion.PositionsBuffer()}),
        .extent_address = in.extent_address,
        .proj = camera_motion.Projection(),
        .num_geometries = in.num_geometries,
        .total_triangles = in.dispatch_triangles,
        .far_cut = settings.ao_far > 0.0f ? settings.ao_far + settings.radius : 0.0f,
        .inv_view = inv_view,
    };
    const auto& expand = in.expand;
    in.probe = ProbeConstants{
        .positions_address = expand.positions_address,
        .num_geometries = in.num_geometries,
        .id = probe_id,
        .proj = expand.proj,
        .inv_view = inv_view,
        .pass_index = 1,
        .num_chunks = in.num_chunks,
    };
    in.probe_combine = in.probe;
    in.probe_combine.pass_index = 2;
    const auto jitter = camera_motion.Jitter();
    in.trace = TraceConstants{
        .view = camera_motion.View(),
        .proj = expand.proj,
        .full_size = {float(w), float(h)},
        .half_size = {float((w + 1) / 2), float((h + 1) / 2)},
        .jitter = jitter,
        .radius = settings.radius,
        .history = history_valid ? settings.history : 0.0f,
        .rays = settings.rays,
        .frame = frame,
        .mode = settings.debug,
        .snap_far = settings.snap_far,
        .ao_far = settings.ao_far,
        .normals = settings.depth_normals ? 1u : 0u,
        .offset = settings.origin_offset,
        .denoise = v2 ? 2u : 1u,
        .subpixel = v2 && settings.subpixel ? 1u : 0u,
        .falloff = settings.falloff,
        .period = settings.noise_period,
        .noise = settings.blue_noise ? 1u : 0u,
        .foliage = in.opaque_triangles < in.total_triangles ? settings.foliage : 0.0f,
    };
    const auto& trace = in.trace;
    in.temporal = TemporalConstants{
        .half_size = {float((w + 1) / 2), float((h + 1) / 2)},
        .sub = settings.subpixel ? std::array<s32, 2>{s32(frame & 1), s32((frame >> 1) & 1)} : std::array<s32, 2>{1, 1},
        .max_n = settings.max_n,
        .clip_k = settings.clip_k,
        .history_ok = history_valid ? 1u : 0u,
        .clip_floor = settings.clip_floor,
    };
    in.raw_rw = *raw_view;
    in.hist_read = *hist_views[read];
    in.hist_write = *hist_views[write];
    in.constants_offset = vk::DeviceSize(slot) * ConstantsStride;
    std::memcpy(static_cast<u8*>(constants_buffer.mapped) + in.constants_offset, &trace, sizeof(trace));
    vmaFlushAllocation(instance.GetAllocator(), constants_buffer.allocation, in.constants_offset, sizeof(trace));
    in.constants_handle = constants_buffer.buffer;
    in.apply = ApplyConstants{
        .proj = expand.proj,
        .full_size = trace.full_size,
        .half_size = trace.half_size,
        .strength = settings.strength,
        .debug = settings.debug ? 1u : 0u,
        .power = settings.power,
        .blur = settings.blur ? 1u : 0u,
        .replace = settings.replace_mask ? 1u : 0u,
    };
    in.ao_read = *ao_views[read];
    in.ao_write = *ao_views[write];
    in.blur_rw = *blur_view;
    in.blur_h = BlurConstants{.half_size = {float((w + 1) / 2), float((h + 1) / 2)}, .dir = {1, 0},
                              .sigma = settings.sigma, .use_n = settings.denoise == 2 && settings.debug < 2 ? 1u : 0u};
    in.blur_v = BlurConstants{.half_size = in.blur_h.half_size, .dir = {0, 1}, .sigma = settings.sigma,
                              .use_n = in.blur_h.use_n};
    in.motion = *motion_view;
}

} // namespace Vulkan
