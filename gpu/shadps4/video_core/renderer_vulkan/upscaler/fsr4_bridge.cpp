// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/upscaler/fsr4_bridge.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "bbport_settings.h"

#include "video_core/host_shaders/fsr4_decode_comp.h"
#include "video_core/host_shaders/fsr4_encode_comp.h"
#include "video_core/host_shaders/fsr4_reactive_comp.h"

#include <algorithm>
#include <cstdio>

namespace Vulkan {

Fsr4Bridge::Fsr4Bridge(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    fsr4 = std::make_unique<Fsr4Upscaler>(instance, scheduler);
}

Fsr4Bridge::~Fsr4Bridge() {
    Destroy();
}

void Fsr4Bridge::Destroy() {
    fsr4_linear_view.reset();
    fsr4_linear_image = VideoCore::UniqueImage{};
    fsr4_linear_frame = false;
}

bool Fsr4Bridge::IsActive() const {
    const int selected = BbSettings::Get().upscaler;
    const bool supported = selected == BbSettings::UpscalerFsr411
                               ? instance.IsFsr411Supported()
                               : instance.IsFsr4Int8Supported();
    return BbSettings::IsFsr4(selected) && supported && !fsr4_failed && fsr4 != nullptr;
}

void Fsr4Bridge::CreatePipelines() {
    if (fsr4_decode_pipeline) return;

    const auto device = instance.GetDevice();
    const auto compute = [&](const auto& code, vk::PipelineLayout layout) {
        const auto module = CompileSPV(code, device);
        auto pipeline = Check(device.createComputePipelineUnique({}, vk::ComputePipelineCreateInfo{
            .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = module, .pName = "main"},
            .layout = layout}));
        device.destroyShaderModule(module);
        return pipeline;
    };

    // Decode pipeline
    {
        const std::array<vk::DescriptorSetLayoutBinding, 2> bindings{{
            {.binding = 0, .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 1, .descriptorType = vk::DescriptorType::eStorageImage,
             .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
        }};
        fsr4_decode_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()), .pBindings = bindings.data()}));
        const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, sizeof(u32)};
        fsr4_decode_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1, .pSetLayouts = &*fsr4_decode_desc_layout,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &push}));
        fsr4_decode_pipeline = compute(FSR4_DECODE_COMP, *fsr4_decode_pipeline_layout);
    }

    // Encode pipeline
    {
        std::array<vk::DescriptorSetLayoutBinding, 1> bindings{{
            {.binding = 0, .descriptorType = vk::DescriptorType::eStorageImage,
             .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
        }};
        fsr4_encode_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = 1, .pBindings = bindings.data()}));
        const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, sizeof(u32)};
        fsr4_encode_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1, .pSetLayouts = &*fsr4_encode_desc_layout,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &push}));
        fsr4_encode_pipeline = compute(FSR4_ENCODE_COMP, *fsr4_encode_pipeline_layout);
    }

    // Reactive pipeline
    {
        std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
        for (u32 i = 0; i < bindings.size(); ++i) {
            bindings[i] = {.binding = i,
                           .descriptorType = i < 2 ? vk::DescriptorType::eCombinedImageSampler
                                                   : vk::DescriptorType::eStorageImage,
                           .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        fsr4_reactive_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()), .pBindings = bindings.data()}));
        const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, 5 * sizeof(float)};
        fsr4_reactive_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1, .pSetLayouts = &*fsr4_reactive_desc_layout,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &push}));
        fsr4_linear_sampler = Check(device.createSamplerUnique({
            .magFilter = vk::Filter::eLinear, .minFilter = vk::Filter::eLinear,
            .mipmapMode = vk::SamplerMipmapMode::eNearest,
            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
            .addressModeW = vk::SamplerAddressMode::eClampToEdge}));
        fsr4_reactive_pipeline = compute(FSR4_REACTIVE_COMP, *fsr4_reactive_pipeline_layout);
    }
}

