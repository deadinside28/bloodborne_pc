// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport shader translator: the byte layout of translated shaders' metadata in the pipeline
// cache (render/pipeline_store.h). Info is stored as its persistent prefix, the flattened user
// data and the SRT walker (its machine code); a specialization field by field.

#include "common/serdes.h"
#include "shader/frontend/fetch_shader.h"
#include "shader/info.h"
#include "shader/specialization.h"

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer writer{ar};
    writer.Write(this, sizeof(InfoPersistent));
    writer.Write(flattened_ud_buf);
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader reader{ar};
    reader.Read(this, sizeof(InfoPersistent));
    reader.Read(flattened_ud_buf);
    return srt_info.Deserialize(ar);
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer writer{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));
    writer.Write(size);
    writer.Write(vertex_offset_sgpr);
    writer.Write(instance_offset_sgpr);
    writer.Write(attributes);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader reader{ar};
    reader.Read(size);
    reader.Read(vertex_offset_sgpr);
    reader.Read(instance_offset_sgpr);
    reader.Read(attributes);
    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer writer{ar};
    const u64 code_size = walker_func_size;
    const u64 program_size = walker_program.size();
    writer.Write(code_size);
    writer.Write(flattened_bufsize_dw);
    writer.Write(program_size);
    if (code_size) {
        writer.Write(reinterpret_cast<void*>(walker_func), code_size);
    }
    if (program_size) {
        writer.Write(walker_program.data(), program_size * sizeof(u32));
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader reader{ar};
    u64 code_size{}, program_size{};
    reader.Read(code_size);
    reader.Read(flattened_bufsize_dw);
    reader.Read(program_size);
    walker_func = nullptr;
    walker_func_size = code_size;
    if (code_size) {
        // The size is checked before the code is registered (it becomes executable): a file cut
        // short must not have bytes past its end run as the walker.
        const auto code = ar.CurrPtr();
        ar.Advance(code_size);
        walker_func = RegisterWalkerCode(code, code_size); // nullptr on hosts other than x86-64
    }
    if (program_size > (1u << 20)) {
        return false; // no walker is that long: a damaged entry
    }
    walker_program.resize(program_size);
    if (program_size) {
        reader.Read(walker_program.data(), program_size * sizeof(u32));
    }
    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer writer{ar};
    writer.Write(start);
    writer.Write(runtime_info);
    writer.Write(bitset.to_string());
    if (fetch_shader_data) {
        writer.Write(sizeof(*fetch_shader_data));
        fetch_shader_data->Serialize(ar);
    } else {
        writer.Write(size_t{0});
    }
    writer.Write(vs_attribs);
    writer.Write(buffers);
    writer.Write(images);
    writer.Write(fmasks);
    writer.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader reader{ar};
    reader.Read(start);
    reader.Read(runtime_info);
    std::string bits{};
    reader.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);
    u64 fetch_data_size{};
    reader.Read(fetch_data_size);
    if (fetch_data_size) {
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }
    reader.Read(vs_attribs);
    reader.Read(buffers);
    reader.Read(images);
    reader.Read(fmasks);
    reader.Read(samplers);
    return true;
}

} // namespace Shader
