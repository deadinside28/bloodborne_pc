// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport HDR transport (render/hdr_channel.h): the 16-bit chain of hdr_mode=native+. The
// interface machinery is the source port's (vk_hdr_channel.cpp): the game shader's SPIR-V is
// read for the OpNames and the Binding/Location decorations (this recompiler names the same
// things the same: fs_imgN, fs_sampN, fs_in_attrN[_p], ssbo_N, frag_colorN — gpu/shader/
// backend/spirv/spirv_emit_context.cpp), and the HDR shader built for a composite variant gets
// the game shader's bindings and locations patched in.

#include "render/hdr_channel.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include "bbport_settings.h"
#include "bbgpu/shaders/bb_color_grading_frag.h"
#include "bbgpu/shaders/bb_color_grading_hw_frag.h"
#include "bbgpu/shaders/bb_composite_hdr_frag.h"
#include "bbgpu/shaders/bb_composite_hdr_hw_frag.h"
#include "bbgpu/shaders/bb_copy_frag.h"
#include "bbgpu/shaders/bb_copy_hw_frag.h"
#include "bbgpu/shaders/bb_display_copy_frag.h"
#include "bbgpu/shaders/bb_glow_source_frag.h"
#include "bbgpu/shaders/bb_glow_source_hw_frag.h"
#include "common/logging/log.h"
#include "render/bb_tonemap_variants.h"
#include "vulkan/instance.h"
#include "vulkan/shader_module.h"

namespace Vulkan {

namespace {

void SetSpirvSpecConstant(std::vector<u32>& spv, u32 spec_id, u32 value) {
    u32 target = 0;
    for (size_t i = 5; i < spv.size();) {
        const u32 count = spv[i] >> 16, opcode = spv[i] & 0xffff;
        if (count == 0 || count > spv.size() - i) return;
        if (opcode == 71 && count >= 4 && spv[i + 2] == 1 && spv[i + 3] == spec_id) {
            target = spv[i + 1];
        } else if (opcode == 50 && count == 4 && target && spv[i + 2] == target) {
            spv[i + 3] = value;
            return;
        }
        i += count;
    }
}

float EnvFloat(const char* name, float fallback, float lo, float hi) {
    const char* v = std::getenv(name);
    if (!v || !*v) {
        return fallback;
    }
    const float f = std::strtof(v, nullptr);
    return f < lo ? lo : f > hi ? hi : f;
}

const TonemapVariant* FindVariant(u64 hash) {
    if (hash == 0 || hash > 0xffffffffULL) {
        return nullptr;
    }
    const auto it = std::lower_bound(TonemapVariants.begin(), TonemapVariants.end(), u32(hash),
                                     [](const TonemapVariant& v, u32 h) { return v.hash < h; });
    return it != TonemapVariants.end() && it->hash == u32(hash) ? &*it : nullptr;
}

// SPIR-V reading and patching: the names (OpName) and the Binding and Location decorations of a
// module.
constexpr u32 SpvOpName = 5, SpvOpDecorate = 71, SpvOpSpecConstantTrue = 48,
              SpvOpSpecConstantFalse = 49, SpvDecorationSpecId = 1, SpvDecorationLocation = 30,
              SpvDecorationBinding = 33;

struct SpvInterface {
    std::unordered_map<std::string, std::vector<u32>> ids; ///< name -> ids
    std::unordered_map<u32, u32> binding;                  ///< id -> Binding
    std::unordered_map<u32, u32> location;                 ///< id -> Location

