// SPDX-License-Identifier: GPL-2.0-or-later
// The game's buffer copy compute shader done as buffer copies: its control buffer lists
// (destination, source, count) records of one stride; copies close together are grouped so
// that each group needs one source and one destination buffer, and each group is one dispatch
// of buffer_multi_copy.comp (or vkCmdCopyBuffer for a few copies).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "bbgpu/shaders/buffer_multi_copy_comp.h"
#include "bbport_toggles.h"
#include "game_profile.h"
#include "render/runtime.h"
#include "render/scheduler.h"
#include "render/shader_hle.h"
#include "render/vk_gpu_profiler.h"
#include "shader/info.h"
#include "render/rasterizer.h"
#include "vulkan/instance.h"
#include "vulkan/shader_module.h"

extern std::unique_ptr<BbCp::CommandProcessor> liverpool;

namespace Vulkan {

// The copy shader runs ~57 times per frame with ~1024 small ranges each. As one
// vkCmdCopyBuffer with that many regions it cost ~37 ns of GPU time per range (~2.1 ms per
// frame) plus the recording; buffer_multi_copy.comp copies all ranges in one dispatch.
//
// bbport 2026-10-22: `ranges` are the buffer ranges of the groups the copies came from (64 KiB
// windows, ExecuteCopyShaderHLE). Barriers are decided and accesses tracked per range, not over
// the bounding span: one dispatch for all groups of a buffer pair must not make every binding
// that lies between the groups (tens of MiB) wait for it.
struct CopyRange {
    u64 src_offset, src_size, dst_offset, dst_size;
};
static bool MultiCopy(Rasterizer& rasterizer, const VideoCore::Buffer* src,
                      const VideoCore::Buffer* dst, std::span<const vk::BufferCopy> copies,
                      std::span<const CopyRange> ranges) {
    if (copies.size() < 8 || BbToggle::Disabled(BbToggle::MultiCopyShader)) {
        return false;
    }
    u64 src_min = ~0ull, src_max = 0, dst_min = ~0ull, dst_max = 0;
    for (const auto& copy : copies) {
        if ((copy.srcOffset | copy.dstOffset | copy.size) & 3) {
            return false; // dword copies only
        }
        src_min = std::min(src_min, copy.srcOffset);
        src_max = std::max(src_max, copy.srcOffset + copy.size);
        dst_min = std::min(dst_min, copy.dstOffset);
        dst_max = std::max(dst_max, copy.dstOffset + copy.size);
    }
    auto& runtime = rasterizer.GetRuntime();
    auto& scheduler = runtime.GetScheduler();
    const auto& instance = runtime.GetInstance();
    const u64 align = instance.StorageMinAlignment();
    src_min = Common::AlignDown(src_min, align);
    dst_min = Common::AlignDown(dst_min, align);
    // The bound spans must fit a storage buffer descriptor and the shader's dword offsets.
    const u64 max_span = std::min<u64>(instance.MaxStorageBufferRange(), u64{1} << 32);
    if (src_max - src_min > max_span || dst_max - dst_min > max_span) {
        return false;
    }

    struct Pipeline {
        vk::UniqueDescriptorSetLayout set_layout;
        vk::UniquePipelineLayout layout;
        vk::UniquePipeline pipeline;
    };
    static Pipeline pipe = [&] {
        const auto device = instance.GetDevice();
        Pipeline p;
        std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
        for (u32 i = 0; i < 3; ++i) {
            bindings[i] = {.binding = i,
                           .descriptorType = vk::DescriptorType::eStorageBuffer,
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        p.set_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                          .offset = 0,
                                          .size = sizeof(u32)};
        p.layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*p.set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        }));
        const auto module = CompileSPV(BUFFER_MULTI_COPY_COMP, device);
        p.pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *p.layout,
                }));
        device.destroyShaderModule(module);
        return p;
    }();

    // The region table goes into the stream buffer (host writes, visible at submission).
    thread_local std::vector<u32> regions;
    regions.resize(copies.size() * 4);
    for (size_t i = 0; i < copies.size(); ++i) {
        regions[i * 4 + 0] = static_cast<u32>((copies[i].srcOffset - src_min) / 4);
        regions[i * 4 + 1] = static_cast<u32>((copies[i].dstOffset - dst_min) / 4);
        regions[i * 4 + 2] = static_cast<u32>(copies[i].size / 4);
        regions[i * 4 + 3] = 0;
    }
    auto& stream = rasterizer.GetBufferCache().GetStreamBuffer();
    const u64 table_size = regions.size() * sizeof(u32);
    const u64 table_offset = stream.Copy(regions.data(), table_size, align);

    scheduler.EndRendering();
    if (auto* profiler = GpuProfiler::Get()) {
        profiler->Mark(0xC0B1ull, [] { return std::string{"copy shader HLE: barrier + dispatch"}; });
    }
    const u64 src_size = src_max - src_min, dst_size = dst_max - dst_min;
    const bool needs_flush = std::ranges::any_of(ranges, [&](const CopyRange& r) {
        return runtime.IsBufferAccessed(src, r.src_offset, r.src_size) ||
               runtime.IsBufferAccessed(dst, r.dst_offset, r.dst_size, true);
    });
    if (needs_flush) {
        runtime.FlushBarriers();
    }
    const u32 count = static_cast<u32>(copies.size());
    scheduler.RecordCrumb({.name = "HLE copy shader"}, [src = src->Handle(), dst = dst->Handle(), table = stream.Handle(), src_min,
                      src_size, dst_min, dst_size, table_offset, table_size,
                      count](vk::CommandBuffer cmdbuf) {
        const std::array<vk::DescriptorBufferInfo, 3> infos{{
            {src, src_min, src_size},
            {dst, dst_min, dst_size},
            {table, table_offset, table_size},
        }};
        std::array<vk::WriteDescriptorSet, 3> writes{};
        for (u32 i = 0; i < 3; ++i) {
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eStorageBuffer,
                         .pBufferInfo = &infos[i]};
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipe.pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipe.layout, 0, writes);
        cmdbuf.pushConstants(*pipe.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(count),
                             &count);
        cmdbuf.dispatch(count, 1, 1);
    });
    for (const auto& r : ranges) {
        runtime.AccessBuffer(src, r.src_offset, r.src_size,
                             vk::PipelineStageFlagBits2::eComputeShader,
                             vk::AccessFlagBits2::eShaderRead);
        runtime.AccessBuffer(dst, r.dst_offset, r.dst_size,
                             vk::PipelineStageFlagBits2::eComputeShader,
                             vk::AccessFlagBits2::eShaderWrite);
    }
    return true;
}

