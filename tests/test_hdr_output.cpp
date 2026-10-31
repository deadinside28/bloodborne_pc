// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// The HDR output encode (render/shaders/hdr_output.frag) on a synthetic row, on this machine's
// GPU: the 2.2 decode, expand, the native+ pass-through above the game's white, the black floor,
// the HUD mask (UI Brightness), the menu compose and the PQ / scRGB encodes, against mirrored
// math. Self-contained (dynamic-loaded Vulkan; only the generated shader headers).
#include <cassert>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <vulkan/vulkan.h>
#include <cstdint>
using u32 = std::uint32_t;
using u16 = std::uint16_t;
using u8 = std::uint8_t;
using s32 = std::int32_t;
#include "bbgpu/shaders/fullscreen_vert.h"
#include "bbgpu/shaders/hdr_output_frag.h"
#include <bit>

// float -> IEEE half bits (the 16F images' texels in an upload buffer are halfs).
u16 Half(float value) {
    const u32 bits = std::bit_cast<u32>(value);
    const u16 sign = u16((bits >> 16) & 0x8000);
    const s32 exponent = s32((bits >> 23) & 0xFF) - 127 + 15;
    const u32 mantissa = bits & 0x7FFFFF;
    if (exponent <= 0) {
        return sign;
    }
    if (exponent >= 31) {
        return u16(sign | 0x7C00);
    }
    return u16(sign | (u32(exponent) << 10) | (mantissa >> 13));
}

// IEEE half bits -> float (the 16F output read back).
float FromHalf(u16 value) {
    const u32 sign = u32(value & 0x8000) << 16;
    const u32 exponent = (value >> 10) & 0x1F;
    const u32 mantissa = value & 0x3FF;
    u32 bits;
    if (exponent == 0) {
        bits = sign;
    } else if (exponent == 31) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }
    float result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

#define CHECK(call) assert((call) == VK_SUCCESS)

namespace {

struct Context {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    u32 family = 0;
    VkQueue queue{};
    VkCommandPool pool{};
    VkCommandBuffer cmd{};
};

Context Init() {
    Context vk;
    const VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                .pApplicationName = "hdr-output-test",
                                .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo info{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                    .pApplicationInfo = &app};
    CHECK(vkCreateInstance(&info, nullptr, &vk.instance));
    u32 count = 0;
    CHECK(vkEnumeratePhysicalDevices(vk.instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    CHECK(vkEnumeratePhysicalDevices(vk.instance, &count, devices.data()));
    for (u32 i = 0; i < count && !vk.physical; ++i) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[i], &properties);
        if (properties.apiVersion >= VK_API_VERSION_1_3) {
            vk.physical = devices[i];
            std::printf("HDR output: %s\n", properties.deviceName);
        }
    }
    assert(vk.physical);
    u32 families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &families, nullptr);
    std::vector<VkQueueFamilyProperties> family_properties(families);
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &families, family_properties.data());
    while (!(family_properties[vk.family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
        ++vk.family;
    }
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                             .queueFamilyIndex = vk.family,
                                             .queueCount = 1,
                                             .pQueuePriorities = &priority};
    // Dynamic rendering (vkCmdBeginRendering) is a Vulkan 1.3 feature: enable it, or the draw
    // is a no-op and the output stays blank.
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features13;
    vkGetPhysicalDeviceFeatures2(vk.physical, &features2);
    assert(features13.dynamicRendering);
    VkPhysicalDeviceVulkan13Features enabled13{};
    enabled13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    enabled13.dynamicRendering = VK_TRUE;
    const VkDeviceCreateInfo device_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &enabled13,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info};
    CHECK(vkCreateDevice(vk.physical, &device_info, nullptr, &vk.device));
    vkGetDeviceQueue(vk.device, vk.family, 0, &vk.queue);
    const VkCommandPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                            .queueFamilyIndex = vk.family};
    CHECK(vkCreateCommandPool(vk.device, &pool_info, nullptr, &vk.pool));
    const VkCommandBufferAllocateInfo cmd_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = vk.pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1};
    CHECK(vkAllocateCommandBuffers(vk.device, &cmd_info, &vk.cmd));
    return vk;
}