    std::optional<u32> Binding(const std::string& name) const {
        return Find(name, binding);
    }
    std::optional<u32> Location(const std::string& name) const {
        return Find(name, location);
    }

private:
    std::optional<u32> Find(const std::string& name,
                            const std::unordered_map<u32, u32>& decorations) const {
        const auto it = ids.find(name);
        if (it == ids.end()) {
            return std::nullopt;
        }
        for (const u32 id : it->second) {
            if (const auto d = decorations.find(id); d != decorations.end()) {
                return d->second;
            }
        }
        return std::nullopt;
    }
};

SpvInterface ReadInterface(std::span<const u32> spv) {
    SpvInterface out;
    for (size_t i = 5; i < spv.size();) {
        const u32 count = spv[i] >> 16, opcode = spv[i] & 0xffff;
        if (count == 0 || i + count > spv.size()) {
            break;
        }
        if (opcode == SpvOpName && count >= 3) {
            const char* text = reinterpret_cast<const char*>(&spv[i + 2]);
            out.ids[std::string(text, strnlen(text, (count - 2) * 4))].push_back(spv[i + 1]);
        } else if (opcode == SpvOpDecorate && count >= 4) {
            if (spv[i + 2] == SpvDecorationBinding) {
                out.binding[spv[i + 1]] = spv[i + 3];
            } else if (spv[i + 2] == SpvDecorationLocation) {
                out.location[spv[i + 1]] = spv[i + 3];
            }
        }
        i += count;
    }
    return out;
}

/// Sets the `decoration` literal of every variable called `name`. False if there is none.
bool SetDecoration(std::vector<u32>& spv, const SpvInterface& names, const std::string& name,
                   u32 decoration, u32 value) {
    const auto it = names.ids.find(name);
    if (it == names.ids.end()) {
        return false;
    }
    bool done = false;
    for (size_t i = 5; i < spv.size();) {
        const u32 count = spv[i] >> 16, opcode = spv[i] & 0xffff;
        if (count == 0) {
            break;
        }
        if (opcode == SpvOpDecorate && count >= 4 && spv[i + 2] == decoration &&
            std::ranges::find(it->second, spv[i + 1]) != it->second.end()) {
            spv[i + 3] = value;
            done = true;
        }
        i += count;
    }
    return done;
}

/// Sets a boolean specialization constant (OpSpecConstantTrue / False).
void SetSpecBool(std::vector<u32>& spv, u32 spec_id, bool value) {
    u32 target = 0;
    for (size_t i = 5; i < spv.size();) {
        const u32 count = spv[i] >> 16, opcode = spv[i] & 0xffff;
        if (count == 0) {
            return;
        }
        if (opcode == SpvOpDecorate && count >= 4 && spv[i + 2] == SpvDecorationSpecId &&
            spv[i + 3] == spec_id) {
            target = spv[i + 1];
        } else if ((opcode == SpvOpSpecConstantTrue || opcode == SpvOpSpecConstantFalse) &&
                   count == 3 && target && spv[i + 2] == target) {
            spv[i] = (count << 16) | (value ? SpvOpSpecConstantTrue : SpvOpSpecConstantFalse);
            return;
        }
        i += count;
    }
}

// The delta shader built for each composite variant (by pgm_hash). Never destroyed, like the
// channel (the device is gone before static objects at exit).
struct CompositeShader {
    std::vector<u32> spv;
    vk::ShaderModule module{};
    bool conflicting = false; ///< two permutations with different interfaces: not used
};
std::mutex composite_mutex;
std::unordered_map<u64, CompositeShader>* composites = new std::unordered_map<u64, CompositeShader>;

/// A game pixel shader input: its location, and whether the shader reads it per vertex
/// (fs_in_attrN_p, the recompiler's manual interpolation) or as the GPU interpolates it
/// (fs_in_attrN, with fragment-shader barycentrics enabled).
struct GameInput {
    u32 location;
    bool per_vertex;
};

std::optional<GameInput> FindInput(const SpvInterface& in, int attr) {
    if (const auto location = in.Location(fmt::format("fs_in_attr{}_p", attr))) {
        return GameInput{*location, true};
    }
    if (const auto location = in.Location(fmt::format("fs_in_attr{}", attr))) {
        return GameInput{*location, false};
    }
    return std::nullopt;
}

/// The variant's HDR delta shader, or why there is none. `per_vertex` says which build of it.
/// The recompiler's constant buffer is always a storage buffer (ssbo_N): only the two builds of
/// that (the source port also built uniform-buffer ones for its native compiler).
std::optional<std::vector<u32>> BuildComposite(const TonemapVariant& v, std::span<const u32> game,
                                               std::string* why, bool* per_vertex) {
    const SpvInterface in = ReadInterface(game);
    // The constant buffer: the game shader's only buffer (several ssbo_N aliases share it).
    std::optional<u32> buffer_index, buffer_binding;
    for (const auto& [name, ids] : in.ids) {
        if (name.rfind("ssbo_", 0) != 0) {
            if (name.rfind("ubo_", 0) == 0) {
                *why = "a uniform buffer (this recompiler uses storage buffers)";
                return std::nullopt;
            }
            continue;
        }
        const u32 index = u32(std::strtoul(name.c_str() + 5, nullptr, 10));
        if (buffer_index && *buffer_index != index) {
            *why = "several buffers";
            return std::nullopt;
        }
        buffer_index = index;
        buffer_binding = in.Binding(name);
    }
    if (!buffer_index || !buffer_binding) {
        *why = "no constant buffer";
        return std::nullopt;
    }
    if (!in.ids.contains(fmt::format("fs_img{}", int(v.img_scene))) ||
        !in.ids.contains(fmt::format("fs_samp{}", int(v.img_scene)))) {
        *why = "unexpected images";
        return std::nullopt;
    }
    // One sampler per image, in the same order (each fetch uses its own S#).
    u32 images = 0, samplers = 0;
    while (in.ids.contains(fmt::format("fs_img{}", images))) {
        ++images;
    }
    while (in.ids.contains(fmt::format("fs_samp{}", samplers))) {
        ++samplers;
    }
    if (images != samplers) {
        *why = fmt::format("{} images but {} samplers", images, samplers);
        return std::nullopt;
    }
    const auto image = [&](int index) { return in.Binding(fmt::format("fs_img{}", index)); };
    const auto sampler = [&](int index) { return in.Binding(fmt::format("fs_samp{}", index)); };
    // Inputs: per vertex when the game's shader reads all of them so (the HDR shader then
    // computes the game's values bit for bit); else the build that reads them as the GPU
    // interpolates them, at the same locations.
    const auto scene_in = FindInput(in, v.attr_scene);
    const auto mult_in = FindInput(in, v.attr_mult);
    const auto bloom_in = FindInput(in, v.attr_bloom);
    const auto vig_in = v.attr_vig >= 0 ? FindInput(in, v.attr_vig) : scene_in;
    const auto bindings = {image(v.img_scene), image(v.img_mult), image(v.img_bloom),
                           sampler(v.img_scene), sampler(v.img_mult), sampler(v.img_bloom)};
    if (std::ranges::any_of(bindings, [](const auto& b) { return !b.has_value(); })) {
        *why = "image bindings not found";
        return std::nullopt;
    }
    if (!scene_in || !mult_in || !bloom_in || !vig_in) {
        *why = "inputs not found";
        return std::nullopt;
    }
    *per_vertex =
        scene_in->per_vertex && mult_in->per_vertex && bloom_in->per_vertex && vig_in->per_vertex;
    const auto scene_loc = std::optional<u32>{scene_in->location};
    const auto mult_loc = std::optional<u32>{mult_in->location};
    const auto bloom_loc = std::optional<u32>{bloom_in->location};
    const auto vig_loc = std::optional<u32>{vig_in->location};

    const std::span<const u32> words =
        *per_vertex ? std::span<const u32>{BB_COMPOSITE_HDR_HW_FRAG}
                    : std::span<const u32>{BB_COMPOSITE_HDR_FRAG};
    std::vector<u32> spv{words.begin(), words.end()};
    const SpvInterface ours = ReadInterface(spv);
    const auto it = bindings.begin();
    const bool ok =
        SetDecoration(spv, ours, "bb_ssbo", SpvDecorationBinding, *buffer_binding) &&
        SetDecoration(spv, ours, "bb_img_scene", SpvDecorationBinding, *it[0]) &&
        SetDecoration(spv, ours, "bb_img_mult", SpvDecorationBinding, *it[1]) &&
        SetDecoration(spv, ours, "bb_img_bloom", SpvDecorationBinding, *it[2]) &&
        SetDecoration(spv, ours, "bb_samp_scene", SpvDecorationBinding, *it[3]) &&
        SetDecoration(spv, ours, "bb_samp_mult", SpvDecorationBinding, *it[4]) &&
        SetDecoration(spv, ours, "bb_samp_bloom", SpvDecorationBinding, *it[5]) &&
        SetDecoration(spv, ours, "bb_in_scene", SpvDecorationLocation, *scene_loc) &&
        SetDecoration(spv, ours, "bb_in_mult", SpvDecorationLocation, *mult_loc) &&
        SetDecoration(spv, ours, "bb_in_bloom", SpvDecorationLocation, *bloom_loc) &&
        SetDecoration(spv, ours, "bb_in_vig", SpvDecorationLocation, *vig_loc);
    if (!ok) {
        *why = "the HDR shader's decorations were not found";
        return std::nullopt;
    }
    // Vanilla+: extend the game's curve above the knee up to the peak (in the game's white
    // units); the final pass encodes for the display. (The source port's PsychoV30 (scene)
    // override ran in the same shader through bb_hdr2 push constants; not ported.)
    const float paper =
        std::clamp(BbSettings::Get().hdr_paper_white.load(std::memory_order_relaxed),
                   BbSettings::HdrPaperWhiteMin, BbSettings::HdrPaperWhiteMax);
    const float peak = std::max(HdrChannel::PeakNits() / paper, HdrChannel::Knee() + 0.05f);
    SetSpirvSpecConstant(spv, 1, std::bit_cast<u32>(peak));
    SetSpirvSpecConstant(spv, 2, std::bit_cast<u32>(HdrChannel::Knee()));
    SetSpirvSpecConstant(spv, 10, v.curve);
    SetSpecBool(spv, 11, v.luminance);
    SetSpirvSpecConstant(spv, 12, v.pre);
    SetSpecBool(spv, 13, v.pre_luminance);
    SetSpecBool(spv, 14, v.cos4);
    SetSpecBool(spv, 15, v.vig888);
    SetSpecBool(spv, 16, v.gamma);
    SetSpecBool(spv, 17, v.additive);
    SetSpirvSpecConstant(spv, 18, *buffer_index);
    SetSpecBool(spv, 20, v.gain);
    SetSpecBool(spv, 21, v.floor);
    SetSpecBool(spv, 22, v.scene_mat);
    SetSpecBool(spv, 23, v.glow_mat);
    SetSpecBool(spv, 24, v.grade);
    SetSpecBool(spv, 25, true); // the 16-bit chain (this port's only mode)
    return spv;
}

constexpr const char* CurveNames[] = {"exp curve", "extended Reinhard", "log curve", "no curve"};
} // namespace

void HdrChannel::NoteGameShader(u64 pgm_hash, std::span<const u32> spv) {
    if (!Generic()) {
        return;
    }
    const TonemapVariant* v = FindVariant(pgm_hash);
    if (!v || !v->standard || v->additive) {
        return; // additive variants output HDR for a later pass: nothing to do here
    }
    std::string why;
    bool per_vertex = false;
    auto built = BuildComposite(*v, spv, &why, &per_vertex);
    std::scoped_lock lock{composite_mutex};
    auto [it, fresh] = composites->try_emplace(pgm_hash);
    if (!built) {
        if (fresh) {
            LOG_WARNING(Render_Vulkan, "HDR pass: composite variant {:08x} left in SDR: {}",
                        u32(pgm_hash), why);
            it->second.conflicting = true;
        }
        return;
    }
    if (fresh) {
        it->second.spv = std::move(*built);
        std::printf("HDR pass: composite variant %08x ready (%s%s, %s)\n",
                 u32(pgm_hash), CurveNames[v->curve < 4 ? v->curve : 3],
                 v->luminance ? " on luminance" : "",
                 per_vertex ? "inputs per vertex" : "inputs interpolated by the GPU");
    } else if (!it->second.conflicting && it->second.spv != *built) {
        it->second.conflicting = true;
        LOG_WARNING(Render_Vulkan,
                    "HDR pass: composite variant {:08x} has permutations with different "
                    "interfaces, left in SDR",
                    u32(pgm_hash));
    }
}

vk::ShaderModule HdrChannel::CompositeModule(const Instance& instance, u64 pgm_hash) {
    std::scoped_lock lock{composite_mutex};
    const auto it = composites->find(pgm_hash);
    if (it == composites->end() || it->second.conflicting || it->second.spv.empty()) {
        return {};
    }
    if (!it->second.module) {
        it->second.module = CompileSPV(it->second.spv, instance.GetDevice());
    }
    return it->second.module;
}

bool HdrChannel::Enabled() {
    const auto& s = BbSettings::Get();
    static const bool enabled = [&] {
        const bool on = s.hdr.load() && s.hdr_mode.load() == BbSettings::HdrModeScene;
        std::printf("HDR 16-bit: the chain is %s (hdr %d hdr_mode %d)\n",
                 on ? "on" : "off", int(s.hdr.load()), int(s.hdr_mode.load()));
        return on;
    }();
    return enabled;
}

bool HdrChannel::Generic() {
    return Enabled();
}

namespace {
std::atomic<bool> native16_active{false};
std::atomic<bool> display_redirect{false};
std::atomic<u32> display_attachment{0}; // vk::Format of the game's display copy (pipeline key)
std::atomic<bool> trace_active{false};
std::mutex native16_mutex;
std::unordered_set<VAddr> native16_targets;
std::atomic<u32> native16_count{0};

// The startup-patched render size (BB_RENDER_RES, "Live resolution changes" off): the game
// allocates its targets at that size, aligned. {0, 0} in a live session.
std::pair<u32, u32> FixedRenderSize() {
    static const std::pair<u32, u32> size = [] {
        u32 w = 0, h = 0;
        const char* res = std::getenv("BB_RENDER_RES");
        if (!res || std::sscanf(res, "%ux%u", &w, &h) != 2) {
            return std::pair<u32, u32>{0, 0};
        }
        return std::pair<u32, u32>{w, h};
    }();
    return size;
}

// The chain's targets, 8-bit RGBA: 1920x1080 (the registers give the height padded to whole
// 8-row tiles), or with the startup patch the patched render size (padded, e.g. 3840x2176).
bool ChainSize(u32 width, u32 height) {
    // The scene's own target, whatever size the game renders it at: the launcher's output
    // presets (and its live resolution changes) move it freely, and a size fence against
    // BB_RENDER_RES silently kept the whole chain off when they disagreed. The shaders' hashes
    // identify the chain; the bounds only keep stray small or absurd targets out.
    return width >= 320 && width <= 8192 && height >= 180 && height <= 8192;
}

constexpr bool ChainFormat(vk::Format format) {
    return format == vk::Format::eR8G8B8A8Unorm || format == vk::Format::eR8G8B8A8Srgb ||
           format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
}

// The shaders after the composite that write its image or the copy the AA reads: PointSample
// Color1 (copy), EmissiveToGlowPS (copy for the glow, clamped by bbport), DLAA, and the glow's
// addition.
constexpr bool ChainShader(u64 ps_hash) {
    return ps_hash == 0xf7229091 || ps_hash == 0x57f3b8c2 || ps_hash == 0xda1faca9 ||
           ps_hash == 0x8c08444c;
}
} // namespace

std::vector<u32> HdrChannel::ChainReplacement(u64 pgm_hash, std::span<const u32> game) {
    if (!Native16()) {
        return {};
    }
    // The game shader's input fs_in_attr0 (none for the display copy): per vertex, the build
    // that reads it so; else the one that reads it as the GPU interpolates it (bb_inputs.glsl).
    const SpvInterface in = ReadInterface(game);
    const auto input = FindInput(in, 0);
    const bool per_vertex = input && input->per_vertex;
    const auto pick = [per_vertex](std::span<const u32> manual, std::span<const u32> hw) {
        return per_vertex ? std::vector<u32>(manual.begin(), manual.end())
                          : std::vector<u32>(hw.begin(), hw.end());
    };
    std::vector<u32> spv;
    std::vector<const char*> bindings;
    bool reads_input = true;
    if (pgm_hash == 0x57f3b8c2) {
        spv = pick(BB_GLOW_SOURCE_FRAG, BB_GLOW_SOURCE_HW_FRAG);
        bindings = {"fs_img0", "fs_samp0"};
    } else if (pgm_hash == 0xf7229091) {
        spv = pick(BB_COPY_FRAG, BB_COPY_HW_FRAG);
        bindings = {"fs_img0", "fs_samp0"};
    } else if (pgm_hash == ColorGradingHash) {
        spv = pick(BB_COLOR_GRADING_FRAG, BB_COLOR_GRADING_HW_FRAG);
        bindings = {"fs_img0", "fs_img1", "fs_samp0", "fs_samp1"};
    } else if (pgm_hash == 0x38d65b32) {
        spv.assign(std::begin(BB_DISPLAY_COPY_FRAG), std::end(BB_DISPLAY_COPY_FRAG));
        bindings = {"fs_img0", "fs_img1", "fs_samp0"};
        reads_input = false;
    } else {
        return {};
    }
    const SpvInterface ours = ReadInterface(spv);
    bool ok = true;
    for (const char* name : bindings) {
        const auto value = in.Binding(name);
        ok = ok && value && SetDecoration(spv, ours, name, SpvDecorationBinding, *value);
    }
    if (const auto color = in.Location("frag_color0")) {
        ok = ok && SetDecoration(spv, ours, "frag_color0", SpvDecorationLocation, *color);
    } else {
        ok = false;
    }
    if (reads_input) {
        // Ours is called fs_in_attr0 in both builds.
        ok = ok && input &&
             SetDecoration(spv, ours, "fs_in_attr0", SpvDecorationLocation, input->location);
    }
    if (!ok) {
        LOG_WARNING(Render_Vulkan, "HDR 16-bit: shader {:08x} has another interface, kept as it is",
                    pgm_hash);
        return {};
    }
    std::printf("HDR 16-bit: shader %08x replaced (%s%s)\n", u32(pgm_hash),
             pgm_hash == 0x57f3b8c2        ? "glow source clamped"
             : pgm_hash == 0xf7229091      ? "copy (chain probe)"
             : pgm_hash == ColorGradingHash ? "color table kept above white"
                                            : "display copy above white",
             !reads_input  ? ""
             : per_vertex  ? ", input per vertex"
                           : ", input interpolated by the GPU");
    return spv;
}

bool HdrChannel::Trace() {
    return trace_active.load(std::memory_order_relaxed);
}

void HdrChannel::OnDisplayCopy() {
    static const bool trace_env = std::getenv("BB_HDR_TRACE") != nullptr;
    if (trace_active.exchange(false)) {
        std::printf("HDR trace: end of the traced frame\n");
    }
    if (Native16() && trace_env) {
        trace_active = true;
        std::printf("HDR trace: frame start (16-bit chain active %d)\n",
                 Native16Active());
    }
}

bool HdrChannel::IsCompositeVariant(u64 pgm_hash) {
    const TonemapVariant* v = FindVariant(pgm_hash);
    return v != nullptr && v->standard;
}

bool HdrChannel::ChainShaderHash(u64 pgm_hash) {
    return ChainShader(pgm_hash);
}

bool HdrChannel::ChainImage(VAddr address, u32 width, u32 height, vk::Format format) {
    return Upgraded(address, width, height, format);
}

u32 HdrChannel::ChainProbe() {
    const char* probe = std::getenv("BB_HDR_PROBE");
    const int value = probe ? std::atoi(probe) : 0;
    return value >= 1 && value <= 5 ? u32(value) : 0u;
}

bool HdrChannel::Native16Fixed() {
    return Native16() && FixedRenderSize().first != 0;
}

bool HdrChannel::Native16() {
    return Enabled();
}

bool HdrChannel::Native16Active() {
    return Native16() && native16_active.load(std::memory_order_relaxed);
}

void HdrChannel::SetNative16Active(bool active) {
    if (Native16() && native16_active.exchange(active) != active) {
        std::printf("HDR 16-bit: %s\n", active ? "on (output-size images)"
                                                          : "off (no output-size images)");
    }
}

void HdrChannel::NoteTarget(u64 ps_hash, VAddr address, u32 width, u32 height,
                            vk::Format format) {
    if (!Native16()) {
        return;
    }
    const TonemapVariant* v = FindVariant(ps_hash);
    if (!(v && v->standard) && !ChainShader(ps_hash)) {
        // Diagnostic: anything drawn at the render size by a shader the chain does not know —
        // the composite's hash missing from the table, or the size/format gate keeping it out.
        // The first few tell which (and the format the scene target really has).
        if (ChainSize(width, height)) {
            static std::mutex told_mutex;
            static std::unordered_set<u64> told;
            std::scoped_lock lock{told_mutex};
            if (told.size() < 24 && told.insert(ps_hash).second) {
                std::printf("HDR 16-bit: ps %08x draws into a %ux%u %s target the chain does not take\n",
                            u32(ps_hash), width, height, vk::to_string(format).c_str());
            }
        }
        return;
    }
    if (!ChainSize(width, height) || !ChainFormat(format)) {
        static std::mutex logged_mutex;
        static std::unordered_set<u64> logged;
        std::scoped_lock lock{logged_mutex};
        if (logged.insert(ps_hash).second) {
            std::printf("HDR 16-bit: target %#llx of ps %08x left 8-bit (%ux%u %s)\n",
                        (unsigned long long)address, u32(ps_hash), width, height,
                        vk::to_string(format).c_str());
        }
        return;
    }
    std::scoped_lock lock{native16_mutex};
    if (native16_targets.insert(address).second) {
        native16_count.store(u32(native16_targets.size()), std::memory_order_release);
        std::printf("HDR 16-bit: target %#llx (from ps %08x) is RGBA16F\n", (unsigned long long)address,
                 ps_hash);
    }
}

void HdrChannel::NoteCopyTarget(VAddr address, u32 width, u32 height, vk::Format format) {
    if (!Native16() || !ChainSize(width, height) || !ChainFormat(format)) {
        return;
    }
    std::scoped_lock lock{native16_mutex};
    if (native16_targets.insert(address).second) {
        native16_count.store(u32(native16_targets.size()), std::memory_order_release);
        std::printf("HDR 16-bit: target %#llx (copy of the chain) is RGBA16F\n",
                 address);
    }
}

bool HdrChannel::Upgraded(VAddr address, u32 width, u32 height, vk::Format format) {
    if (!Native16Active() || !ChainSize(width, height) || !ChainFormat(format) ||
        native16_count.load(std::memory_order_acquire) == 0) {
        return false;
    }
    std::scoped_lock lock{native16_mutex};
    return native16_targets.contains(address);
}

void HdrChannel::SetDisplayRedirect(bool redirect) {
    display_redirect.store(redirect, std::memory_order_relaxed);
}

bool HdrChannel::DisplayUpgraded(VAddr address, vk::Format format) {
    (void)address;
    const bool upgraded =
        Native16Active() && display_redirect.load(std::memory_order_relaxed) &&
        (format == vk::Format::eR8G8B8A8Srgb || format == vk::Format::eR8G8B8A8Unorm ||
         format == vk::Format::eB8G8R8A8Srgb || format == vk::Format::eB8G8R8A8Unorm);
    if (upgraded) {
        display_attachment.store(static_cast<u32>(format), std::memory_order_relaxed);
    }
    return upgraded;
}

vk::Format HdrChannel::DisplayAttachmentFormat() {
    return static_cast<vk::Format>(display_attachment.load(std::memory_order_relaxed));
}

float HdrChannel::PeakNits() {
    // The peak in use: the display's (peak_auto) or bbport.ini's hdr_peak; live.
    return std::clamp(BbSettings::HdrPeakNits(BbSettings::Get()), 200.0f, 10000.0f);
}

float HdrChannel::Knee() {
    // Highlight range: the fraction of the game's white below which its own curve stands, above
    // which the extension runs to the peak (bbport.ini hdr_knee, the menu's slider; live).
    // BB_HDR_KNEE overrides for tests.
    static const float env = [] {
        const char* value = std::getenv("BB_HDR_KNEE");
        return value && *value ? EnvFloat("BB_HDR_KNEE", 0.6f, 0.3f, 0.95f) : 0.0f;
    }();
    if (env > 0.0f) {
        return env;
    }
    const float knee = BbSettings::Get().hdr_knee.load(std::memory_order_relaxed);
    return std::clamp(std::isfinite(knee) ? knee : BbSettings::HdrKneeDefault,
                      BbSettings::HdrKneeMin, BbSettings::HdrKneeMax);
}

bool HdrChannel::VanillaChain() {
    // The menu's HDR preset Off and the compare-on-SDR mode are not ported yet: the chain's HDR
    // changes always run in native+.
    return false;
}

u32 HdrChannel::Debug() {
    const char* env = std::getenv("BB_HDR_DEBUG");
    const int value = env ? std::atoi(env) : 0;
    return u32(std::clamp(value, 0, 2));
}

} // namespace Vulkan
