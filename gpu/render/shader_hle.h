// SPDX-License-Identifier: GPL-2.0-or-later
// bbport render: compute shaders done by the renderer instead of being translated. Only the
// game's buffer copy shader (from its profile), when the game's memory is not in place.
#pragma once

#include <utility>
#include <vector>

namespace Gcn {
struct ComputeProgram;
union Regs;
} // namespace Gcn

namespace Shader {
struct Info;
}

namespace Vulkan {

class Rasterizer;

/// True if the dispatch was done here.
bool ExecuteShaderHLE(const Shader::Info& info, const Gcn::Regs& regs,
                      const Gcn::ComputeProgram& cs_program, Rasterizer& rasterizer);

/// The game's buffer copy shader (its profile's BufferCopyShader): the destination ranges its
/// control list names for `groups` work groups, sorted and merged where less than `gap` bytes
/// apart, into `out`. False (nothing added) for another shader or an unexpected shape. Its bound
/// destination spans tens of MiB while each dispatch writes a few KiB.
bool CopyShaderDestinations(const Shader::Info& info, u32 groups, u64 gap,
                            std::vector<std::pair<unsigned long long, unsigned long long>>& out);

/// BB_COPY_DEST_GAP: the merge gap for CopyShaderDestinations (default 256 bytes, the granule of the
/// queued-write table); BB_COPY_DEST_PRECISE=0: the whole bound destination counts as written.
bool CopyDestinationsPrecise();
unsigned long long CopyDestinationGap();

} // namespace Vulkan