struct PendingBatch {
    const VideoCore::Buffer* src = nullptr;
    const VideoCore::Buffer* dst = nullptr;
    std::vector<vk::BufferCopy> copies;
    std::vector<CopyRange> ranges;
};
static std::vector<PendingBatch> pending_batches;
static std::vector<std::pair<u64, u64>> pending_dst_vas;

void FlushPendingHleCopies(Rasterizer& rasterizer) {
    if (pending_batches.empty()) {
        return;
    }
    auto& runtime = rasterizer.GetRuntime();
    for (auto& batch : pending_batches) {
        if (!MultiCopy(rasterizer, batch.src, batch.dst, batch.copies, batch.ranges)) {
            runtime.CopyBuffer(batch.src, batch.dst, batch.copies);
        }
    }
    pending_batches.clear();
    pending_dst_vas.clear();
}

bool HasPendingHleCopies() {
    return !pending_batches.empty();
}

bool PendingHleCopiesConflict(unsigned long long addr, unsigned long long size) {
    if (pending_dst_vas.empty() || size == 0) {
        return false;
    }
    const u64 end = addr + size;
    for (const auto& [lo, hi] : pending_dst_vas) {
        if (addr < hi && end > lo) {
            return true;
        }
    }
    return false;
}

/// The copy shader's three formatted buffers: control records, source and destination.
static bool ExecuteCopyShaderHLE(const Shader::Info& info, const Gcn::ComputeProgram& cs_program,
                                 Rasterizer& rasterizer) {
    auto& runtime = rasterizer.GetRuntime();
    auto& buffer_cache = rasterizer.GetBufferCache();

    const auto ctl_buf_sharp = info.buffers[0].GetSharp(info);
    const auto src_buf_sharp = info.buffers[1].GetSharp(info);
    const auto dst_buf_sharp = info.buffers[2].GetSharp(info);
    const auto buf_stride = src_buf_sharp.GetStride();
    ASSERT(buf_stride == dst_buf_sharp.GetStride());

    struct CopyShaderControl {
        u32 dst_idx;
        u32 src_idx;
        u32 end;
    };
    static_assert(sizeof(CopyShaderControl) == 12);
    ASSERT(ctl_buf_sharp.GetStride() == sizeof(CopyShaderControl));
    const auto ctl_buf = reinterpret_cast<const CopyShaderControl*>(ctl_buf_sharp.base_address);

    static std::vector<vk::BufferCopy> copies;
    copies.clear();
    copies.reserve(cs_program.dim_x);
    // BB_COPY_SHADER_TRACE=1 (diagnostics): the copy shader's buffers, every 2 s.
    static const bool trace = std::getenv("BB_COPY_SHADER_TRACE") != nullptr;
    if (trace) {
        static auto last = std::chrono::steady_clock::now();
        static u32 calls = 0;
        ++calls;
        if (std::chrono::steady_clock::now() - last > std::chrono::seconds(2)) {
            last = std::chrono::steady_clock::now();
            u64 bytes = 0, lo = ~0ull, hi = 0;
            for (u32 i = 0; i < cs_program.dim_x; ++i) {
                bytes += u64(ctl_buf[i].end + 1) * buf_stride;
                lo = std::min<u64>(lo, u64(ctl_buf[i].dst_idx) * buf_stride);
                hi = std::max<u64>(hi, u64(ctl_buf[i].dst_idx + ctl_buf[i].end + 1) * buf_stride);
            }
            std::printf("Copy shader: %u calls; this one %u ranges %llu bytes; ctl %#llx size %u; "
                        "src %#llx size %u stride %u fmt %u/%u; dst %#llx size %u stride %u fmt "
                        "%u/%u; dst touched %#llx..%#llx; threads %u x %u, user data %u regs\n",
                        calls, cs_program.dim_x, (unsigned long long)bytes,
                        (unsigned long long)ctl_buf_sharp.base_address, u32(ctl_buf_sharp.GetSize()),
                        (unsigned long long)src_buf_sharp.base_address, u32(src_buf_sharp.GetSize()),
                        u32(buf_stride), u32(src_buf_sharp.GetDataFmt()),
                        u32(src_buf_sharp.GetNumberFmt()),
                        (unsigned long long)dst_buf_sharp.base_address, u32(dst_buf_sharp.GetSize()),
                        u32(dst_buf_sharp.GetStride()), u32(dst_buf_sharp.GetDataFmt()),
                        u32(dst_buf_sharp.GetNumberFmt()), (unsigned long long)lo,
                        (unsigned long long)hi, cs_program.num_thread_x.full,
                        cs_program.num_thread_y.full, u32(info.UserData().size()));
            calls = 0;
        }
    }

    for (u32 i = 0; i < cs_program.dim_x; i++) {
        const auto& [dst_idx, src_idx, end] = ctl_buf[i];
        const u32 local_dst_offset = dst_idx * buf_stride;
        const u32 local_src_offset = src_idx * buf_stride;
        const u32 local_size = (end + 1) * buf_stride;
        copies.emplace_back(local_src_offset, local_dst_offset, local_size);
    }

    // Grouping distance: 64 KiB max span, 256 B max destination gap so ObtainBuffer(..., true)
    // does not mark untouched 64 KiB gaps between scattered destination writes as GPU-modified
    // or upload CPU pages in those gaps.
    static const vk::DeviceSize MaxDistanceForMerge = [] {
        const char* env = std::getenv("BB_COPY_MERGE_KB");
        return env ? vk::DeviceSize(std::strtoull(env, nullptr, 10)) * 1024 : vk::DeviceSize(64_KB);
    }();
    static const vk::DeviceSize MaxDstGapForMerge = [] {
        const char* env = std::getenv("BB_COPY_DST_GAP");
        return env ? vk::DeviceSize(std::strtoull(env, nullptr, 10)) : vk::DeviceSize(256);
    }();
    static const bool merge_env = [] {
        const char* env = std::getenv("BB_COPY_HLE_MERGE");
        return !(env && env[0] == '0');
    }();
    const bool merge_groups = merge_env && !BbToggle::Experiment(64);
    if (merge_groups) {
        std::stable_sort(copies.begin(), copies.end(),
                         [](const vk::BufferCopy& a, const vk::BufferCopy& b) {
                             return a.dstOffset < b.dstOffset;
                         });
    }
    auto& scheduler = runtime.GetScheduler();
    if (HasPendingHleCopies()) {
        bool flush_prev = !scheduler.IsRendering();
        if (!flush_prev) {
            for (const auto& c : copies) {
                if (PendingHleCopiesConflict(src_buf_sharp.base_address + c.srcOffset, c.size) ||
                    PendingHleCopiesConflict(dst_buf_sharp.base_address + c.dstOffset, c.size)) {
                    flush_prev = true;
                    break;
                }
            }
        }
        if (flush_prev) {
            FlushPendingHleCopies(rasterizer);
        }
    }
    const auto is_stream_src = [&](const vk::BufferCopy& c) {
        return merge_groups && !VideoCore::GuestInPlace() &&
               c.size <= VideoCore::BufferCache::STREAM_THRESHOLD &&
               !buffer_cache.IsRegionGpuModified(src_buf_sharp.base_address + c.srcOffset, c.size);
    };
    u64 total_stream_bytes = 0;
    if (merge_groups) {
        for (const auto& c : copies) {
            if (is_stream_src(c)) {
                total_stream_bytes += Common::AlignUp<u64>(c.size, 4);
            }
        }
    }
    auto& stream = buffer_cache.GetStreamBuffer();
    u8* stream_ptr = nullptr;
    u64 stream_base_offset = 0;
    u64 stream_cursor = 0;
    if (total_stream_bytes > 0) {
        std::tie(stream_ptr, stream_base_offset) =
            stream.Map(total_stream_bytes, runtime.GetInstance().StorageMinAlignment());
    }
    struct PairBatch {
        const VideoCore::Buffer* src;
        const VideoCore::Buffer* dst;
        std::vector<vk::BufferCopy> copies;
        std::vector<CopyRange> ranges;
        std::vector<std::pair<u64, u64>> dst_vas;
        std::vector<u32> group_ends; ///< end of each group in `copies`
    };
    thread_local std::vector<PairBatch> batches;
    size_t used_batches = 0;
    for (u32 batch_start = 0, batch_end = 0; batch_end < copies.size(); batch_start = batch_end) {
        const auto& first = copies[batch_start];
        const bool stream_src = stream_ptr != nullptr && is_stream_src(first);
        auto src_min = first.srcOffset;
        auto src_max = first.srcOffset + first.size;
        auto dst_min = first.dstOffset;
        auto dst_max = first.dstOffset + first.size;
        for (++batch_end; batch_end < copies.size(); batch_end++) {
            const auto& next = copies[batch_end];
            if ((stream_ptr != nullptr && is_stream_src(next)) != stream_src) {
                break;
            }
            const auto& [src_offset, dst_offset, size] = next;
            const auto new_src_min = std::min(src_min, src_offset);
            const auto new_src_max = std::max(src_max, src_offset + size);
            const auto new_dst_min = std::min(dst_min, dst_offset);
            const auto new_dst_max = std::max(dst_max, dst_offset + size);
            if (new_dst_max - new_dst_min > MaxDistanceForMerge ||
                (merge_groups && dst_offset > dst_max + MaxDstGapForMerge) ||
                (!stream_src && new_src_max - new_src_min > MaxDistanceForMerge)) {
                break;
            }
            src_min = new_src_min;
            src_max = new_src_max;
            dst_min = new_dst_min;
            dst_max = new_dst_max;
        }
        const u64 group_src_va = src_buf_sharp.base_address + src_min;
        const u64 group_src_size = src_max - src_min;
        const u64 group_dst_va = dst_buf_sharp.base_address + dst_min;
        const u64 group_dst_size = dst_max - dst_min;
        const VideoCore::Buffer* src_buf = nullptr;
        u64 src_buf_offset = 0;
        u64 src_range_size = group_src_size;
        if (stream_src) {
            src_buf = &stream;
            src_buf_offset = stream_base_offset + stream_cursor;
        } else {
            std::tie(src_buf, src_buf_offset) =
                buffer_cache.ObtainBuffer(group_src_va, group_src_size, false);
        }
        const auto [dst_buf, dst_buf_offset] =
            buffer_cache.ObtainBuffer(group_dst_va, group_dst_size, true);
        const auto group = std::span{copies}.subspan(batch_start, batch_end - batch_start);
        if (stream_src) {
            for (auto& copy : group) {
                std::memcpy(stream_ptr + stream_cursor,
                            reinterpret_cast<const void*>(src_buf_sharp.base_address + copy.srcOffset),
                            copy.size);
                copy.srcOffset = stream_base_offset + stream_cursor;
                stream_cursor += Common::AlignUp<u64>(copy.size, 4);
                copy.dstOffset = copy.dstOffset - dst_min + dst_buf_offset;
            }
            src_range_size = (stream_base_offset + stream_cursor) - src_buf_offset;
        } else {
            for (auto& copy : group) {
                copy.srcOffset = copy.srcOffset - src_min + src_buf_offset;
                copy.dstOffset = copy.dstOffset - dst_min + dst_buf_offset;
            }
        }
        LOG_TRACE(Render_Vulkan, "HLE buffer copy: src_size = {}, dst_size = {}", src_range_size,
                  dst_max - dst_min);
        const CopyRange range{src_buf_offset, src_range_size, dst_buf_offset, dst_max - dst_min};
        if (!merge_groups) {
            if (!MultiCopy(rasterizer, src_buf, dst_buf, group, std::span{&range, 1})) {
                runtime.CopyBuffer(src_buf, dst_buf, group);
            }
            continue;
        }
        PairBatch* batch = nullptr;
        for (size_t i = 0; i < used_batches; ++i) {
            if (batches[i].src == src_buf && batches[i].dst == dst_buf) {
                batch = &batches[i];
                break;
            }
        }
        if (!batch) {
            if (used_batches == batches.size()) {
                batches.emplace_back();
            }
            batch = &batches[used_batches++];
            batch->src = src_buf;
            batch->dst = dst_buf;
            batch->copies.clear();
            batch->ranges.clear();
            batch->dst_vas.clear();
            batch->group_ends.clear();
        }
        batch->copies.insert(batch->copies.end(), group.begin(), group.end());
        batch->ranges.push_back(range);
        batch->dst_vas.emplace_back(Common::AlignDown<u64>(group_dst_va, 4096),
                                    Common::AlignUp<u64>(group_dst_va + group_dst_size, 4096));
        batch->group_ends.push_back(static_cast<u32>(batch->copies.size()));
    }
    if (stream_ptr) {
        stream.Commit();
    }
    if (!scheduler.IsRendering() && HasPendingHleCopies()) {
        FlushPendingHleCopies(rasterizer);
    }
    static const bool defer_copies = [] {
        const char* env = std::getenv("BB_COPY_HLE_DEFER");
        return !(env && env[0] == '0');
    }();
    for (size_t i = 0; i < used_batches; ++i) {
        auto& batch = batches[i];
        if (defer_copies && scheduler.IsRendering()) {
            PendingBatch* p = nullptr;
            for (auto& existing : pending_batches) {
                if (existing.src == batch.src && existing.dst == batch.dst) {
                    p = &existing;
                    break;
                }
            }
            if (!p) {
                pending_batches.emplace_back();
                p = &pending_batches.back();
                p->src = batch.src;
                p->dst = batch.dst;
            }
            p->copies.insert(p->copies.end(), batch.copies.begin(), batch.copies.end());
            p->ranges.insert(p->ranges.end(), batch.ranges.begin(), batch.ranges.end());
            pending_dst_vas.insert(pending_dst_vas.end(), batch.dst_vas.begin(),
                                   batch.dst_vas.end());
            continue;
        }
        if (MultiCopy(rasterizer, batch.src, batch.dst, batch.copies, batch.ranges)) {
            continue;
        }
        runtime.CopyBuffer(batch.src, batch.dst, batch.copies);
    }

    return true;
}

