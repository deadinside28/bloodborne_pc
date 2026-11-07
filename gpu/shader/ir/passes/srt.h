// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport shader translator: the shader resource table walker kept with a shader: generated code
// that copies the descriptors the shader reads through pointers in its user data into one flat
// buffer, and that buffer's size.
#pragma once

#include <vector>

#include "common/types.h"

namespace Serialization {
struct Archive;
}

namespace Shader {

using PFN_SrtWalker = void PS4_SYSV_ABI (*)(const u32* /*user_data*/, u32* /*flat_dst*/);

/// Makes `size` bytes of walker code at `ptr` executable; the function to call (x86-64 hosts;
/// elsewhere nullptr: the walker program runs instead).
PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size);

/// The walker as a program for any host (srt_program.cpp), words: an opcode, then its operands.
/// The current table starts as the user data; offset expressions are evaluated on a stack of u32
/// (postfix), exactly as the x86 walker computes them (32-bit wrap, shift counts mod 32).
namespace SrtOp {
enum : u32 {
    End,
    PushTableImm,  ///< disp: table := (u64 at table + sext(disp)) & 2^48-1 (the old one kept)
    PushTableExpr, ///< pops v: table := (u64 at table + u32(v << 2)) & 2^48-1
    PopTable,
    CopyImm,  ///< slot, disp: flat[slot] := u32 at table + sext(disp)
    CopyExpr, ///< slot; pops v: flat[slot] := u32 at table + u32(v << 2)
    Flat,     ///< index: push flat[index] (the user data registers, then slots copied)
    Imm,      ///< value: push value
    Add, Sub, Mul, And, Or, Xor, UMin, UMax, Shl, Shr, ///< pop b, pop a, push a op b
    Not,
    Bfe,         ///< pop count, offset, base: (base >> (offset & 31)) & ((1 << (count & 31)) - 1)
    BfeImmCount, ///< count; pop offset, base: as Bfe, no mask for count >= 32
};
}

/// Runs a walker program: as the x86 walker, a load from unmapped memory reads 0.
void RunSrtProgram(const u32* program, const u32* user_data, u32* flat_dst);

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
    std::vector<u32> walker_program; ///< the same walk for any host (SrtOp)

    bool HasWalker() const {
        return walker_func || !walker_program.empty();
    }
    /// Fills the flat buffer's walked part: the x86 walker, or the program (other hosts;
    /// BB_SRT_INTERP=1 on x86-64; =verify runs both and compares).
    void RunWalker(const u32* user_data, u32* flat_dst) const;

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

} // namespace Shader
