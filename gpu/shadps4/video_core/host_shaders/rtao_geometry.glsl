// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: ray-traced AO, the per-draw record of the geometry table (GpuGeometry in rtao_geometry.h)
// and the game's index reader, shared by rtao_expand.comp and rtao_probe.comp. Needs
// GL_EXT_buffer_reference and GL_EXT_shader_explicit_arithmetic_types_int64.

struct Geometry {
    uint store;           // positions base in the object-motion capture (vec4 index)
    uint vertices;        // vertices per instance in the capture
    uint instances;
    uint first_vertex;
    uint64_t index_address;
    uint index_count;     // per instance
    uint index16;         // bit 0: 16-bit indices; bit 1: the buffer starts mid-word
    int base_vertex;
    uint restart;         // 1: primitive restart on
    uint triangle_offset; // first triangle of this draw in the expand dispatch
    uint triangles;       // triangles of this draw (index_count / 3 * instances)
    uint out_base;        // first output triangle in the triangle buffer
    uint flags;           // 1: world space (a static-cache build), 2: probed
};

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Words { uint w[]; };

uint ReadIndex(Words words, uint index16, uint i) {
    if ((index16 & 1u) != 0u) {
        const uint j = i + ((index16 & 2u) != 0u ? 1u : 0u); // bit 1: the buffer starts mid-word
        const uint word = words.w[j >> 1];
        return (j & 1u) != 0u ? (word >> 16) : (word & 0xffffu);
    }
    return words.w[i];
}
