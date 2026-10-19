// SPDX-License-Identifier: GPL-2.0-or-later
// bbport render: Rasterizer, dispatches done without running the shader: compute shaders that
// clear metadata (CMASK/HTILE) or copy or clear an image through its memory are recognized by
// their resources and replaced by the tracked metadata state or a Vulkan copy or clear (the
// images live in their own Vulkan memory, not in the buffers the shader would write).

#include "render/raster_internal.h"

namespace Vulkan {

namespace {

/// The shader's own buffers: the page table the info collection pass appends to every shader
/// with guest buffers (BB_LAYER_MEMORY) does not count.
size_t ShaderBufferCount(const Shader::Info& info) {
    size_t count = info.buffers.size();
    if (count != 0 && !info.uses_dma &&
        info.buffers.back().buffer_type == Shader::BufferType::BdaPagetable) {
        --count;
    }
    return count;
}

/// 64 threads, two own buffers, no images: the copy and clear shaders' shape.
bool IsTwoBufferShader(const Gcn::ComputeProgram& cs, const Shader::Info& info) {
    return cs.num_thread_x.full == 64 && ShaderBufferCount(info) == 2 && info.images.empty();
}

} // namespace

bool Rasterizer::IsComputeMetaClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }
    // A shader writing metadata almost always clears it: the dispatch is skipped and the
    // tracked state updated (reading cleared metadata, as HTILE introspection would, needs the
    // full emulation anyway).
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    for (const auto& desc : info.buffers) {
        if (!desc.IsSpecial() && !desc.is_written &&
            texture_cache.IsMeta(desc.GetSharp(info).base_address)) {
            return false; // reads metadata: a copy
        }
    }
    // Encoding tiled metadata (HTILE from a depth image) computes tile addresses with an xor:
    // such a shader is not a clear.
    if (info.has_bitwise_xor) {
        return false;
    }
    for (const auto& desc : info.buffers) {
        if (!desc.IsSpecial() && desc.is_written &&
            texture_cache.ClearMeta(desc.GetSharp(info).base_address)) {
            return true; // all slices taken as cleared
        }
    }
    return false;
}

bool Rasterizer::IsComputeImageCopy(const Pipeline* pipeline, bool dry_run) {
    if (!pipeline->IsCompute()) {
        return false;
    }
    // Two formatted buffers of one size, one read and one written, a dword per thread: a copy
    // between the memory of two images of the same layout.
    const auto& cs = CsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (!IsTwoBufferShader(cs, info)) {
        return false;
    }
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (!desc0.is_formatted || !desc1.is_formatted || desc0.is_written == desc1.is_written) {
        return false;
    }
    const Gcn::Buffer buf0 = desc0.GetSharp(info);
    const Gcn::Buffer buf1 = desc1.GetSharp(info);
    if (buf0.GetSize() != buf1.GetSize() || cs.dim_x != buf0.GetSize() / 256) {
        return false;
    }
    const auto image0_id = texture_cache.FindImageFromRange(buf0.base_address, buf0.GetSize());
    if (!image0_id) {
        return false;
    }
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }
    auto& image0 = texture_cache.GetImage(image0_id);
    auto& image1 = texture_cache.GetImage(image1_id);
    // The buffers may run past the images (padding up to the next one: the light shafts' 960x540
    // targets are copied as 0x241800 bytes of 0x240000): those bytes are copied as a buffer.
    constexpr u64 MaxTail = 64_KB;
    const u64 image_size = image0.info.guest_size;
    if (image_size != image1.info.guest_size || image0.info.pitch != image1.info.pitch ||
        buf0.GetSize() < image_size || buf0.GetSize() - image_size > MaxTail ||
        image0.info.num_bits != image1.info.num_bits) {
        return false;
    }
    auto& src = desc0.is_written ? image1 : image0;
    auto& dst = desc0.is_written ? image0 : image1;
    // BB_IMAGE_COPY_LOG=1 (diagnostics): the copies recognized.
    static const bool copy_log = std::getenv("BB_IMAGE_COPY_LOG") != nullptr;
    if (copy_log) {
        static std::atomic<u32> printed{0};
        if (printed.fetch_add(1) < 40) {
            std::printf("Image copy: %#llx (%ux%u fmt %u flags %#x) -> %#llx (%ux%u fmt %u flags "
                        "%#x), %u bytes\n",
                        (unsigned long long)src.info.guest_address, src.info.size.width,
                        src.info.size.height, u32(src.info.pixel_format), u32(src.flags),
                        (unsigned long long)dst.info.guest_address, dst.info.size.width,
                        dst.info.size.height, u32(dst.info.pixel_format), u32(dst.flags),
                        u32(buf0.GetSize()));
        }
    }
    if (dry_run) {
        return false;
    }
    runtime.CopyColorAndDepth(&src, &dst);
    // Before: the shader ran (5 times a frame over the light shafts), writing 2.3 MiB into the
    // game's memory each time (in place: those blocks are dynamic), the target then uploaded
    // from there again: 3.4 ms of GPU time a frame with BB_GUEST_UNCACHED on an RX 7800 XT.
    if (const u64 tail = buf0.GetSize() - image_size) {
        const VAddr src_tail = src.info.guest_address + image_size;
        const VAddr dst_tail = dst.info.guest_address + image_size;
        const auto [src_buffer, src_offset] = buffer_cache.ObtainBuffer(src_tail, u32(tail), false);
        const auto [dst_buffer, dst_offset] = buffer_cache.ObtainBuffer(dst_tail, u32(tail), true);
        const vk::BufferCopy copy{src_offset, dst_offset, tail};
        runtime.CopyBuffer(src_buffer, dst_buffer, {&copy, 1});
        texture_cache.InvalidateMemoryFromGPU(dst_tail, tail);
    }
    return true;
}

