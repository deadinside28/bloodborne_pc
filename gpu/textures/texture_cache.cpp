// SPDX-License-Identifier: GPL-2.0-or-later
// The texture cache: construction, lookup, overlap resolution, bindings and samplers.
#include "textures/texture_cache.h"

#include <xxhash.h>

#include "common/assert.h"
#include "common/hash.h"
#include "core/emulator_settings.h"
#include "textures/format_class.h"
#include "buffers/buffer_cache.h"
#include "vulkan/instance.h"
#include "render/runtime.h"
#include "render/scheduler.h"

namespace VideoCore {

namespace {

/// Submissions an image must have been unused before an overlap may evict it.
constexpr u64 TicksBeforeRemoval = 32;

/// The slot of a FindImage result.
u64 FoundSlot(const ImageInfo& info, TextureCache::BindingType type, bool exact_fmt) {
    u64 key = info.guest_address ^ u64(info.guest_size) << 17 ^ u64(info.pixel_format) << 40 ^
              u64(info.size.width) << 24 ^ info.size.height ^ u64(type) << 58 ^ u64(exact_fmt) << 63;
    key = (key ^ key >> 33) * 0xFF51AFD7ED558CCDull;
    key = (key ^ key >> 33) * 0xC4CEB9FE1A85EC53ull;
    return key ^ key >> 33;
}

} // namespace

TextureCache::TextureCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                           Vulkan::Runtime& runtime_, BbCp::CommandProcessor* command_processor_,
                           BufferCache& buffer_cache_, BbMem::PageWatch& page_watch_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      command_processor{command_processor_}, buffer_cache{buffer_cache_},
      page_watch{page_watch_}, detiler{instance, scheduler, runtime, buffer_cache.GetStreamBuffer()},
      readback_linear_images{EmulatorSettings.IsReadbackLinearImagesEnabled()} {
    const u32 max_samplers = instance.GetMaxSamplerAllocationCount();
    trigger_gc_samplers = max_samplers * 3 / 4;
    pressure_gc_samplers = max_samplers * 7 / 8;
    critical_gc_samplers = max_samplers * 15 / 16;
    UpdateMemoryBudget();
}

TextureCache::~TextureCache() = default;

u64 TextureCache::CurrentTick() const {
    return scheduler.CurrentTick();
}

std::string TextureCache::DescribeImagesIn(VAddr addr, u64 size) {
    std::unique_lock lock{mutex, std::try_to_lock}; // the caller may hold it already
    if (!lock.owns_lock()) {
        return " [images: not looked up]";
    }
    std::string out;
    int count = 0;
    ForEachImageInRegion(addr, size, [&](ImageId, Image& image) {
        if (count++ < 3) {
            out += fmt::format(" [image {:#x}+{:#x} {}x{} fmt {} tile {}{}{}]",
                               image.info.guest_address, image.info.guest_size,
                               image.info.size.width, image.info.size.height,
                               u32(image.info.pixel_format), u32(image.info.tile_mode),
                               image.usage.render_target ? " RT" : "",
                               True(image.flags & ImageFlagBits::GpuModified) ? " gpu" : "");
        }
    });
    if (count > 3) {
        out += fmt::format(" +{} more", count - 3);
    }
    return out;
}

ImageId TextureCache::InsertImage(const ImageInfo& info) {
    const ImageId id = slot_images.insert(instance, runtime, slot_image_views, info);
    RegisterImage(id);
    return id;
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_fmt) {
    const ImageInfo& info = desc.info;
    ASSERT(info.guest_address != 0);
    std::scoped_lock lock{mutex};

    // A remembered result: an exact match stays valid while that image is registered with the
    // same description, a resolved overlap (a view into another image) until any registration
    // changes.
    FoundImage& found = found_images[FoundSlot(info, desc.type, exact_fmt) % found_images.size()];
    const auto still_exact = [&] {
        if (found.view_mip >= 0 || found.view_slice >= 0 || !found.image_id) {
            return false;
        }
        const Image& image = slot_images[found.image_id];
        return True(image.flags & ImageFlagBits::Registered) &&
               image.info.guest_address == info.guest_address &&
               image.info.guest_size == info.guest_size && image.info.size == info.size &&
               image.info.pixel_format == info.pixel_format;
    };
    if ((found.generation == registry_generation.load(std::memory_order_relaxed) ||
         still_exact()) &&
        found.address == info.guest_address && found.size == info.guest_size &&
        found.extent == info.size && found.format == info.pixel_format &&
        found.type == info.type && found.exact_fmt == exact_fmt && found.binding == desc.type &&
        found.levels == info.resources.levels && found.layers == info.resources.layers &&
        !BbToggle::Disabled(BbToggle::FindImageCache)) {
        Image& image = slot_images[found.image_id];
        image.tick_accessed_last = CurrentTick();
        TouchImage(image);
        if (found.view_mip > 0) {
            desc.view_info.range.base.level = found.view_mip;
        }
        if (found.view_slice > 0) {
            desc.view_info.range.base.layer = found.view_slice;
        }
        return found.image_id;
    }

    ImageIds overlapping;
    ForEachImageInRegion(info.guest_address, info.guest_size,
                         [&](ImageId id, Image&) { overlapping.push_back(id); });

    // An exact match: same place, size and extent, a compatible format and the type (but for
    // 1x1x1 images); the last one wins.
    ImageId image_id{};
    for (const ImageId id : overlapping) {
        const ImageInfo& cached = slot_images[id].info;
        if (cached.guest_address == info.guest_address && cached.guest_size == info.guest_size &&
            cached.size == info.size &&
            IsVulkanFormatCompatible(cached.pixel_format, info.pixel_format) &&
            (cached.type == info.type || info.size == Extent3D{1, 1, 1}) &&
            (!exact_fmt || cached.pixel_format == info.pixel_format)) {
            image_id = id;
        }
    }

    // Else resolve the overlaps one by one, carrying the merged result.
    int view_mip = -1;
    int view_slice = -1;
    if (!image_id) {
        for (const ImageId id : overlapping) {
            view_mip = -1;
            view_slice = -1;
            const ImageInfo& merged = image_id ? slot_images[image_id].info : info;
            const auto [resolved, mip, slice] = ResolveOverlap(merged, desc.type, id, image_id);
            if (resolved) {
                image_id = resolved;
                view_mip = mip;
                view_slice = slice;
            }
        }
    }
    if (image_id) {
        const Image& resolved = slot_images[image_id];
        if (exact_fmt && info.pixel_format != resolved.info.pixel_format) {
            image_id = {}; // the very format is needed
        } else if (resolved.info.resources < info.resources) {
            FreeImage(image_id); // picked up wrongly
            image_id = {};
            LOG_WARNING(Render_Vulkan, "Image overlap resolve failed");
        }
    }
    if (!image_id) {
        image_id = InsertImage(info);
    }

    Image& image = slot_images[image_id];
    image.tick_accessed_last = CurrentTick();
    TouchImage(image);
    // A subresource of a larger image: where the view starts.
    if (view_mip > 0) {
        desc.view_info.range.base.level = view_mip;
    }
    if (view_slice > 0) {
        desc.view_info.range.base.layer = view_slice;
    }
    found = FoundImage{
        .address = info.guest_address,
        .size = info.guest_size,
        .extent = info.size,
        .format = info.pixel_format,
        .type = info.type,
        .exact_fmt = exact_fmt,
        .binding = desc.type,
        .levels = info.resources.levels,
        .layers = info.resources.layers,
        .generation = registry_generation.load(std::memory_order_relaxed),
        .image_id = image_id,
        .view_mip = view_mip,
        .view_slice = view_slice,
    };
    return image_id;
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested, BindingType binding,
                                          ImageId cache_id) {
    Image& cached = slot_images[cache_id];
    if (!cached.info.props.is_depth && !requested.props.is_depth) {
        return {};
    }
    // Recreated with the other aspect or more subresources; else the view handles it.
    bool recreate = cached.info.resources < requested.resources;
    switch (binding) {
    case BindingType::Texture:
        // A depth texture where the cache holds a color image of the same bits.
        recreate |= requested.props.is_depth && !cached.info.props.is_depth;
        break;
    case BindingType::Storage:
        recreate |= cached.info.props.is_depth;
        break;
    case BindingType::RenderTarget:
        ASSERT(!requested.props.is_depth);
        recreate |= cached.info.props.is_depth;
        break;
    case BindingType::DepthTarget:
        // A color image becomes a depth image; a depth image needs the same aspects and bits.
        recreate |= !cached.info.props.is_depth;
        recreate |= cached.info.props.is_depth &&
                    (requested.props.has_stencil != cached.info.props.has_stencil ||
                     requested.num_bits != cached.info.num_bits);
        break;
    default:
        break;
    }
    if (!recreate) {
        return cache_id;
    }

    ImageInfo info = requested;
    info.resources = std::max(requested.resources, cached.info.resources);
    info.UpdateSize();
    const ImageId new_id = InsertImage(info);
    Image& image = slot_images[new_id];
    image.usage = cached.usage;
    image.flags &= ~ImageFlagBits::Dirty;
    // A depth buffer made from another image is not cleared on first use.
    image.info.meta_info.htile_clear_mask = 0;
    runtime.CopyColorAndDepth(&cached, &image);
    FreeImage(cache_id);
    return new_id;
}

std::tuple<ImageId, int, int> TextureCache::ResolveOverlap(const ImageInfo& info,
                                                           BindingType binding, ImageId cache_id,
                                                           ImageId merged_id) {
    Image& cached = slot_images[cache_id];
    const ImageInfo& old = cached.info;
    const bool stale = CurrentTick() - cached.tick_accessed_last > TicksBeforeRemoval;

    if (info.guest_address == old.guest_address) {
        // Different block layouts at one address: most likely memory reused from a pool.
        if (info.BlockDim() != old.BlockDim() ||
            info.num_bits * info.num_samples != old.num_bits * old.num_samples) {
            if (stale) {
                FreeImage(cache_id);
            }
            return {merged_id, -1, -1};
        }
        if (const ImageId depth_id = ResolveDepthOverlap(info, binding, cache_id)) {
            return {depth_id, -1, -1};
        }
        // A compressed view of an uncompressed image of the same block size, or either of them
        // 3D at the same size: a new image with the old contents.
        if ((info.props.is_block && !old.props.is_block) ||
            (info.guest_size == old.guest_size &&
             (info.type == Gcn::ImageType::Color3D || old.type == Gcn::ImageType::Color3D))) {
            return {ExpandImage(info, cache_id), -1, -1};
        }
        const bool only_pow2_padding =
            info.props.is_pow2 != old.props.is_pow2 && info.tile_mode == old.tile_mode &&
            info.size == old.size && info.pitch == old.pitch && info.resources.levels == 1 &&
            old.resources.levels == 1 && info.resources.layers == 1 && old.resources.layers == 1;
        // Another format, or not larger: a view of the image.
        if (info.pixel_format != old.pixel_format || info.guest_size <= old.guest_size ||
            only_pow2_padding) {
            const ImageId result = merged_id ? merged_id : cache_id;
            const bool fits =
                IsVulkanFormatCompatible(slot_images[result].info.pixel_format, info.pixel_format);
            return {fits ? result : ImageId{}, -1, -1};
        }
        // More levels or layers: expanded.
        if (info.type == old.type && info.resources > old.resources) {
            return {ExpandImage(info, cache_id), -1, -1};
        }
        // Larger only by its tiling: the address reused for another image.
        if (info.tile_mode != old.tile_mode) {
            if (stale) {
                FreeImage(cache_id);
            }
            return {merged_id, -1, -1};
        }
        LOG_ERROR(Render_Vulkan,
                  "Unresolvable image overlap at {:#x}: cached {}x{}x{} {} type {} pitch {} "
                  "L{} x{} S{} tile {} bits {} block {} size {:#x} pow2 {} alt {} (unused {} "
                  "ticks); requested {}x{}x{} {} type {} pitch {} L{} x{} S{} tile {} bits {} "
                  "block {} size {:#x} pow2 {} alt {}; binding {}, merged {}",
                  info.guest_address, old.size.width, old.size.height, old.size.depth,
                  vk::to_string(old.pixel_format), u64(old.type), old.pitch, old.resources.levels,
                  old.resources.layers, old.num_samples, u32(old.tile_mode), old.num_bits,
                  bool(old.props.is_block), old.guest_size, bool(old.props.is_pow2),
                  old.alt_tile, CurrentTick() - cached.tick_accessed_last, info.size.width,
                  info.size.height, info.size.depth, vk::to_string(info.pixel_format),
                  u64(info.type), info.pitch, info.resources.levels, info.resources.layers,
                  info.num_samples, u32(info.tile_mode), info.num_bits, bool(info.props.is_block),
                  info.guest_size, bool(info.props.is_pow2), info.alt_tile, u32(binding),
                  merged_id.index);
        UNREACHABLE_MSG("unresolvable image overlap at the same address");
    }

    if (info.guest_address > old.guest_address) {
        // The requested image may be a subresource of the cached one.
        if (const s32 mip = info.MipOf(old); mip >= 0) {
            if (const s32 slice = info.SliceOf(old, mip); slice >= 0) {
                return {cache_id, mip, slice};
            }
        }
        // A chance overlap.
        if (stale) {
            FreeImage(cache_id);
        }
        return {{}, -1, -1};
    }

    // The cached image may be a subresource of the requested one.
    if (const s32 mip = old.MipOf(info); mip >= 0) {
        if (const s32 slice = old.SliceOf(info, mip); slice >= 0) {
            if (cached.binding.is_target) {
                // A part of it bound as a render target: rebound to the larger image.
                cached.binding.needs_rebind = 1;
                if (merged_id) {
                    GetImage(merged_id).binding.is_target = 1;
                }
                FreeImage(cache_id);
                return {merged_id, -1, -1};
            }
            // Copied into the larger image once there is one.
            if (merged_id) {
                runtime.CopyMip(&cached, &slot_images[merged_id], mip, slice);
                FreeImage(cache_id);
            }
        }
    }
    return {merged_id, -1, -1};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId image_id) {
    const ImageId new_id = InsertImage(info);
    Image& source = slot_images[image_id];
    Image& image = slot_images[new_id];
    RefreshImage(image);
    runtime.CopyImage(&source, &image);
    if (source.binding.is_bound || source.binding.is_target) {
        source.binding.needs_rebind = 1;
    }
    FreeImage(image_id);
    TrackImage(new_id);
    return new_id;
}

ImageId TextureCache::FindImageFromRange(VAddr address, size_t size, bool ensure_valid) {
    ImageIds candidates;
    ForEachImageInRegion(address, size, [&](ImageId id, Image& image) {
        if (image.info.guest_address == address && (!ensure_valid || image.SafeToDownload())) {
            candidates.push_back(id);
        }
    });
    // One candidate is taken as is (its size may differ from the buffer's a little).
    if (candidates.size() == 1) {
        return candidates.back();
    }
    for (const ImageId id : candidates) {
        if (slot_images[id].info.guest_size == size) {
            return id;
        }
    }
    if (!candidates.empty()) {
        LOG_WARNING(Render_Vulkan, "No image of exactly {:#x}+{:#x}", address, size);
    }
    return {};
}

void TextureCache::QueueDownload(ImageId image_id) {
    std::scoped_lock lk{download_mutex};
    download_images.emplace(image_id);
    downloads_queued.store(true, std::memory_order_release);
}

ImageView& TextureCache::FindTexture(ImageId image_id, const ImageDesc& desc, ViewMemo* memo,
                                     bool refresh) {
    Image& image = slot_images[image_id];
    WatchImage(image, refresh ? "texture (refresh)" : "texture (no refresh)");
    if (desc.type == BindingType::Storage) {
        image.flags |= ImageFlagBits::GpuModified;
        image.gpu_write_tick = CurrentTick();
        if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8) &&
            image.info.guest_address != 0) {
            QueueDownload(image_id);
        }
    }
    if (refresh) {
        UpdateImage(image_id);
    }
    if (!memo || BbToggle::Disabled(BbToggle::TextureViewMemo)) {
        return image.FindView(desc.view_info);
    }
    if (memo->image_id == image_id && memo->backing == image.backing && memo->view_id) {
        return slot_image_views[memo->view_id];
    }
    ImageView& view = image.FindView(desc.view_info);
    const auto& ids = image.backing->image_view_ids;
    const u32 last = image.backing->last_view;
    memo->image_id = image_id;
    memo->backing = image.backing;
    memo->view_id =
        last < ids.size() && &slot_image_views[ids[last]] == &view ? ids[last] : ids.back();
    return view;
}