void Fsr4Bridge::EnsureResources(u32 width, u32 height) {
    const auto device = instance.GetDevice();
    const auto allocator = instance.GetAllocator();
    fsr4_linear_view.reset();
    fsr4_linear_image = VideoCore::UniqueImage(device, allocator);
    fsr4_linear_image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent = {width, height, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    fsr4_linear_view = Check(device.createImageViewUnique({
        .image = vk::Image(fsr4_linear_image), .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}));
    CreatePipelines();
}

bool Fsr4Bridge::Record(vk::CommandBuffer cmdbuf, Fsr4Upscaler::Image color,
                        Fsr4Upscaler::Image depth, vk::Image motion_image,
                        vk::ImageView motion_view, vk::Image output_image,
                        vk::ImageView output_view, u32 w, u32 h, u32 ow, u32 oh,
                        int applied_preset, const std::array<float, 2>& jitter,
                        float frame_ms, float near_plane, float vertical_fov,
                        bool reset) {
    if (!fsr4) return false;
    const auto& settings = BbSettings::Get();
    const float sign =
        BbToggle::Disabled(1u << 26) != settings.fsr4_invert_jitter.load() ? -1.0f : 1.0f;
    const bool ok = fsr4->Record({
        .cmdbuf = cmdbuf,
        .color = color,
        .depth = depth,
        .motion = {motion_image, motion_view, w, h},
        .output = {output_image, output_view, ow, oh},
        .render_width = w,
        .render_height = h,
        .preset = applied_preset,
        .jitter = {sign * jitter[0], sign * jitter[1]},
        .frame_ms = frame_ms,
        .near_plane = near_plane,
        .far_plane = 3000.0f,
        .vertical_fov = vertical_fov,
        .sharpness = std::min(settings.sharpness.load(), 1.0f),
        .sharpen = settings.sharpen,
        .reset = reset,
        .auto_exposure = settings.fsr4_auto_exposure,
    });

    static std::string shown;
    const char* problem = fsr4->Problem();
    if (!problem) {
        BbSettings::Get().fsr4_problem = nullptr;
    } else if (shown != problem) {
        shown = problem;
        static std::array<std::string, 8> kept;
        static u32 next = 0;
        kept[next] = shown;
        BbSettings::Get().fsr4_problem = kept[next].c_str();
        next = (next + 1) % kept.size();
    }
    if (!ok && fsr4->Fatal()) {
        std::printf("Upscaler: falling back to FSR 3.1\n");
        BbSettings::Get().upscaler = BbSettings::UpscalerFsr3;
        fsr4_failed = true;
    }
    return ok;
}

bool Fsr4Bridge::RecordDecode(vk::CommandBuffer cmdbuf, vk::ImageView color_view,
                              u32 source_width, u32 source_height,
                              Fsr4Upscaler::Image& decoded_input) {
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const vk::ImageMemoryBarrier2 to_write{
        .srcStageMask = all, .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eGeneral,
        .image = vk::Image(fsr4_linear_image),
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
    const vk::MemoryBarrier2 source_ready{
        .srcStageMask = all, .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &source_ready,
                             .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_write});

    const vk::DescriptorImageInfo src{.imageView = color_view, .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo dst{.imageView = *fsr4_linear_view, .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 2> writes{{
        {.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &src},
        {.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &dst},
    }};
    const u32 unused = 0;
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *fsr4_decode_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *fsr4_decode_pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*fsr4_decode_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(unused), &unused);
    cmdbuf.dispatch((source_width + 7) / 8, (source_height + 7) / 8, 1);

    const vk::MemoryBarrier2 decoded{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = all, .dstAccessMask = vk::AccessFlagBits2::eShaderRead};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &decoded});
    decoded_input = {vk::Image(fsr4_linear_image), *fsr4_linear_view, source_width, source_height};
    return true;
}

void Fsr4Bridge::RecordEncode(vk::CommandBuffer cmdbuf, vk::ImageView output_view, u32 ow, u32 oh) {
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const vk::MemoryBarrier2 upscaled{
        .srcStageMask = all, .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &upscaled});

    const vk::DescriptorImageInfo out{.imageView = output_view, .imageLayout = vk::ImageLayout::eGeneral};
    const vk::WriteDescriptorSet write{.dstBinding = 0, .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &out};
    const u32 unused = 0;
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *fsr4_encode_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *fsr4_encode_pipeline_layout, 0, write);
    cmdbuf.pushConstants(*fsr4_encode_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(unused), &unused);
    cmdbuf.dispatch((ow + 7) / 8, (oh + 7) / 8, 1);

    const vk::MemoryBarrier2 encoded{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = all, .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &encoded});
}

void Fsr4Bridge::RecordReactive(vk::CommandBuffer cmdbuf, vk::ImageView color,
                                vk::ImageView reactive_mask, vk::ImageView output_view,
                                u32 w, u32 h, u32 ow, u32 oh, const std::array<float, 2>& jitter) {
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const vk::MemoryBarrier2 before{
        .srcStageMask = all, .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});

    const vk::DescriptorImageInfo current{.sampler = *fsr4_linear_sampler, .imageView = color, .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo mask{.sampler = *fsr4_linear_sampler, .imageView = reactive_mask, .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo out{.imageView = output_view, .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 3> writes{{
        {.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &current},
        {.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &mask},
        {.dstBinding = 2, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &out},
    }};
    const std::array<float, 5> push{float(w), float(h), jitter[0], jitter[1], 1.0f};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *fsr4_reactive_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *fsr4_reactive_pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*fsr4_reactive_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), push.data());
    cmdbuf.dispatch((ow + 7) / 8, (oh + 7) / 8, 1);

    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = all, .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
}

} // namespace Vulkan
