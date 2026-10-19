// SPDX-License-Identifier: GPL-2.0-or-later
// The texture cache: uploads of images the guest (or the GPU, through buffers) changed.
#include <cstring>
#include "textures/texture_cache.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <utility>

#include <xxhash.h>

#include "bbport_button_prompts.h"
#include "bbport_toggles.h"
#include "buffers/buffer_cache.h"
#include "render/runtime.h"
#include "render/scheduler.h"

namespace VideoCore {

namespace {

bool EnvFlag(const char* name) {
    const char* env = std::getenv(name);
    return env && env[0] == '1';
}

/// BB_PROMPT_TRACE=1: the small images the game uploads (which are its key guide icons).
void TracePromptCandidate(const Image& image) {
    static const bool trace = EnvFlag("BB_PROMPT_TRACE");
    if (!trace) {
        return;
    }
    // Any block-compressed image: blocks of the cross icon (KG_Cancel) in any layout.
    if (image.info.props.is_block) {
        if (const u8* cancel = BbPrompts::IconBytes(2)) {
            const auto* bytes = std::bit_cast<const u8*>(image.info.guest_address);
            u32 hits = 0, first = 0;
            for (u32 offset = 0; offset + 16 <= image.info.guest_size; offset += 16) {
                for (u32 b : {27u, 28u, 35u, 36u}) {
                    if (std::memcmp(bytes + offset, cancel + b * 16, 16) == 0 && !hits++) {
                        first = offset;
                    }
                }
            }
            if (hits) {
                std::printf("Prompt trace: %u cross blocks in uploaded %#llx %ux%u levels %u fmt %s "
                            "tile %u from %#x\n",
                            hits, (unsigned long long)image.info.guest_address,
                            image.info.size.width, image.info.size.height,
                            image.info.resources.levels,
                            vk::to_string(image.info.pixel_format).c_str(),
                            u32(image.info.tile_mode), first);
            }
        }
    }
    // Larger block-compressed images: a key guide icon (one 1024-byte micro tile) copied in?
    if (image.info.props.is_block && image.info.size.width * image.info.size.height > 32 * 32) {
        const auto* bytes = std::bit_cast<const u8*>(image.info.guest_address);
        for (u32 offset = 0; offset + 1024 <= image.info.guest_size; offset += 1024) {
            if (const u32 icon = BbPrompts::IconOf(bytes + offset, 1024)) {
                std::printf("Prompt trace: icon %u inside image %#llx %ux%u fmt %s at %#x (tile %u)\n",
                            icon, (unsigned long long)image.info.guest_address,
                            image.info.size.width, image.info.size.height,
                            vk::to_string(image.info.pixel_format).c_str(), offset,
                            u32(image.info.tile_mode));
            }
        }
        return;
    }
    if (image.info.size.width != 32 || image.info.size.height != 32) {
        return;
    }
    static std::atomic<u32> traced{0};
    if (traced.fetch_add(1, std::memory_order_relaxed) >= 2000) {
        return;
    }
    const auto* bytes = std::bit_cast<const u8*>(image.info.guest_address);
    std::printf("Prompt trace: image %#llx %ux%u levels %u layers %u fmt %s block %d size %#x "
                "tile %u hash %016llx: icon %u\n",
                (unsigned long long)image.info.guest_address, image.info.size.width,
                image.info.size.height, image.info.resources.levels, image.info.resources.layers,
                vk::to_string(image.info.pixel_format).c_str(), int(image.info.props.is_block),
                image.info.guest_size, u32(image.info.tile_mode),
                (unsigned long long)XXH3_64bits(bytes, std::min<u32>(image.info.guest_size, 1024)),
                u32(image.prompt_icon));
}

/// BB_IMAGE_SYNC_LOG: the images refreshed most per 5 s (any reason), with their flags.
void NoteRefresh(const Image& image) {
    static const bool log = std::getenv("BB_IMAGE_SYNC_LOG") != nullptr;
    if (!log) {
        return;
    }
    // Render-target-sized images: every refresh, with the GPU frame (flicker hunting).
    if (image.info.size.width >= 640 && image.info.size.height >= 360) {
        static std::mutex big_mutex;
        static std::unordered_map<VAddr, u64> last_frame;
        static u32 big = 0;
        const u64 frame = BbStats::gpu_frames.load(std::memory_order_relaxed);
        std::scoped_lock big_lock{big_mutex};
        const u64 last = std::exchange(last_frame[image.info.guest_address], frame);
        if (last + 1 < frame && big++ < 3000) {
            std::printf("Image sync: big refresh %#llx %ux%u fmt %u tile %u flags %#x frame %llu\n",
                        (unsigned long long)image.info.guest_address, image.info.size.width,
                        image.info.size.height, u32(image.info.pixel_format),
                        u32(image.info.tile_mode), u32(image.flags),
                        (unsigned long long)BbStats::gpu_frames.load(std::memory_order_relaxed));
        }
    }
    if (True(image.flags & ImageFlagBits::GpuDirty)) {
        static std::atomic<u32> printed{0};
        if (printed.fetch_add(1) < 100) {
            std::printf("Image sync: refresh of %#llx (%ux%u) from memory the GPU wrote, flags "
                        "%#x\n",
                        (unsigned long long)image.info.guest_address, image.info.size.width,
                        image.info.size.height, u32(image.flags));
        }
    }
    struct Refreshes {
        u64 count = 0, bytes = 0;
        u32 width = 0, height = 0, format = 0, tiling = 0, flags = 0;
    };
    static std::mutex mutex;
    static std::unordered_map<VAddr, Refreshes> refreshes;
    static auto window_start = std::chrono::steady_clock::now();
    std::scoped_lock lk{mutex};
    auto& r = refreshes[image.info.guest_address];
    r = {r.count + 1, r.bytes + image.info.guest_size, image.info.size.width,
         image.info.size.height, u32(image.info.pixel_format), u32(image.info.tile_mode),
         r.flags | u32(image.flags)};
    const auto now = std::chrono::steady_clock::now();
    if (now - window_start < std::chrono::seconds(5)) {
        return;
    }
    std::vector<std::pair<VAddr, Refreshes>> top(refreshes.begin(), refreshes.end());
    std::ranges::sort(top, std::greater{}, [](const auto& e) { return e.second.bytes; });
    std::string text;
    for (size_t i = 0; i < std::min<size_t>(top.size(), 5); ++i) {
        const auto& [address, e] = top[i];
        text += fmt::format(" {:#x} {}x{} fmt {} tile {}: {}x {:.0f} MB flags {:#x};", address,
                            e.width, e.height, e.format, e.tiling, e.count, e.bytes / 1e6, e.flags);
    }
    std::printf("Image refreshes (5 s, %zu images):%s\n", refreshes.size(), text.c_str());
    refreshes.clear();
    window_start = now;
}

} // namespace

void TextureCache::RefreshImage(Image& image) {
    WatchImage(image, "refresh entered");
    if (False(image.flags & ImageFlagBits::Dirty) || image.info.num_samples > 1) {
        return;
    }
    BbStats::Timer timer{BbStats::t_refresh};
    const ImageInfo& info = image.info;

    // Maybe written (a page under it was, not it): compared by its first 8x8 texels.
    if (True(image.flags & ImageFlagBits::MaybeCpuDirty) &&
        False(image.flags & ImageFlagBits::CpuDirty)) {
        const u32 w = std::min(info.size.width, 8u);
        const u32 h = std::min(info.size.height, 8u);
        const u32 bytes = (info.props.is_block ? (w + 3) / 4 : w) *
                          (info.props.is_block ? (h + 3) / 4 : h) * (info.num_bits / 8);
        const u64 hash = XXH3_64bits(std::bit_cast<const u8*>(info.guest_address), bytes);
        if (image.hash == hash) {
            image.flags &= ~ImageFlagBits::MaybeCpuDirty;
            return;
        }
        image.hash = hash;
    }

    const u32 layers = info.resources.layers;
    const u32 levels = info.resources.levels;
    // The game's key guide icons (32x32, one level, known by their bytes): the port may sample
    // its own for the device in use.
    if (info.size.width == 32 && info.size.height == 32 && levels == 1 && layers == 1 &&
        info.props.is_block && info.guest_size >= 1024) {
        image.prompt_icon =
            u8(BbPrompts::IconOf(std::bit_cast<const u8*>(info.guest_address), 1024));
    }
    // The HUD's atlases with key guide icons packed in: the port may sample a copy with its own.
    if (levels == 1 && layers == 1 && info.props.is_block) {
        static std::atomic<u32> uploads{0};
        image.prompt_atlas = u8(BbPrompts::AtlasOf(std::bit_cast<const u8*>(info.guest_address),
                                                   info.guest_size, info.size.width,
                                                   info.size.height));
        image.prompt_upload = image.prompt_atlas ? ++uploads : 0;
    }
    TracePromptCandidate(image);
    NoteRefresh(image);
    BbStats::image_upload_bytes.fetch_add(info.guest_size, std::memory_order_relaxed);

    // A GPU-written image is reuploaded only where the guest's copy changed (by level hashes):
    // not over what the GPU rendered with a stale copy.
    // Every upload of such an image keeps the hashes, the first one too (a new image is
    // GpuDirty): else the first refresh after the GPU wrote it (a CPU write elsewhere in its
    // page) found no hash and uploaded unchanged memory over the GPU's data. The character
    // creator's exposure (1x1, written by a compute shader) went to 0: no character (#178).
    const bool gpu_modified = True(image.flags & ImageFlagBits::GpuModified);
    const bool gpu_written = gpu_modified && False(image.flags & ImageFlagBits::GpuDirty);
    boost::container::small_vector<vk::BufferImageCopy, 14> copies;
    for (u32 level = 0; level < levels; ++level) {
        const auto& layout = info.mips_layout[level];
        if (gpu_modified) {
            const u64 hash = XXH3_64bits(
                std::bit_cast<const u8*>(info.guest_address) + layout.offset, layout.size);
            if (gpu_written && image.mip_hashes[level] == hash) {
                continue;
            }
            image.mip_hashes[level] = hash;
        }
        const u32 width = std::max(info.size.width >> level, 1u);
        const u32 height = std::max(info.size.height >> level, 1u);
        const u32 depth = info.props.is_volume ? std::max(info.size.depth >> level, 1u) : 1u;
        copies.push_back({
            .bufferOffset = layout.offset,
            .bufferRowLength = layout.pitch,
            .bufferImageHeight = layout.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = level,
                .baseArrayLayer = 0,
                .layerCount = layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {layout.pitch ? std::min(layout.pitch, width) : width,
                            layout.height ? std::min(layout.height, height) : height, depth},
        });
    }
    WatchImage(image, copies.empty() ? "refresh: nothing to copy" : "refresh: uploading");
    if (copies.empty()) {
        image.flags &= ~ImageFlagBits::Dirty;
        return;
    }

    scheduler.EndRendering();
    const auto [source, source_offset] =
        buffer_cache.ObtainBufferForImage(info.guest_address, info.guest_size);
    const auto [linear, linear_offset] = detiler.DetileImage(source, source_offset, info);
    for (auto& copy : copies) {
        copy.bufferOffset += linear_offset;
    }
    runtime.UploadImage(&image, linear, copies);
}

} // namespace VideoCore