u32 MemoryType(const VkPhysicalDeviceMemoryProperties& memory, u32 bits, VkMemoryPropertyFlags wants) {
    for (u32 type = 0; type < memory.memoryTypeCount; ++type) {
        if ((bits & (1u << type)) &&
            (memory.memoryTypes[type].propertyFlags & wants) == wants) {
            return type;
        }
    }
    assert(false);
    return 0;
}

struct Buffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* mapped{};
};

Buffer MakeBuffer(const Context& vk, const VkPhysicalDeviceMemoryProperties& memory, VkDeviceSize size) {
    Buffer buffer;
    const VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .size = size,
                                  .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    CHECK(vkCreateBuffer(vk.device, &info, nullptr, &buffer.buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(vk.device, buffer.buffer, &requirements);
    const VkMemoryAllocateInfo allocate{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = MemoryType(memory, requirements.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    CHECK(vkAllocateMemory(vk.device, &allocate, nullptr, &buffer.memory));
    CHECK(vkBindBufferMemory(vk.device, buffer.buffer, buffer.memory, 0));
    CHECK(vkMapMemory(vk.device, buffer.memory, 0, size, 0, &buffer.mapped));
    return buffer;
}

struct Image {
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    VkFormat format{};
};

Image MakeImage(const Context& vk, const VkPhysicalDeviceMemoryProperties& memory, VkFormat format,
                u32 width, u32 height, VkImageUsageFlags extra) {
    Image image{.format = format};
    const VkImageCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                 .imageType = VK_IMAGE_TYPE_2D,
                                 .format = format,
                                 .extent = {width, height, 1},
                                 .mipLevels = 1,
                                 .arrayLayers = 1,
                                 .samples = VK_SAMPLE_COUNT_1_BIT,
                                 .tiling = VK_IMAGE_TILING_OPTIMAL,
                                 .usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | extra};
    CHECK(vkCreateImage(vk.device, &info, nullptr, &image.image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(vk.device, image.image, &requirements);
    const VkMemoryAllocateInfo allocate{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = MemoryType(memory, requirements.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CHECK(vkAllocateMemory(vk.device, &allocate, nullptr, &image.memory));
    CHECK(vkBindImageMemory(vk.device, image.image, image.memory, 0));
    const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .image = image.image,
                                     .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                     .format = format,
                                     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    CHECK(vkCreateImageView(vk.device, &view, nullptr, &image.view));
    return image;
}

void Destroy(const Context& vk, const Image& image) {
    vkDestroyImageView(vk.device, image.view, nullptr);
    vkDestroyImage(vk.device, image.image, nullptr);
    vkFreeMemory(vk.device, image.memory, nullptr);
}

void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
             VkPipelineStageFlags2 source = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
             VkAccessFlags2 source_access = VK_ACCESS_2_MEMORY_WRITE_BIT,
             VkPipelineStageFlags2 dest = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
             VkAccessFlags2 dest_access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT) {
    const VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = source,
                                        .srcAccessMask = source_access,
                                        .dstStageMask = dest,
                                        .dstAccessMask = dest_access,
                                        .oldLayout = from,
                                        .newLayout = to,
                                        .image = image,
                                        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    const VkDependencyInfo info{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                .imageMemoryBarrierCount = 1,
                                .pImageMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(cmd, &info);
}

VkShaderModule MakeModule(VkDevice device, const uint32_t* words, size_t words_count) {
    const VkShaderModuleCreateInfo info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                        .codeSize = words_count * sizeof(u32),
                                        .pCode = words};
    VkShaderModule module{};
    CHECK(vkCreateShaderModule(device, &info, nullptr, &module));
    return module;
}

} // namespace

int main() {
    const Context vk = Init();
    // The encode's push constants (hdr_output.frag; present_passes.h Settings).
    struct HdrPush {
        float paper_white, peak, boost;
        u32 flags;
        float black_floor, ui_white;
        float exposure, contrast, highlights, shadows, saturation;
    };
    constexpr u32 FlagScRgb = 1u, FlagOverlay = 2u, FlagNative16 = 16u, FlagUiMask = 32u;

    constexpr u32 W = 8, H = 1;
    constexpr float paper = 203.0f, peak = 1000.0f;
    constexpr std::array<float, W> values{0.0f, 0.5f, 1.0f, 0.7f, 2.0f, 0.5f, 2.0f, 0.9f};

    const VkPhysicalDeviceMemoryProperties memory_properties{
        [&] {
            VkPhysicalDeviceMemoryProperties props{};
            vkGetPhysicalDeviceMemoryProperties(vk.physical, &props);
            return props;
        }()};

    Image game = MakeImage(vk, memory_properties, VK_FORMAT_R16G16B16A16_SFLOAT, W, H, 0);
    Image overlay = MakeImage(vk, memory_properties, VK_FORMAT_R8G8B8A8_UNORM, W, H, 0);
    Image snapshot = MakeImage(vk, memory_properties, VK_FORMAT_R16G16B16A16_SFLOAT, W, H, 0);
    Image out_hdr10 = MakeImage(vk, memory_properties, VK_FORMAT_A2B10G10R10_UNORM_PACK32, W, H, 0);
    Image out_scrgb = MakeImage(vk, memory_properties, VK_FORMAT_R16G16B16A16_SFLOAT, W, H, 0);
    // One upload buffer per source (the GPU reads them at execution: a shared one would carry
    // the last fill into every image), and one for the readback.
    std::array<Buffer, 3> uploads{MakeBuffer(vk, memory_properties, W * H * 16),
                                  MakeBuffer(vk, memory_properties, W * H * 16),
                                  MakeBuffer(vk, memory_properties, W * H * 16)};
    Buffer read = MakeBuffer(vk, memory_properties, W * H * 16);

    const VkDescriptorSetLayoutBinding bindings[3]{
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    const VkDescriptorSetLayoutCreateInfo set_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3,
        .pBindings = bindings};
    VkDescriptorSetLayout set_layout{};
    CHECK(vkCreateDescriptorSetLayout(vk.device, &set_info, nullptr, &set_layout));
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * 8};
    const VkDescriptorPoolCreateInfo pool_create{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 8,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size};
    VkDescriptorPool pool{};
    CHECK(vkCreateDescriptorPool(vk.device, &pool_create, nullptr, &pool));
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 11 * 4};
    const VkPipelineLayoutCreateInfo layout_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push};
    VkPipelineLayout layout{};
    CHECK(vkCreatePipelineLayout(vk.device, &layout_info, nullptr, &layout));

    VkShaderModule vertex = MakeModule(vk.device, FULLSCREEN_VERT, std::size(FULLSCREEN_VERT));
    VkShaderModule fragment = MakeModule(vk.device, HDR_OUTPUT_FRAG, std::size(HDR_OUTPUT_FRAG));

    const auto make_pipeline = [&](VkFormat format) {
        const VkPipelineShaderStageCreateInfo stages[2]{
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
             VK_SHADER_STAGE_VERTEX_BIT, vertex, "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
             VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "main", nullptr}};
        const VkPipelineRenderingCreateInfo rendering{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &format};
        const VkPipelineColorBlendAttachmentState write_all{
            .blendEnable = VK_FALSE,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
        const VkPipelineColorBlendStateCreateInfo blend{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &write_all};
        const VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        const VkPipelineDynamicStateCreateInfo dynamic{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2,
            .pDynamicStates = dynamics};
        // The required fixed-function states (all defaults; the fullscreen vertex needs none).
        const VkPipelineVertexInputStateCreateInfo vertex_input{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        const VkPipelineInputAssemblyStateCreateInfo assembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        const VkPipelineViewportStateCreateInfo viewport_state{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount = 1};
        const VkPipelineRasterizationStateCreateInfo raster{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .lineWidth = 1.0f};
        const VkPipelineMultisampleStateCreateInfo multisample{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
        const VkGraphicsPipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                                .pNext = &rendering,
                                                .stageCount = 2,
                                                .pStages = stages,
                                                .pVertexInputState = &vertex_input,
                                                .pInputAssemblyState = &assembly,
                                                .pViewportState = &viewport_state,
                                                .pRasterizationState = &raster,
                                                .pMultisampleState = &multisample,
                                                .pColorBlendState = &blend,
                                                .pDynamicState = &dynamic,
                                                .layout = layout};
        VkPipeline pipeline{};
        CHECK(vkCreateGraphicsPipelines(vk.device, {}, 1, &info, nullptr, &pipeline));
        return pipeline;
    };
    VkPipeline hdr10_pipeline = make_pipeline(VK_FORMAT_A2B10G10R10_UNORM_PACK32);
    VkPipeline scrgb_pipeline = make_pipeline(VK_FORMAT_R16G16B16A16_SFLOAT);
    vkDestroyShaderModule(vk.device, vertex, nullptr);
    vkDestroyShaderModule(vk.device, fragment, nullptr);

    VkSampler sampler{};
    const VkSamplerCreateInfo sampler_info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                           .magFilter = VK_FILTER_LINEAR,
                                           .minFilter = VK_FILTER_LINEAR,
                                           .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST};
    CHECK(vkCreateSampler(vk.device, &sampler_info, nullptr, &sampler));

    // Mirrored math.
    const auto pq = [](float nits) {
        const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f;
        const float c2 = 18.8515625f, c3 = 18.6875f;
        const float ym = std::pow(std::clamp(nits / 10000.0f, 0.0f, 1.0f), m1);
        return std::pow((c1 + c2 * ym) / (1.0f + c3 * ym), m2);
    };
    const auto decode = [](float encoded) { return std::pow(std::max(encoded, 0.0f), 2.2f); };

    const auto fill16 = [&](Buffer& buffer, const std::array<float, W>& rgb, float a5, float a_else) {
        auto* texel = static_cast<u16*>(buffer.mapped);
        for (u32 p = 0; p < W; ++p) {
            texel[p * 4 + 0] = texel[p * 4 + 1] = texel[p * 4 + 2] = Half(rgb[p]);
            texel[p * 4 + 3] = Half(p == 5 ? a5 : a_else);
        }
    };
    const auto fill8 = [&](Buffer& buffer, float a6) {
        auto* texel = static_cast<u8*>(buffer.mapped);
        for (u32 p = 0; p < W; ++p) {
            texel[p * 4 + 0] = texel[p * 4 + 1] = texel[p * 4 + 2] = 255;
            texel[p * 4 + 3] = u8((p == 6 ? a6 : 0.0f) * 255.0f);
        }
    };

    u32 case_index = 0;
    const auto run = [&](VkPipeline pipeline, const Image& out, const HdrPush& push,
                         const std::array<float, W>& expected) {
        VkDescriptorSet set{};
        const VkDescriptorSetAllocateInfo set_allocate{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &set_layout};
        CHECK(vkAllocateDescriptorSets(vk.device, &set_allocate, &set));
        const VkDescriptorImageInfo infos[3]{{sampler, game.view, VK_IMAGE_LAYOUT_GENERAL},
                                             {sampler, overlay.view, VK_IMAGE_LAYOUT_GENERAL},
                                             {sampler, snapshot.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[3]{
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0,
             .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo = &infos[0]},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1,
             .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo = &infos[1]},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 2,
             .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo = &infos[2]}};
        vkUpdateDescriptorSets(vk.device, 3, writes, 0, nullptr);

        CHECK(vkResetCommandBuffer(vk.cmd, 0));
        const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                             .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        CHECK(vkBeginCommandBuffer(vk.cmd, &begin));
        const auto general = [&](VkImage image) {
            Barrier(vk.cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        };
        general(game.image);
        general(overlay.image);
        general(snapshot.image);
        const VkBufferImageCopy copy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                     .imageExtent = {W, H, 1}};
        fill16(uploads[0], values, 0.0f, 1.0f);
        vkCmdCopyBufferToImage(vk.cmd, uploads[0].buffer, game.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        fill8(uploads[1], 1.0f); // the menu: white at column 6
        vkCmdCopyBufferToImage(vk.cmd, uploads[1].buffer, overlay.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        fill16(uploads[2], values, 1.0f, 0.0f); // the HUD's mask at column 5
        vkCmdCopyBufferToImage(vk.cmd, uploads[2].buffer, snapshot.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        Barrier(vk.cmd, out.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        const VkRenderingAttachmentInfo attachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = out.view,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        const VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                        .renderArea = {{0, 0}, {W, H}},
                                        .layerCount = 1,
                                        .colorAttachmentCount = 1,
                                        .pColorAttachments = &attachment};
        vkCmdBeginRendering(vk.cmd, &rendering);
        vkCmdBindPipeline(vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(vk.cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        const VkViewport viewport{0.0f, 0.0f, float(W), float(H), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, {W, H}};
        vkCmdSetViewport(vk.cmd, 0, 1, &viewport);
        vkCmdSetScissor(vk.cmd, 0, 1, &scissor);
        vkCmdDraw(vk.cmd, 3, 1, 0, 0);
        vkCmdEndRendering(vk.cmd);
        Barrier(vk.cmd, out.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
        vkCmdCopyImageToBuffer(vk.cmd, out.image, VK_IMAGE_LAYOUT_GENERAL, read.buffer, 1, &copy);
        CHECK(vkEndCommandBuffer(vk.cmd));
        const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &vk.cmd};
        CHECK(vkQueueSubmit(vk.queue, 1, &submit, {}));
        CHECK(vkQueueWaitIdle(vk.queue));
        vkFreeDescriptorSets(vk.device, pool, 1, &set);

        const auto* words = static_cast<const u32*>(read.mapped);
        const auto* floats = static_cast<const float*>(read.mapped);
        if (std::getenv("BB_TEST_DUMP")) {
            std::printf("raw:");
            for (u32 p = 0; p < W; ++p) {
                std::printf(" %08x", words[p]);
            }
            std::printf("\n");
            std::fflush(stdout);
        }
        for (u32 p = 0; p < W; ++p) {
            float got;
            if (out.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
                const u32 word = words[p];
                got = float((word >> 20) & 0x3FF) / 1023.0f; // the red channel
            } else {
                // The 16F output: its texels are halfs; back to white-relative.
                const auto* halves = static_cast<const u16*>(read.mapped);
                got = FromHalf(halves[p * 4]) * 80.0f / paper;
            }
            if (std::abs(got - expected[p]) > 0.004f) {
                std::fprintf(stderr, "HDR output regression: case %u pixel %u: actual %.5f, "
                                     "expected %.5f\n",
                             case_index, p, got, expected[p]);
                std::fflush(stderr);
            }
            assert(std::abs(got - expected[p]) <= 0.004f);
        }
        std::printf("HDR output: case %u PASS\n", case_index);
        ++case_index;
    };

    const auto settings = [&](u32 flags, float boost, float black_floor, float ui_white) {
        // The menu layer is always uploaded (white at its pixel): its flag is on throughout.
        return HdrPush{paper, peak, boost, flags | FlagOverlay, black_floor, ui_white,
                       1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    };
    // Expand, boost 0 (the SDR picture at paper white).
    run(hdr10_pipeline, out_hdr10, settings(0, 0.0f, 0.0f, 1.0f),
        {pq(0), pq(decode(0.5f) * paper), pq(decode(1.0f) * paper), pq(decode(0.7f) * paper),
         pq(decode(2.0f) * paper), pq(decode(0.5f) * paper), pq(paper),
         pq(decode(0.9f) * paper)});
    // Expand, boost 1 (the white reaches the peak; below the knee the game's picture stands).
    // Above the knee the brightest pixels run to the peak and clamp there; the menu's own pixel
    // stays at paper white.
    run(hdr10_pipeline, out_hdr10, settings(0, 1.0f, 0.0f, 1.0f),
        {pq(0), pq(decode(0.5f) * paper), pq(peak), pq(decode(0.7f) * paper), 1.0f,
         pq(decode(0.5f) * paper), pq(paper), pq(decode(0.9f) * paper)});
    // Native+: nothing expanded — 2.0 decodes above the game's white; the menu forces paper
    // white at its pixel.
    run(hdr10_pipeline, out_hdr10, settings(FlagNative16, 1.0f, 0.0f, 1.0f),
        {pq(0), pq(decode(0.5f) * paper), pq(paper), pq(decode(0.7f) * paper),
         pq(decode(2.0f) * paper), pq(decode(0.5f) * paper), pq(paper), pq(decode(0.9f) * paper)});
    // Native+ with the black floor.
    run(hdr10_pipeline, out_hdr10, settings(FlagNative16, 0.0f, 0.1f, 1.0f),
        {pq(0), pq((decode(0.5f) - 0.1f) / 0.9f * paper), pq(paper),
         pq((decode(0.7f) - 0.1f) / 0.9f * paper), pq((decode(2.0f) - 0.1f) / 0.9f * paper),
         pq((decode(0.5f) - 0.1f) / 0.9f * paper), pq(paper),
         pq((decode(0.9f) - 0.1f) / 0.9f * paper)});
    // Native+ with the HUD mask: column 5 is UI, shown at UI Brightness (1.5x paper white).
    run(hdr10_pipeline, out_hdr10, settings(FlagNative16 | FlagUiMask, 0.0f, 0.0f, 1.5f),
        {pq(0), pq(decode(0.5f) * paper), pq(paper), pq(decode(0.7f) * paper),
         pq(decode(2.0f) * paper), pq(decode(0.5f) * 1.5f * paper), pq(paper),
         pq(decode(0.9f) * paper)});
    // scRGB: linear values as they are (80 nits = 1.0).
    run(scrgb_pipeline, out_scrgb, settings(FlagNative16 | FlagScRgb, 0.0f, 0.0f, 1.0f),
        {0.0f, decode(0.5f), 1.0f, decode(0.7f), decode(2.0f), decode(0.5f), 1.0f, decode(0.9f)});

    std::printf("HDR output: PASS\n");
    vkDestroyPipeline(vk.device, hdr10_pipeline, nullptr);
    vkDestroyPipeline(vk.device, scrgb_pipeline, nullptr);
    vkDestroyPipelineLayout(vk.device, layout, nullptr);
    vkDestroyDescriptorSetLayout(vk.device, set_layout, nullptr);
    vkDestroyDescriptorPool(vk.device, pool, nullptr);
    vkDestroySampler(vk.device, sampler, nullptr);
    for (const Image* image : {&game, &overlay, &snapshot, &out_hdr10, &out_scrgb}) {
        Destroy(vk, *image);
    }
    for (const Buffer* buffer : {&uploads[0], &uploads[1], &uploads[2], &read}) {
        vkDestroyBuffer(vk.device, buffer->buffer, nullptr);
        vkFreeMemory(vk.device, buffer->memory, nullptr);
    }
    vkDestroyCommandPool(vk.device, vk.pool, nullptr);
    vkDestroyDevice(vk.device, nullptr);
    vkDestroyInstance(vk.instance, nullptr);
    return 0;
}
