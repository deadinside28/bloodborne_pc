// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/rtao_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <string>

namespace Rtao {

bool Requested() {
    static const bool requested = [] {
        const char* env = std::getenv("BB_RTAO");
        return env && env[0] == '1';
    }();
    return requested;
}

u64 KeyOf(u64 base, u32 occurrence) {
    u64 key = (base ^ (u64(occurrence + 1) * 0x9E3779B97F4A7C15ull)) * 0xBF58476D1CE4E5B9ull;
    key ^= key >> 31;
    return key ? key : 1;
}

bool StatsMatch(const ProbeStats& a, const ProbeStats& b, const std::array<float, 3>& camera) {
    if (a.count != b.count || a.count == 0) {
        return false;
    }
    // World positions are rebuilt from clip space each frame: allow for the rounding, which grows
    // with the distance from the camera.
    const float dx = a.centroid[0] - camera[0], dy = a.centroid[1] - camera[1], dz = a.centroid[2] - camera[2];
    const float tol = 0.02f + 2e-4f * std::sqrt(dx * dx + dy * dy + dz * dz);
    for (int i = 0; i < 3; ++i) {
        if (std::abs(a.centroid[i] - b.centroid[i]) > tol || std::abs(a.min[i] - b.min[i]) > tol ||
            std::abs(a.max[i] - b.max[i]) > tol || std::abs(a.first[i] - b.first[i]) > tol) {
            return false;
        }
    }
    return true;
}

bool TooBig(const ProbeStats& stats, float max_size) {
    if (max_size <= 0.0f) {
        return false;
    }
    const float dx = stats.max[0] - stats.min[0], dy = stats.max[1] - stats.min[1], dz = stats.max[2] - stats.min[2];
    return dx * dx + dy * dy + dz * dz > max_size * max_size;
}

void Box::Add(const Box& other) {
    for (int i = 0; i < 3; ++i) {
        min[i] = std::min(min[i], other.min[i]);
        max[i] = std::max(max[i], other.max[i]);
    }
}

void Box::Add(const float* p) {
    for (int i = 0; i < 3; ++i) {
        min[i] = std::min(min[i], p[i]);
        max[i] = std::max(max[i], p[i]);
    }
}

float Box::Diagonal() const {
    if (Empty()) {
        return 0.0f;
    }
    const float dx = max[0] - min[0], dy = max[1] - min[1], dz = max[2] - min[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float Box::Distance2(const std::array<float, 3>& p) const {
    float d2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float v = std::max({min[i] - p[i], 0.0f, p[i] - max[i]});
        d2 += v * v;
    }
    return d2;
}

std::vector<ChunkGroup> SplitTriangles(const float* triangles, u32 count, float cell, u32 max_triangles,
                                       u32 min_triangles, float* out) {
    std::vector<u32> order;
    std::vector<std::array<float, 3>> centre(count);
    order.reserve(count);
    for (u32 t = 0; t < count; ++t) {
        const float* v = triangles + size_t(t) * 9;
        bool active = true;
        for (int k = 0; k < 9; ++k) {
            active = active && std::isfinite(v[k]);
        }
        if (active) {
            for (int i = 0; i < 3; ++i) {
                centre[t][i] = (v[i] + v[3 + i] + v[6 + i]) / 3.0f;
            }
            order.push_back(t);
        }
    }
    std::vector<ChunkGroup> groups;
    const auto bounds = [&](size_t begin, size_t end) {
        Box box;
        for (size_t i = begin; i < end; ++i) {
            const float* v = triangles + size_t(order[i]) * 9;
            box.Add(v);
            box.Add(v + 3);
            box.Add(v + 6);
        }
        return box;
    };
    // Depth first, so neighbouring pieces stay next to each other in the output.
    std::vector<std::pair<size_t, size_t>> stack{{0, order.size()}};
    while (!stack.empty()) {
        const auto [begin, end] = stack.back();
        stack.pop_back();
        const size_t n = end - begin;
        if (n == 0) {
            continue;
        }
        const Box box = bounds(begin, end);
        Box centres;
        for (size_t i = begin; i < end; ++i) {
            centres.Add(centre[order[i]].data());
        }
        int axis = 0;
        for (int i = 1; i < 3; ++i) {
            if (centres.max[i] - centres.min[i] > centres.max[axis] - centres.min[axis]) {
                axis = i;
            }
        }
        const bool fits = n <= max_triangles && box.Diagonal() <= cell;
        if (fits || n <= min_triangles || centres.max[axis] <= centres.min[axis]) {
            groups.push_back({u32(begin), u32(n), box});
            continue;
        }
        const size_t mid = begin + n / 2;
        std::nth_element(order.begin() + begin, order.begin() + mid, order.begin() + end,
                         [&](u32 a, u32 b) { return centre[a][axis] < centre[b][axis]; });
        stack.push_back({mid, end}); // the lower half is taken first
        stack.push_back({begin, mid});
    }
    for (size_t i = 0; i < order.size(); ++i) {
        std::copy_n(triangles + size_t(order[i]) * 9, 9, out + i * 9);
    }
    return groups;
}

double HeapScale(std::uint64_t heap_budget, std::uint64_t footprint) {
    if (footprint == 0) {
        return 1.0;
    }
    const double scale = std::min(1.0, HeapShare * double(heap_budget) / double(footprint));
    return scale < MinScale ? 0.0 : scale;
}

double FrameGrowth(std::uint64_t heap_budget, std::uint64_t footprint, std::uint64_t per_frame) {
    const double room = GrowShare * double(heap_budget) - double(footprint);
    if (per_frame == 0 || room <= 0.0) {
        return 1.0;
    }
    return std::min(MaxFrameGrowth, 1.0 + room / double(per_frame));
}

void RangeAllocator::Reset(u32 new_size) {
    free_ranges.clear();
    size = new_size;
    used = 0;
    if (size) {
        free_ranges[0] = size;
    }
}

std::optional<u32> RangeAllocator::Alloc(u32 count) {
    if (count == 0) {
        return std::nullopt;
    }
    for (auto it = free_ranges.begin(); it != free_ranges.end(); ++it) {
        if (it->second >= count) {
            const u32 offset = it->first, rest = it->second - count;
            free_ranges.erase(it);
            if (rest) {
                free_ranges[offset + count] = rest;
            }
            used += count;
            return offset;
        }
    }
    return std::nullopt;
}

void RangeAllocator::Free(u32 offset, u32 count) {
    if (count == 0) {
        return;
    }
    used -= count;
    auto next = free_ranges.lower_bound(offset);
    if (next != free_ranges.end() && offset + count == next->first) { // joins the range after
        count += next->second;
        next = free_ranges.erase(next);
    }
    if (next != free_ranges.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == offset) { // and the one before
            prev->second += count;
            return;
        }
    }
    free_ranges[offset] = count;
}

u32 RangeAllocator::LargestFree() const {
    u32 largest = 0;
    for (const auto& [offset, count] : free_ranges) {
        largest = std::max(largest, count);
    }
    return largest;
}

Observation Observe(CacheState& state, const ProbeStats& stats, bool sibling_match, bool multi_copy,
                    const std::array<float, 3>& camera) {
    if (!state.have_stats) {
        state.stats = stats;
        state.have_stats = true;
        state.stable = 1;
        return Observation::First;
    }
    if (StatsMatch(state.stats, stats, camera)) {
        ++state.stable; // the reference stays the first observation: slow drift is not static
        if (state.evictions >= 3 && state.stable >= ForgiveAfter) {
            state.evictions = 2; // still for a while: one more chance
        }
        return Observation::Same;
    }
    if (sibling_match) {
        state.stable = 0; // the order is not stable: no promotion from this slot for now
        return Observation::Reordered;
    }
    const bool was_cached = state.cached;
    if (was_cached) { // cached, then it moved (a door, a lift): back to per frame
        state.cached = false;
        state.evictions += multi_copy ? 0 : 1;
    }
    state.stats = stats;
    state.stable = 1;
    return was_cached ? Observation::ChangedEvict : Observation::Changed;
}

bool Promotable(const CacheState& state, bool skinned, bool too_big, u32 promote_after) {
    return !skinned && !state.cached && state.evictions < 3 && state.stable >= promote_after && !too_big;
}

bool Apply(Settings& s, std::string_view key, float value) {
    const auto flag = [&](bool& target) { target = value != 0.0f; };
    if (key == "enabled") flag(s.active);
    else if (key == "strength") s.strength = std::clamp(value, 0.0f, 1.0f);
    else if (key == "power") s.power = std::clamp(value, 0.1f, 8.0f);
    else if (key == "radius") s.radius = std::max(value, 0.05f);
    else if (key == "history") s.history = std::clamp(value, 0.0f, 0.98f);
    else if (key == "sigma") s.sigma = std::clamp(value, 0.0f, 4.0f);
    else if (key == "rays") s.rays = std::clamp(u32(std::max(value, 0.0f)), 1u, 32u);
    else if (key == "debug") s.debug = std::clamp(u32(std::max(value, 0.0f)), 0u, 4u);
    else if (key == "stage") s.stage = std::clamp(u32(std::max(value, 0.0f)), 0u, 5u);
    else if (key == "denoise") s.denoise = std::clamp(u32(std::max(value, 0.0f)), 1u, 2u);
    else if (key == "blur") flag(s.blur);
    else if (key == "replace") flag(s.replace_mask);
    else if (key == "normals") flag(s.depth_normals);
    else if (key == "pipeline") flag(s.use_rt_pipeline);
    else if (key == "snap") s.snap_far = std::max(value, 0.0f);
    else if (key == "far") s.ao_far = std::max(value, 0.0f);
    else if (key == "offset") s.origin_offset = std::clamp(value, 0.0f, 0.1f);
    else if (key == "maxn") s.max_n = std::clamp(value, 1.0f, 255.0f);
    else if (key == "clipk") s.clip_k = std::clamp(value, 0.1f, 10.0f);
    else if (key == "clipfloor") s.clip_floor = std::clamp(value, 0.0f, 1.0f);
    else if (key == "subpixel") flag(s.subpixel);
    else if (key == "noise") flag(s.blue_noise);
    else if (key == "falloff") s.falloff = value != 0.0f ? 1u : 0u;
    else if (key == "period") s.noise_period = std::clamp(u32(std::max(value, 0.0f)), 1u, 1024u);
    else if (key == "cache") flag(s.cache_enabled);
    else if (key == "promote") s.promote_after = std::clamp(u32(std::max(value, 0.0f)), 2u, 600u);
    else if (key == "maxsize") s.cache_max_size = std::max(value, 0.0f);
    else if (key == "chunk") s.chunk_cell = std::max(value, 0.0f);
    else if (key == "staticbuild") s.static_build = std::clamp(u32(std::max(value, 0.0f)), 0u, 2u);
    else if (key == "margin") s.static_margin = std::clamp(value, 0.5f, 100.0f);
    else if (key == "rebuild") s.static_interval = std::clamp(u32(std::max(value, 0.0f)), 0u, 6000u);
    else if (key == "alpha") flag(s.include_alpha);
    else if (key == "foliage") s.foliage = std::clamp(value, 0.0f, 1.0f);
    else if (key == "decals") flag(s.include_nowrite);
    else if (key == "ringtest") flag(s.ring_test);
    else if (key == "build") flag(s.build_fast_trace);
    else if (key == "dump") s.dump_request = value;
    else if (key == "recreate") s.recreate_request = value;
    else return false;
    return true;
}

u32 ApplyText(Settings& settings, std::string_view text, bool& history_reset) {
    u32 applied = 0;
    size_t pos = 0;
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (pos < text.size()) {
        while (pos < text.size() && space(text[pos])) ++pos;
        size_t end = pos;
        while (end < text.size() && !space(text[end])) ++end;
        const std::string_view token = text.substr(pos, end - pos);
        pos = end;
        if (token.empty() || token[0] == '#') {
            // A comment runs to the end of its line.
            if (!token.empty()) {
                while (pos < text.size() && text[pos] != '\n') ++pos;
            }
            continue;
        }
        const size_t eq = token.find('=');
        if (eq == std::string_view::npos || eq == 0) {
            continue;
        }
        const std::string value(token.substr(eq + 1));
        char* parse_end = nullptr;
        const float number = std::strtof(value.c_str(), &parse_end);
        if (parse_end == value.c_str()) {
            continue;
        }
        const u32 denoise_before = settings.denoise;
        if (Apply(settings, token.substr(0, eq), number)) {
            ++applied;
            history_reset |= settings.denoise != denoise_before;
        }
    }
    return applied;
}

} // namespace Rtao
