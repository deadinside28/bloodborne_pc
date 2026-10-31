// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport HDR transport (the Native+ port; the source port's vk_hdr_channel.h, re-plumbed onto
// this renderer):
// hdr_mode=native+ keeps the post-composite chain in RGBA16F and adds the composite's HDR delta
// to the game's own target right after its draw (shaders/hdr/bb_composite_hdr.frag, per variant
// of bb_tonemap_variants.h), so the frame carries light above the game's white to the final
// pass, which encodes it for the display (presenter.cpp RecordHdrPresent). The tone mapper is
// Vanilla+ (souperman9's RenoDX curve: the game's curve to mid grey, a straight line above it,
// bloom, CInfinityRollOff to the peak); PsychoV-30's own shadows and the gamut bridge's target
// selection are not ported (its code is carried in shaders/hdr/ because the composite's shader
// includes it, but no path selects it).

#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/types.h"
#include "vulkan/vk_common.h"

namespace Vulkan {

class Instance;

/// The 16-bit chain: the game's composite (and the passes that copy or grade its image) drawn
/// into RGBA16F targets, with the HDR delta added beside them.
class HdrChannel {
public:
    /// hdr=1 and hdr_mode=native+ in bbport.ini.
    static bool Enabled();
    /// Same as Enabled() here (the source port's legacy side-channel modes are not ported).
    /// Also says that every composite variant gets its delta pass built.
    static bool Generic();
    /// The 16-bit chain is in use: Enabled(), and it is the only mode of this port.
    static bool Native16();
    /// Mode 5 with the startup-patched render size (BB_RENDER_RES, "Live resolution changes"
    /// off): the game's targets are already at the render size and the scene has no proxies, so
    /// the chain's targets get same-size RGBA16F proxies of their own.
    static bool Native16Fixed();
    /// Native16 and the port's output-size images in use (set by the scene proxies, each frame).
    static bool Native16Active();
    static void SetNative16Active(bool active);
    /// GPU thread, at each draw: the color target of a draw with this pixel shader (guest
    /// address, size, format). Composite variants and the chain's shaders mark their
    /// 1920x1080 RGBA8 target as 16-bit from the next frame on.
    static void NoteTarget(u64 ps_hash, VAddr address, u32 width, u32 height, vk::Format format);
    /// GPU thread: the destination of a copy of a chain target (the game copies its finished
    /// image for its color grading with a compute copy, not a draw) joins the chain from the
    /// next frame on, so the copy is done between RGBA16F proxies.
    static void NoteCopyTarget(VAddr address, u32 width, u32 height, vk::Format format);
    /// Whether this guest color target is drawn as RGBA16F (image formats, pipeline formats).
    static bool Upgraded(VAddr address, u32 width, u32 height, vk::Format format);
    /// Whether a texture cache image is one of the chain's 16-bit targets.
    static bool ChainImage(VAddr address, u32 width, u32 height, vk::Format format);
    /// The display buffers (the game's last copy) when the port draws them at output size.
    static void SetDisplayRedirect(bool redirect);
    static bool DisplayUpgraded(VAddr address, vk::Format format);
    /// The format the game's display copy writes through (its 8-bit attachment): sRGB when the
    /// copy's output is linear (encoded by the attachment), UNORM when it is already encoded.
    static vk::Format DisplayAttachmentFormat();
    static constexpr vk::Format Native16Format = vk::Format::eR16G16B16A16Sfloat;
    /// The game's ColorGrading pass (a color table), replaced in the chain; its strength goes
    /// to the shader in bb_hdr.w (shaders/hdr/bb_color_grading.frag).
    static constexpr u64 ColorGradingHash = 0xed4c7668;
    /// The game's second glow, added after its tone mapping (a blur of the 960x540 glow
    /// targets, additive); in the chain weighted by the Bloom slider (blend constant).
    static constexpr u64 GlowAddHash = 0x8c08444c;
    /// Is the shader one of the composite's variants (its tonemap is recognized)? And one of the
    /// chain's own shaders (the copy, the glow's source, the colour table, the display copy)?
    /// For the draw-time diagnostics only.
    static bool IsCompositeVariant(u64 pgm_hash);
    static bool ChainShaderHash(u64 pgm_hash);
    /// Diagnostics: display buffers whose memory holds float bits (the pipeline upgraded them),
    /// and a log line per unique native access to such a buffer (who touches it, and as what).
    static void NoteDisplayFloat(VAddr address);
    static void LogFloatAccess(VAddr address, const char* what);
    /// bbport's version of a game shader of the chain (glow source, copy, color table, display
    /// copy) with the game shader's bindings and locations, or empty to keep the game's.
    static std::vector<u32> ChainReplacement(u64 pgm_hash, std::span<const u32> game);
    /// Called with the game's SPIR-V of every fragment shader compiled or loaded. For a
    /// composite variant, builds its delta shader (shaders/hdr/bb_composite_hdr.frag with that
    /// variant's interface: descriptor bindings, input locations, constant buffer).
    static void NoteGameShader(u64 pgm_hash, std::span<const u32> spv);
    /// The delta shader of this composite variant (null if none).
    static vk::ShaderModule CompositeModule(const Instance& instance, u64 pgm_hash);
    /// Peak luminance in nits (HdrPeakNits, clamped to 200..10000 for the shader's scale).
    static float PeakNits();
    /// Fraction of the game's white below which the game's curve is kept (BB_HDR_KNEE, 0.6).
    static float Knee();
    /// The tone mapper in use: 4, Vanilla+ (souperman9), this port's only one.
    static constexpr u32 Tonemapper() { return 4u; }
    /// True where the chain draws the game's own picture (the menu's HDR off, or the compare on
    /// SDR, once those exist): the chain's HDR changes are left out.
    static bool VanillaChain();
    /// BB_HDR_DEBUG: 0 off, 1 the gain as a heat map, 2 the pixels taken as UI in red.
    static u32 Debug();
    /// The chain probe (BB_HDR_PROBE, diagnostics): 1..5, else 0. A pass draws the value it
    /// runs as, to see where the chain breaks.
    static u32 ChainProbe();
    /// One traced frame (BB_HDR_TRACE): every access to the chain's targets logged, from a
    /// display copy to the next (OnDisplayCopy bounds it).
    static bool Trace();
    /// GPU thread, at each display copy: starts or ends the traced frame.
    static void OnDisplayCopy();
};

} // namespace Vulkan
