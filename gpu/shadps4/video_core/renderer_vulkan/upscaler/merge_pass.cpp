// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/upscaler/merge_pass.h"
#include <array>
#include "video_core/host_shaders/upscale_merge_comp.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

MergePass::MergePass(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {}

MergePass::~MergePass() {
    Destroy();
}

void MergePass::Destroy() {
    pipeline.reset();
    pipeline_layout.reset();
    desc_layout.reset();
}

void MergePass::CreatePipelines() {
    if (pipeline) return;
    const auto device = instance.GetDevice();
    std::array<vk::DescriptorSetLayoutBinding, 5> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i) {
        bindings[i] = {
            .binding = i,
            .descriptorType = i < 3 ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        };
    }
    desc_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange push{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(u32),
    };
    pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    }));
    const auto module = CompileSPV(UPSCALE_MERGE_COMP, device);
    pipeline = Check(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
            .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = module, .pName = "main"},
            .layout = *pipeline_layout,
        }));
    device.destroyShaderModule(module);
}

void MergePass::Record(vk::CommandBuffer cmdbuf, vk::ImageView output_view, vk::ImageView scene_view,
                       vk::ImageView mask_view, vk::ImageView motion_view, vk::ImageView object_motion_view,
                       u32 ow, u32 oh, u32 mode) {
    const vk::DescriptorImageInfo out_i{.imageView = output_view, .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo scn_i{.imageView = scene_view, .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo msk_i{.imageView = mask_view, .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo mot_i{.imageView = motion_view, .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo obj_i{.imageView = object_motion_view ? object_motion_view : motion_view,
                                        .imageLayout = vk::ImageLayout::eGeneral};

    const std::array<vk::WriteDescriptorSet, 5> writes = {{
        {.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &out_i},
        {.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &scn_i},
        {.dstBinding = 2, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &msk_i},
        {.dstBinding = 3, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &mot_i},
        {.dstBinding = 4, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &obj_i},
    }};

    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(mode), &mode);
    cmdbuf.dispatch((ow + 7) / 8, (oh + 7) / 8, 1);
}

} // namespace Vulkan
