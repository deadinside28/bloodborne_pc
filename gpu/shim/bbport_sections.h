// SPDX-License-Identifier: GPL-2.0-or-later
// bbport BB_SECTIONS=1 (diagnostics): time the draw recording thread (and, "A:" parts, the GPU
// command thread) spends per part of a draw, inclusive (nested parts count in their parent too),
// printed with the draw pipe statistics.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <x86intrin.h>

namespace BbSections {
enum Id : std::uint32_t {
    Packet,          ///< a whole packet of the draw pipe
    Task,            ///< ordered tasks (fences, signals, DMA, flips)
    DrawRecord,
    PrepareRenderState,
    BindResources,
    BindBuffers,
    BindTextures,
    BeginRendering,
    ResolveVertexBuffers,
    ResolveIndexBuffer,
    EmitVertexBuffers,
    UpdateDynamicState,
    DispatchRecord,
    ShaderHle,
    ObtainBuffer,
    Flush,
    Motion,          ///< object motion (bone palettes, index ranges)
    PipelineBind,    ///< Pipeline::BindResources (descriptors, push constants)
    Kick,            ///< ResetBindings and the hand-over to the Vulkan recording threads
    Preupload,
    EnsureResident,
    ObtainStream,  ///< ObtainBuffer's copy of a small read-only buffer into the stream buffer
    ObtainVram,    ///< ObtainBuffer's VRAM path (SynchronizeMemory)
    LayerBind,     ///< BB_LAYER_MEMORY: BufferCache::LayerBind
    // The GPU command thread (stage A), per draw handed over.
    ADraw,         ///< Rasterizer::Draw: the pipeline selection and the hand-over
    APipeline,     ///< PipelineCache::GetGraphicsPipeline
    APrepared,     ///< its prepared-draw check (TryPreparedPipeline)
    APostDraw,     ///< packing the draw for the recording thread
    ARing,         ///< constants copied into the ring (CollectRingBindings)
    ARingChecks,   ///< its checks of each buffer (GPU-written, pending writes, VRAM copies)
    ARingCopy,     ///< its copies (memcpy and flush)
    ARingSharp,    ///< its V# reads and range clamps
    ARingNote,     ///< its pending read notes (NotePendingRead)
    ADecode,       ///< decoding a whole submission (everything of stage A)
    Count,
};
inline constexpr const char* Names[Count] = {
    "packet",       "tasks",       "draw",          "render state", "bind",
    "buffers",      "textures",    "begin render",  "vertex",       "index",
    "emit vertex",  "dynamic",     "dispatch",      "HLE shader",   "ObtainBuffer",
    "flush",        "motion",      "descriptors",   "kick",         "preupload",
    "EnsureResident", "stream copy", "VRAM path", "layer bind",
    "A: draw", "A: pipeline", "A: prepared", "A: post", "A: ring", "A: ring checks",
    "A: ring copy", "A: ring sharp", "A: ring note", "A: decode",
};
inline std::array<std::atomic<std::uint64_t>, Count> cycles{};
/// Constant buffers stage A looked at and bytes it copied into the ring (with the sections).
inline std::atomic<std::uint64_t> ring_buffers{0}, ring_copies{0}, ring_bytes{0};

inline bool Enabled() {
    static const bool on = [] {
        const char* env = std::getenv("BB_SECTIONS");
        return env && env[0] == '1';
    }();
    return on;
}
/// Set on the draw recording thread (DrawPipe::Run) and the GPU command thread: only their time
/// is counted.
inline thread_local bool recording_thread = false;
inline thread_local bool command_thread = false;

struct Scope {
    explicit Scope(Id id_)
        : id{id_}, start{Enabled() && (recording_thread || command_thread) ? __rdtsc() : 0} {}
    ~Scope() {
        if (start) {
            cycles[id].fetch_add(__rdtsc() - start, std::memory_order_relaxed);
        }
    }
    Id id;
    std::uint64_t start;
};
} // namespace BbSections

#define BB_SECTION_JOIN2(a, b) a##b
#define BB_SECTION_JOIN(a, b) BB_SECTION_JOIN2(a, b)
#define BB_SECTION(id) BbSections::Scope BB_SECTION_JOIN(bb_section_scope_, __LINE__){BbSections::id}