bool ExecuteShaderHLE(const Shader::Info& info, const Gcn::Regs& regs,
                      const Gcn::ComputeProgram& cs_program, Rasterizer& rasterizer) {
    // The game's buffer copy shader, from its profile (games/); nothing else is replaced.
    const u64 copy_shader = Game::BufferCopyShader();
    if (copy_shader != 0 && info.pgm_hash == copy_shader) {
        // With the game's memory in place the game's shader runs (translated), reading its
        // copy list when the GPU executes it. BB_COPY_SHADER_NATIVE=0: the list read on the CPU
        // when the dispatch is recorded, copies of our own (before). Experiment bit 2 inverts it
        // while the game runs. The layer's paged stores now write through to guest memory while
        // keeping the current mirrors in sync, so the original shader is the default here too.
        static const bool native_env = [copy_shader] {
            const char* env = std::getenv("BB_COPY_SHADER_NATIVE");
            const bool native = !env || env[0] != '0';
            const bool translated = VideoCore::GuestInPlace() && native != BbToggle::Experiment(2);
            std::printf("GPU: buffer copy shader %llx: %s%s (BB_COPY_SHADER_NATIVE=0: HLE)\n",
                        (unsigned long long)copy_shader,
                        translated ? "original translated shader" : "HLE",
                        translated && VideoCore::BufferCache::LayerPagedActive()
                            ? ", paged writes through to guest memory" : "");
            return native;
        }();
        if (VideoCore::GuestInPlace() && native_env != BbToggle::Experiment(2)) {
            return false;
        }
        // Its destinations stay in the game's memory, as when it runs as itself (see
        // Rasterizer::DispatchRecord): our translator reads what it copies there on the CPU.
        auto& buffer_cache = rasterizer.GetBufferCache();
        buffer_cache.force_writes_in_place = true;
        const bool done = ExecuteCopyShaderHLE(info, cs_program, rasterizer);
        buffer_cache.force_writes_in_place = false;
        return done;
    }
    return false;
}

} // namespace Vulkan

