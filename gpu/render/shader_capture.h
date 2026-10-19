// SPDX-License-Identifier: GPL-2.0-or-later
// bbport render: BB_SHADER_CAPTURE=<dir> records every shader compilation the game makes: its
// inputs (the program, user data, runtime info, profile, bindings, and the guest memory the
// compiler reads: fetch shaders, vertex buffer descriptors) and its outputs (SPIR-V, the shader
// info, bindings). A replay harness compiles the inputs again and compares the outputs: the
// shader compiler is changed against this corpus, the output kept byte for byte.
#pragma once

#include <span>

#include "common/types.h"

namespace Shader {
struct Info;
struct RuntimeInfo;
struct Profile;
namespace Backend {
struct Bindings;
}
} // namespace Shader

namespace BbShaderCapture {

/// Whether compilations are recorded (BB_SHADER_CAPTURE set).
bool Enabled();

/// Starts recording the guest memory the compiler reads on this thread.
void Begin();

/// Writes the record of a compilation (`in_info`: the info as it was before compiling).
void Write(const Shader::Info& in_info, const Shader::RuntimeInfo& runtime_info,
           const Shader::Profile& profile, const Shader::Backend::Bindings& in_bindings,
           std::span<const u32> code, size_t perm_idx, std::span<const u32> spv,
           const Shader::Info& out_info, const Shader::Backend::Bindings& out_bindings);

} // namespace BbShaderCapture
