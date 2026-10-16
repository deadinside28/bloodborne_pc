// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: ray-traced AO, the logic without Vulkan (tests/test_rtao_geometry.cpp): the GPU records
// shared with the shaders, draw identities, the static cache's observation state machine, and the
// settings (environment at start, <user>/rtao.txt while the game runs). See docs/rtao.md.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

namespace Rtao {

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;

/// BB_RTAO=1: asked for (the object-motion capture and the pipeline cache size themselves by it
/// before the pass exists).
bool Requested();

/// One draw of the frame's geometry table (rtao_geometry.glsl's Geometry).
struct GpuGeometry {
    u32 store, vertices, instances, first_vertex;
    u64 index_address;
    u32 index_count;
    u32 index16;         // bit 0: 16-bit indices; bit 1: the buffer starts mid-word
    s32 base_vertex;
    u32 restart;
    u32 triangle_offset; // first triangle of this draw in the expand dispatch
    u32 triangles;
    u32 out_base;        // first output triangle in the triangle buffer
    u32 flags;           // GeoWorld, GeoProbe
};
static_assert(sizeof(GpuGeometry) == 56);
inline constexpr u32 GeoWorld = 1, GeoProbe = 2;

/// Per probed draw (rtao_probe.comp): world-space summary of its index-referenced vertices.
struct ProbeStats {
    u32 id, count;
    float centroid[3], min[3], max[3], first[3];
    u32 pad[2];
};
static_assert(sizeof(ProbeStats) == 64);

/// A draw's identity in the static cache: the mesh (shader, buffers, ranges) and its occurrence in
/// the frame (copies of a mesh drawn several times). Never 0.
u64 KeyOf(u64 base, u32 occurrence);

/// Same summary within the reconstruction tolerance (2 cm + 0.02% of the distance to the camera).
bool StatsMatch(const ProbeStats& a, const ProbeStats& b, const std::array<float, 3>& camera);

/// The bounding box's diagonal is longer than max_size (0: no limit).
bool TooBig(const ProbeStats& stats, float max_size);

/// An axis-aligned box (min x y z, max x y z); empty when min > max.
struct Box {
    float min[3]{1e30f, 1e30f, 1e30f};
    float max[3]{-1e30f, -1e30f, -1e30f};
    bool Empty() const { return min[0] > max[0]; }
    void Add(const Box& other);
    void Add(const float* point);
    float Diagonal() const;
    /// Squared distance from a point to the box (0 inside).
    float Distance2(const std::array<float, 3>& p) const;
};

/// A piece of a big mesh that gets its own BLAS: a range of the reordered triangles.
struct ChunkGroup {
    u32 first_triangle, triangles;
    Box box;
};

/// Big static meshes (terrain, building blocks) are cached in spatially compact pieces, each
/// instanced only within the far cut. The world-space triangles (9 floats each; NaN: inactive)
/// are split at the median centroid of the longest axis until a piece fits in `cell` metres
/// (box diagonal) and `max_triangles`, or has at most `min_triangles`. Writes the active
/// triangles reordered piece by piece to `out` (9 floats each) and returns the pieces.
std::vector<ChunkGroup> SplitTriangles(const float* triangles, u32 count, float cell, u32 max_triangles,
                                       u32 min_triangles, float* out);

/// The share of the device heap the RTAO's buffers may take, and the smallest scale it runs at.
inline constexpr double HeapShare = 0.12, MinScale = 1.0 / 3.0;

/// How far the RTAO's triangle budgets shrink so that `footprint` bytes (at the default budgets,
/// scaling linearly with them) stay within HeapShare of `heap_budget` bytes: 1 when they fit,
/// 0 when the scale would fall below MinScale (too little memory: the RTAO stays off).
double HeapScale(std::uint64_t heap_budget, std::uint64_t footprint);

/// Where everything fits (scale 1), the per-frame budget may grow up to MaxFrameGrowth times
/// (crowded scenes overflow 1.5M triangles: draws drop out and their AO pops) while the buffers
/// stay within GrowShare of the heap: `footprint` at the defaults, `per_frame` the part of it
/// that grows with the per-frame budget. Returns the growth, 1 when there is no room.
inline constexpr double GrowShare = 0.16, MaxFrameGrowth = 2.0;
double FrameGrowth(std::uint64_t heap_budget, std::uint64_t footprint, std::uint64_t per_frame);

/// First-fit ranges in a fixed pool (the cached world-space triangles); freed neighbours merge.
class RangeAllocator {
public:
    void Reset(u32 size);
    std::optional<u32> Alloc(u32 count);
    void Free(u32 offset, u32 count);
    u32 Used() const { return used; }
    u32 Size() const { return size; }
    u32 LargestFree() const;

private:
    std::map<u32, u32> free_ranges; // offset -> count
    u32 size = 0, used = 0;
};

/// What the static cache knows of a draw, without its Vulkan objects.
struct CacheState {
    u64 base = 0;
    u32 stable = 0;     // consecutive direct matching observations
    u32 evictions = 0;  // cached, then changed: three times and it stays per frame until it has
                        // been still for ForgiveAfter observations
    bool have_stats = false;
    ProbeStats stats{}; // the reference: the first observation of the current run
    bool cached = false;
};

enum class Observation { First, Same, Reordered, Changed, ChangedEvict };
/// Matching observations after which a draw that kept changing gets another chance (probed every
/// 64th pass then: about 20 s).
inline constexpr u32 ForgiveAfter = 16;

/// One probe result for a draw: `sibling_match` says it matches another copy of the same mesh (the
/// game reordered its draws). Updates the state; ChangedEvict means the caller must drop the
/// draw's BLAS (and the state is no longer cached). `multi_copy`: the mesh is drawn more than once
/// a frame, so a change is most likely a copy culled or uncovered (the slots after it shift), not
/// a move: it is evicted all the same but not counted against the draw.
Observation Observe(CacheState& state, const ProbeStats& stats, bool sibling_match, bool multi_copy,
                    const std::array<float, 3>& camera);

/// May the draw get its own BLAS now?
bool Promotable(const CacheState& state, bool skinned, bool too_big, u32 promote_after);

/// Settings: environment at start, the live file while the game runs.
struct Settings {
    bool active = true;
    float strength = 1.0f, power = 2.0f, radius = 1.5f, history = 0.5f, sigma = 1.5f;
    u32 rays = 2, debug = 0, stage = 5, denoise = 2;
    bool blur = true, replace_mask = false, depth_normals = true, use_rt_pipeline = true;
    float snap_far = 40.0f, ao_far = 40.0f, origin_offset = 0.0f;
    float max_n = 32.0f, clip_k = 2.0f, clip_floor = 0.1f;
    bool subpixel = false;
    bool blue_noise = true;    // the rays' samples from a blue-noise texture (0: IGN)
    u32 falloff = 0, noise_period = 8;
    bool cache_enabled = true;
    u32 promote_after = 8;
    float cache_max_size = 20.0f;
    float chunk_cell = 10.0f;
    float static_margin = 8.0f;  // the static BLAS reaches this far past the far cut (metres)
    u32 static_build = 2;        // the static BLAS: 0 fast trace, 1 fast build, 2 fast build + data access
    u32 static_interval = 30;    // frames between rebuilds for a changed set (not for a move); 0: every pass // > 0: meshes bigger than cache_max_size are cached in pieces of this size
    bool include_alpha = false, include_nowrite = false, ring_test = false, build_fast_trace = false;
    float foliage = 0.0f; // > 0: alpha-tested draws occlude with this probability per hit (0: left out)
    float dump_request = 0.0f;
    float recreate_request = 0.0f; // a new value: the images are made again (tests a resize)
};

/// One key=value; false for an unknown key. Values are clamped to their ranges.
bool Apply(Settings& settings, std::string_view key, float value);

/// The live file's text: key=value tokens separated by any white space (several per line too).
/// Returns how many were applied; a change of denoiser sets `history_reset`.
u32 ApplyText(Settings& settings, std::string_view text, bool& history_reset);

} // namespace Rtao