ImageView& TextureCache::FindRenderTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    WatchImage(image, "render target");
    image.flags |= ImageFlagBits::GpuModified;
    image.gpu_write_tick = CurrentTick();
    if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8)) {
        QueueDownload(image_id);
    }
    image.usage.render_target = 1;
    UpdateImage(image_id);
    // Its metadata surfaces.
    if (const VAddr cmask = desc.info.meta_info.cmask_addr) {
        surface_metas.emplace(cmask, MetaInfo{.type = MetaType::CMask});
        image.info.meta_info.cmask_addr = cmask;
    }
    if (const VAddr fmask = desc.info.meta_info.fmask_addr) {
        surface_metas.emplace(fmask, MetaInfo{.type = MetaType::FMask});
        image.info.meta_info.fmask_addr = fmask;
    }
    return image.FindView(desc.view_info, false);
}

ImageView& TextureCache::FindDepthTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    WatchImage(image, "depth target");
    image.flags |= ImageFlagBits::GpuModified;
    image.gpu_write_tick = CurrentTick();
    image.usage.depth_target = 1;
    UpdateImage(image_id);
    if (const VAddr htile = desc.info.meta_info.htile_addr) {
        surface_metas.emplace(htile, MetaInfo{.type = MetaType::HTile,
                                              .clear_mask = image.info.meta_info.htile_clear_mask});
        image.info.meta_info.htile_addr = htile;
    }
    // The stencil surface: an image of its own (a placeholder of no format) linked to the depth.
    if (desc.info.stencil_addr != 0) {
        ImageId stencil_id{};
        ForEachImageInRegion(desc.info.stencil_addr, desc.info.stencil_size,
                             [&](ImageId id, Image& candidate) {
                                 if (candidate.info.guest_address == desc.info.stencil_addr) {
                                     stencil_id = id;
                                 }
                             });
        if (!stencil_id) {
            ImageInfo info{};
            info.guest_address = desc.info.stencil_addr;
            info.guest_size = desc.info.stencil_size;
            info.size = desc.info.size;
            stencil_id = InsertImage(info);
        }
        Image& stencil = slot_images[stencil_id];
        TouchImage(stencil);
        stencil.AssociateDepth(image_id, image.image_uid);
    }
    return image.FindView(desc.view_info, false);
}

