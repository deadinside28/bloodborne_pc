// SPDX-License-Identifier: GPL-2.0-or-later
// bbport shader translator: the shader resource table walker kept with a shader: generated code
// that copies the descriptors the shader reads through pointers in its user data into one flat
// buffer, and that buffer's size.
#pragma once

#include "common/types.h"

namespace Serialization {
struct Archive;
}

namespace Shader {

using PFN_SrtWalker = void PS4_SYSV_ABI (*)(const u32* /*user_data*/, u32* /*flat_dst*/);

/// Makes `size` bytes of walker code at `ptr` executable; the function to call.
PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size);

struct PersistentSrtInfo {
    /// A fetch shader's vertex buffer descriptors kept in place (instance step rates).
    struct SrtSharpReservation {
        u32 sgpr_base;
        u32 dword_offset;
        u32 num_dwords;
    };

    PFN_SrtWalker walker_func{};
    size_t walker_func_size{};
    u32 flattened_bufsize_dw = 16; ///< the user data registers, then the walked descriptors

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

} // namespace Shader