namespace Vulkan {

bool CopyShaderDestinations(
    const Shader::Info& info, u32 groups, u64 gap,
    std::vector<std::pair<unsigned long long, unsigned long long>>& out,
    std::vector<std::pair<unsigned long long, unsigned long long>>* reads_out) {
    const u64 copy_shader = Game::BufferCopyShader();
    if (copy_shader == 0 || info.pgm_hash != copy_shader || info.buffers.size() < 3 || groups == 0) {
        return false;
    }
    const auto ctl_sharp = info.buffers[0].GetSharp(info);
    const auto src_sharp = info.buffers[1].GetSharp(info);
    const auto dst_sharp = info.buffers[2].GetSharp(info);
    const u64 stride = dst_sharp.GetStride();
    if (ctl_sharp.GetStride() != 12 || stride == 0 || ctl_sharp.base_address == 0 ||
        u64(groups) * 12 > ctl_sharp.GetSize()) {
        return false;
    }
    struct Control {
        u32 dst_idx;
        u32 src_idx;
        u32 end;
    };
    const auto* ctl = reinterpret_cast<const Control*>(ctl_sharp.base_address);
    const auto merge_ranges =
        [&](std::vector<std::pair<unsigned long long, unsigned long long>>& vec, size_t first) {
            if (vec.size() <= first) {
                return;
            }
            std::sort(vec.begin() + first, vec.end());
            size_t merged = first;
            for (size_t i = first + 1; i < vec.size(); ++i) {
                if (vec[i].first <= vec[merged].second + gap) {
                    vec[merged].second = std::max(vec[merged].second, vec[i].second);
                } else {
                    vec[++merged] = vec[i];
                }
            }
            vec.resize(merged + 1);
            for (size_t i = first; i < vec.size(); ++i) {
                vec[i].second -= vec[i].first;
            }
        };
    const size_t first_out = out.size();
    const size_t first_reads = reads_out ? reads_out->size() : 0;
    const u64 src_stride = src_sharp.GetStride();
    const bool track_reads = reads_out && src_stride != 0 && src_sharp.base_address != 0;
    for (u32 i = 0; i < groups; ++i) {
        const u64 count = u64(ctl[i].end) + 1;
        const u64 begin = dst_sharp.base_address + u64(ctl[i].dst_idx) * stride;
        out.emplace_back(begin, begin + count * stride);
        if (track_reads) {
            const u64 r_begin = src_sharp.base_address + u64(ctl[i].src_idx) * src_stride;
            reads_out->emplace_back(r_begin, r_begin + count * src_stride);
        }
    }
    merge_ranges(out, first_out);
    if (track_reads) {
        merge_ranges(*reads_out, first_reads);
    }
    return true;
}

} // namespace Vulkan

namespace Vulkan {

bool CopyDestinationsPrecise() {
    static const bool on = [] {
        const char* env = std::getenv("BB_COPY_DEST_PRECISE");
        return !(env && env[0] == '0');
    }();
    return on;
}

unsigned long long CopyDestinationGap() {
    static const unsigned long long gap = [] {
        const char* env = std::getenv("BB_COPY_DEST_GAP");
        return env ? std::strtoull(env, nullptr, 0) : 256ull;
    }();
    return gap;
}

} // namespace Vulkan