ImageId TextureCache::GetAssociatedDepth(Image& image) {
    if (!image.depth_id) {
        return {};
    }
    if (slot_images.is_allocated(image.depth_id)) {
        const Image& depth = slot_images[image.depth_id];
        if (depth.image_uid == image.depth_uid && True(depth.flags & ImageFlagBits::Registered)) {
            return image.depth_id;
        }
    }
    image.DisassociateDepth(); // that depth image is gone
    return {};
}

vk::Sampler TextureCache::GetSampler(const Gcn::Sampler& sampler,
                                     Gcn::BorderColorBuffer border_color_base, bool is_depth,
                                     float extra_lod_bias) {
    // Compare and plain uses of one S# need samplers of their own, so do other LOD biases.
    const u64 key = HashCombine(HashCombine(XXH3_64bits(&sampler, sizeof(sampler)), is_depth),
                                u64(std::bit_cast<u32>(extra_lod_bias)));
    std::scoped_lock lock{samplers_mutex};
    const auto [it, created] =
        samplers.try_emplace(key, instance, sampler, border_color_base, is_depth, extra_lod_bias);
    if (created) {
        it.value().lru_id = sampler_lru.Insert(key, gc_tick);
    } else {
        sampler_lru.Touch(it->second.lru_id, gc_tick);
    }
    return it->second.Handle();
}

} // namespace VideoCore
