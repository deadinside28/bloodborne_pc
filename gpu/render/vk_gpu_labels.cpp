// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "render/vk_gpu_labels.h"
#include "shim/bb_env.h"

namespace Vulkan {

void RecordPortableGpuLabel(vk::CommandBuffer command, vk::Buffer buffer, u64 offset,
                            u64 value, u32 num_bytes) {
    const auto barrier = [&](vk::PipelineStageFlags2 source, vk::AccessFlags2 source_access,
                             vk::PipelineStageFlags2 dest, vk::AccessFlags2 dest_access) {
        const vk::MemoryBarrier2 memory{.srcStageMask = source, .srcAccessMask = source_access,
                                        .dstStageMask = dest, .dstAccessMask = dest_access};
        command.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &memory});
    };
    // fillBuffer writes the word itself; updateBuffer made Adreno's driver allocate kernel
    // memory for its data at every call (46% of the recording thread in game, 7 FPS).
    // BB_LABEL_UPDATE_BUFFER=1: updateBuffer as before.
    static const bool update = bb_env_on("BB_LABEL_UPDATE_BUFFER");
    const auto write = [&](u64 at, u32 word) {
        if (update) {
            command.updateBuffer(buffer, at, sizeof(word), &word);
        } else {
            command.fillBuffer(buffer, at, sizeof(word), word);
        }
    };
    // Wait for all earlier work, including reads of a reused label, and publish its outputs
    // to the CPU before publishing completion. the words are written while
    // recording; no host write to guest memory or callback writes the label.
    barrier(vk::PipelineStageFlagBits2::eAllCommands,
            vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            vk::PipelineStageFlagBits2::eTransfer | vk::PipelineStageFlagBits2::eHost,
            vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eHostRead);
    if (num_bytes == 8) {
        const u32 high = u32(value >> 32);
        write(offset + 4, high);
        // A CPU polling the low word must never see its new value with the old high word.
        barrier(vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                vk::PipelineStageFlagBits2::eTransfer | vk::PipelineStageFlagBits2::eHost,
                vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eHostRead);
    }
    const u32 low = u32(value);
    write(offset, low);
    // Subsequent GPU consumers and coherent host polling both see the label.
    barrier(vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eAllCommands | vk::PipelineStageFlagBits2::eHost,
            vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite |
                vk::AccessFlagBits2::eHostRead);
}

} // namespace Vulkan
