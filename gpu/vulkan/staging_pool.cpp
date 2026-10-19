// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan/staging_pool.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <numeric>

#include "common/alignment.h"
#include "common/assert.h"
#include "core/devtools/widget/gpu_memory.h"
#include "render/scheduler.h"
#include "vulkan/instance.h"

namespace Vulkan {

using VideoCore::MemoryType;

namespace {

constexpr u64 MiB = 1ull << 20;
constexpr u64 SegmentSize = 16 * MiB;
/// The kernel populates staging memory on first touch (~2 ms per 16 MiB): memory given back
/// between streaming bursts a few seconds apart was made again at 0.3 GB/s. It lives 30 s idle,
/// and the upload segments are populated at startup.
constexpr u64 IdleFrames = 3000;
constexpr u64 PrewarmSegments = 8;
/// Requests in a segment start on this much at least (copies into images need their texel size,
/// storage bindings their offset alignment; 256 covers both on every GPU).
constexpr u64 MinAlignment = 256;
/// Buffers of their own are made in whole MiB, and a free one serves requests down to 2/3 of it.
constexpr u64 WholeGranule = MiB;

constexpr bool Mapped(MemoryType type) {
    return type != MemoryType::DeviceLocal;
}

} // namespace

StagingBufferPool::StagingBufferPool(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    const char* keep = std::getenv("BB_STAGING_KEEP_MB");
    keep_segments = (keep ? std::strtoull(keep, nullptr, 10) : 512) * MiB / SegmentSize;
    Segments& uploads = segments[u32(MemoryType::HostUncached)];
    for (u64 i = 0; i < PrewarmSegments; ++i) {
        Segment& segment = NewSegment(uploads, MemoryType::HostUncached);
        if (!segment.buffer->mapped_data.empty()) {
            std::memset(segment.buffer->mapped_data.data(), 0, segment.buffer->mapped_data.size());
        }
    }
}

StagingBufferPool::~StagingBufferPool() = default;

StagingBufferRef StagingBufferPool::Request(u64 size, MemoryType type, u64 alignment,
                                            bool deferred) {
    if (deferred || size > SegmentSize) {
        return FromWhole(size, type, deferred);
    }
    return FromSegments(segments[u32(type)], size, alignment, type);
}

StagingBufferPool::Segment& StagingBufferPool::NewSegment(Segments& list, MemoryType type) {
    list.list.push_back(Segment{
        .buffer = std::make_unique<VideoCore::Buffer>(instance, 0, SegmentSize, type,
                                                      "StagingBufferPool:Segment"),
        .last_frame = frame,
    });
    return list.list.back();
}

StagingBufferRef StagingBufferPool::FromSegments(Segments& list, u64 size, u64 alignment,
                                                 MemoryType type) {
    // Host writes to non-coherent memory are flushed in whole atoms: requests start and end on
    // them so that two never share one.
    u64 align = std::max(alignment, MinAlignment);
    if (Mapped(type)) {
        align = std::lcm(align, std::max<u64>(instance.NonCoherentAtomSize(), 1));
        size = Common::AlignUp(size, std::max<u64>(instance.NonCoherentAtomSize(), 1));
    }
    const auto fits = [&](const Segment& segment) {
        return Common::AlignUp(segment.used, align) + size <= SegmentSize;
    };
    Segment* segment = list.list.empty() ? nullptr : &list.list[list.current];
    if (!segment || !fits(*segment)) {
        // The next segment the GPU is done with starts empty again; else a new one.
        segment = nullptr;
        for (size_t step = 1; step <= list.list.size() && !segment; ++step) {
            const size_t index = (list.current + step) % list.list.size();
            Segment& candidate = list.list[index];
            if (candidate.last_tick == 0 || scheduler.IsFree(candidate.last_tick)) {
                candidate.used = 0;
                list.current = index;
                segment = &candidate;
            }
        }
        if (!segment) {
            segment = &NewSegment(list, type);
            list.current = list.list.size() - 1;
        }
    }
    const u64 offset = Common::AlignUp(segment->used, align);
    segment->used = offset + size;
    segment->last_tick = scheduler.CurrentTick();
    segment->last_frame = frame;
    VideoCore::Buffer& buffer = *segment->buffer;
    return {
        .buffer = &buffer,
        .offset = offset,
        .size = size,
        .mapped = buffer.mapped_data.empty() ? nullptr : buffer.mapped_data.data() + offset,
        .type = type,
    };
}

StagingBufferRef StagingBufferPool::FromWhole(u64 size, MemoryType type, bool deferred) {
    auto& list = wholes[u32(type)];
    // The smallest free one at least as large and at most half as large again.
    Whole* best = nullptr;
    for (Whole& whole : list) {
        const u64 capacity = whole.buffer->SizeBytes();
        if (whole.held || capacity < size || capacity * 2 > size * 3 ||
            (whole.last_tick != 0 && !scheduler.IsFree(whole.last_tick))) {
            continue;
        }
        if (!best || capacity < best->buffer->SizeBytes()) {
            best = &whole;
        }
    }
    if (!best) {
        list.push_back(Whole{
            .buffer = std::make_unique<VideoCore::Buffer>(instance, 0,
                                                          Common::AlignUp(size, WholeGranule),
                                                          type, "StagingBufferPool:Whole"),
        });
        best = &list.back();
    }
    best->last_tick = scheduler.CurrentTick();
    best->last_frame = frame;
    best->held = deferred;
    VideoCore::Buffer& buffer = *best->buffer;
    return {
        .buffer = &buffer,
        .offset = 0,
        .size = size,
        .mapped = buffer.mapped_data.empty() ? nullptr : buffer.mapped_data.data(),
        .type = type,
    };
}

void StagingBufferPool::FreeDeferred(const StagingBufferRef& ref) {
    auto& list = wholes[u32(ref.type)];
    const auto it = std::ranges::find_if(
        list, [&](const Whole& whole) { return whole.buffer.get() == ref.buffer; });
    ASSERT_MSG(it != list.end() && it->held, "FreeDeferred of memory not held");
    it->held = false;
    it->last_tick = scheduler.CurrentTick();
    it->last_frame = frame;
}

void StagingBufferPool::TickFrame() {
    ++frame;
    for (u32 type = 0; type < NumTypes; ++type) {
        TrimIdle(MemoryType(type));
    }
    PublishStats();
}

void StagingBufferPool::TrimIdle(MemoryType type) {
    const auto idle = [&](u64 last_frame, u64 last_tick) {
        return frame - last_frame >= IdleFrames && (last_tick == 0 || scheduler.IsFree(last_tick));
    };
    Segments& list = segments[u32(type)];
    const u64 keep = type == MemoryType::HostUncached ? keep_segments : 0;
    for (size_t i = list.list.size(); i-- > 0 && list.list.size() > keep;) {
        if (i == list.current || !idle(list.list[i].last_frame, list.list[i].last_tick)) {
            continue;
        }
        list.list.erase(list.list.begin() + i);
        if (i < list.current) {
            --list.current;
        }
    }
    std::erase_if(wholes[u32(type)], [&](const Whole& whole) {
        return !whole.held && idle(whole.last_frame, whole.last_tick);
    });
}

void StagingBufferPool::PublishStats() const {
    using namespace Core::Devtools::Widget;
    if (!GpuMemoryViewer::IsEnabled()) {
        return;
    }
    static constexpr std::array<const char*, NumTypes> names = {"DeviceLocal", "HostUncached",
                                                                "HostCached"};
    GpuMemoryGroup group{.name = "Staging"};
    for (size_t i = 0; i < NumTypes; ++i) {
        u64 bytes = 0;
        for (const Segment& segment : segments[i].list) {
            bytes += segment.buffer->SizeBytes();
        }
        for (const Whole& whole : wholes[i]) {
            bytes += whole.buffer->SizeBytes();
        }
        if (bytes != 0) {
            group.rows.push_back({.name = names[i], .bytes = bytes});
        }
    }
    GpuMemoryViewer::Publish(std::move(group));
}

} // namespace Vulkan