/// BB_IMAGE_CLEAR_LOG=1: why a clear shader (cs 8b355b5a) went to the real shader, once per reason
/// and group count: the number of the failing check in IsComputeImageClear.
static bool RejectClear(int reason, u32 dim_x) {
    static const bool log = [] {
        const char* env = std::getenv("BB_IMAGE_CLEAR_LOG");
        return env && env[0] == '1';
    }();
    if (log) {
        static std::mutex mutex;
        static std::set<std::pair<int, u32>> told;
        std::scoped_lock lock{mutex};
        if (reason > 1 && told.emplace(reason, dim_x).second) {
            std::printf("Image clear HLE: rejected at check %d (%u groups)\n", reason, dim_x);
        }
    }
    return false;
}

bool Rasterizer::IsComputeImageClear(const Pipeline* pipeline) {
    int reason = 0;
    if (!pipeline->IsCompute()) {
        return RejectClear(++reason, 0);
    }
    // A raw vec4 read and a formatted buffer over one layer of a color image written, 128
    // elements per workgroup: a clear of that image to the vec4.
    const auto& cs = CsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (!IsTwoBufferShader(cs, info)) {
        return RejectClear(++reason, cs.dim_x);
    }
    const auto& value_desc = info.buffers[0];
    const auto& target_desc = info.buffers[1];
    if (value_desc.is_formatted || !target_desc.is_formatted || value_desc.is_written ||
        !target_desc.is_written) {
        return RejectClear(++reason, cs.dim_x);
    }
    const Gcn::Buffer value = value_desc.GetSharp(info);
    const Gcn::Buffer target = target_desc.GetSharp(info);
    const u32 bits = Gcn::NumBitsPerBlock(target.GetDataFmt());
    if (value.GetSize() != 16 || cs.dim_x * 128ULL * (bits / 8) != target.GetSize()) {
        return RejectClear(++reason, cs.dim_x);
    }
    const auto image_id =
        texture_cache.FindImageFromRange(target.base_address, target.GetSize(), false);
    if (!image_id) {
        return RejectClear(++reason, cs.dim_x);
    }
    auto& image = texture_cache.GetImage(image_id);
    if (image.info.guest_size != target.GetSize() || image.info.num_bits != bits ||
        image.info.props.is_depth) {
        return RejectClear(++reason, cs.dim_x);
    }
    const auto* color = reinterpret_cast<const float*>(value.base_address);
    const vk::ClearValue clear{
        .color = {.float32 = std::array<float, 4>{color[0], color[1], color[2], color[3]}},
    };
    const VideoCore::SubresourceRange range{.base = {.level = 0, .layer = 0},
                                            .extent = image.info.resources};
    runtime.ClearImage(&image, range, clear);
    return true;
}

} // namespace Vulkan
