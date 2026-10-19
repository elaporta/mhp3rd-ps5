// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ge_scene.hpp"
#include <algorithm>
#include <array>
#if YAKUMO_WITH_PROFILE
#include "profile_probe.hpp"
#endif
#include "ge_shaders.inc"

#define GE_COMMANDS(X) \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) \
    X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(CreateImage) X(DestroyImage) \
    X(GetImageMemoryRequirements) X(BindImageMemory) X(CreateSampler) X(DestroySampler) \
    X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
    X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) \
    X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateGraphicsPipelines) X(DestroyPipeline) \
    X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdBindVertexBuffers) X(CmdPushConstants) X(CmdDraw) \
    X(CmdPipelineBarrier) X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) X(CmdBlitImage)

namespace yakumo::ps5 {
// Isolated synthetic renderer. The original pinned shaders are compiled unchanged.
// Every handle is installed immediately so partial setup can be cleaned up.
template<class API> struct GeGpu {
    API& vk;
    Log& log;
    VkPhysicalDeviceMemoryProperties memory{};
    struct Buffer { VkBuffer handle{}; VkDeviceMemory memory{}; } vertex, environment, object, staging, readback;
    struct Image { VkImage handle{}; VkDeviceMemory memory{}; VkImageView view{}; } color, depth, texture;
    VkRenderPass pass{};
    VkFramebuffer framebuffer{};
    VkSampler sampler{};
    VkShaderModule shaders[3]{};
    VkDescriptorSetLayout sets[2]{};
    VkDescriptorPool descriptors{};
    VkDescriptorSet bindings[2]{};
    VkPipelineLayout layout{};
    VkPipeline pipelines[2][3]{}; // specialized/plain x opaque/alpha/blend
    ge::Scene scene;
    ge::Scene base_scene;
    std::array<std::uint32_t,4> profile_texels{};
    bool cleaned{};
    GeGpu(API& api, Log& output) : vk(api), log(output) {}
    ~GeGpu() { if (!cleaned) cleanup(); }
    bool result(VkResult r, const char* operation) {
        if (r == VK_SUCCESS) return true;
        log.write("[ge] FAIL %s result=%d", operation, r); return false;
    }
#define GE_OK(expr) do { if (!result((expr), #expr)) return false; } while (0)
    unsigned memory_type(unsigned bits, VkMemoryPropertyFlags flags) {
        for (unsigned i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
        log.write("[ge] FAIL no memory type bits=%#x flags=%#x", bits, flags); return UINT32_MAX;
    }
    bool allocate(const VkMemoryRequirements& needs, VkMemoryPropertyFlags flags, VkDeviceMemory& output) {
        VkMemoryAllocateInfo info{}; info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        info.allocationSize = needs.size; info.memoryTypeIndex = memory_type(needs.memoryTypeBits, flags);
        if (info.memoryTypeIndex == UINT32_MAX) return false;
        GE_OK(vk.AllocateMemory(vk.device, &info, nullptr, &output)); return true;
    }
    bool buffer(Buffer& out, VkDeviceSize size, VkBufferUsageFlags usage, const void* data = nullptr) {
        VkBufferCreateInfo info{}; info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size; info.usage = usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        GE_OK(vk.CreateBuffer(vk.device, &info, nullptr, &out.handle));
        VkMemoryRequirements needs{}; vk.GetBufferMemoryRequirements(vk.device, out.handle, &needs);
        if (!allocate(needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, out.memory)) return false;
        GE_OK(vk.BindBufferMemory(vk.device, out.handle, out.memory, 0));
        if (data) {
            void* mapped{}; GE_OK(vk.MapMemory(vk.device, out.memory, 0, size, 0, &mapped));
            std::memcpy(mapped, data, size); vk.UnmapMemory(vk.device, out.memory);
        }
        return true;
    }
    bool image(Image& out, unsigned w, unsigned h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
        VkImageCreateInfo info{}; info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D; info.format = format; info.extent = {w, h, 1};
        info.mipLevels = info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        GE_OK(vk.CreateImage(vk.device, &info, nullptr, &out.handle));
        VkMemoryRequirements needs{}; vk.GetImageMemoryRequirements(vk.device, out.handle, &needs);
        if (!allocate(needs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out.memory)) return false;
        GE_OK(vk.BindImageMemory(vk.device, out.handle, out.memory, 0));
        VkImageViewCreateInfo view{}; view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = out.handle; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format;
        view.subresourceRange = {aspect, 0, 1, 0, 1};
        GE_OK(vk.CreateImageView(vk.device, &view, nullptr, &out.view)); return true;
    }
    bool format(VkPhysicalDevice gpu, VkFormat fmt, VkFormatFeatureFlags needed) {
        VkFormatProperties props{}; vk.GetPhysicalDeviceFormatProperties(gpu, fmt, &props);
        log.write("[ge] format=%d optimal=%#x required=%#x (capability check only)", fmt, props.optimalTilingFeatures, needed);
        if ((props.optimalTilingFeatures & needed) == needed) return true;
        log.write("[ge] FAIL required format features absent"); return false;
    }
    bool setup(VkPhysicalDevice gpu, VkFormat swap_format) {
#if YAKUMO_WITH_PROFILE
        if(!profile_graphics_scene(scene,profile_texels,log))return false;
#else
        std::copy(std::begin(ge::texels),std::end(ge::texels),profile_texels.begin());
#endif
        base_scene=scene;
        vk.GetPhysicalDeviceMemoryProperties(gpu, &memory);
        if (!format(gpu, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT) ||
            !format(gpu, VK_FORMAT_D32_SFLOAT, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ||
            !format(gpu, swap_format, VK_FORMAT_FEATURE_BLIT_DST_BIT)) return false;
        if (!image(color, ge::width, ge::height, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
            !image(depth, ge::width, ge::height, VK_FORMAT_D32_SFLOAT,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT) ||
            !image(texture, 2, 2, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) return false;
        const float zeros[124]{};
        if (!buffer(vertex, sizeof(scene.vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, scene.vertices) ||
            !buffer(environment, 496, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, zeros) ||
            !buffer(object, 144, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, zeros) ||
            !buffer(staging, sizeof(ge::texels), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, profile_texels.data()) ||
            !buffer(readback, ge::width * ge::height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return false;
        VkSamplerCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_NEAREST; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        GE_OK(vk.CreateSampler(vk.device, &si, nullptr, &sampler));
        VkAttachmentDescription attachments[2]{};
        attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM; attachments[1].format = VK_FORMAT_D32_SFLOAT;
        for (auto& a : attachments) {
            a.samples = VK_SAMPLE_COUNT_1_BIT; a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE; a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        }
        attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference cr{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dr{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &cr; sub.pDepthStencilAttachment = &dr;
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0; deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo rp{}; rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rp.attachmentCount = 2; rp.pAttachments = attachments; rp.subpassCount = 1; rp.pSubpasses = &sub;
        rp.dependencyCount = 2; rp.pDependencies = deps;
        GE_OK(vk.CreateRenderPass(vk.device, &rp, nullptr, &pass));
        VkImageView views[] = {color.view, depth.view};
        VkFramebufferCreateInfo fb{}; fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = pass; fb.attachmentCount = 2; fb.pAttachments = views;
        fb.width = ge::width; fb.height = ge::height; fb.layers = 1;
        GE_OK(vk.CreateFramebuffer(vk.device, &fb, nullptr, &framebuffer));
        const std::uint32_t* code[] = {kGeVertexShader, kGeFragmentShader, kGeFragmentShaderPlain};
        const std::size_t sizes[] = {sizeof(kGeVertexShader), sizeof(kGeFragmentShader), sizeof(kGeFragmentShaderPlain)};
        for (unsigned i = 0; i < 3; ++i) {
            VkShaderModuleCreateInfo sm{}; sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            sm.codeSize = sizes[i]; sm.pCode = code[i];
            GE_OK(vk.CreateShaderModule(vk.device, &sm, nullptr, &shaders[i]));
        }
        VkDescriptorSetLayoutBinding tex{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutBinding uniform[] = {
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo dl{}; dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dl.bindingCount = 1; dl.pBindings = &tex;
        GE_OK(vk.CreateDescriptorSetLayout(vk.device, &dl, nullptr, &sets[0]));
        dl.bindingCount = 2; dl.pBindings = uniform;
        GE_OK(vk.CreateDescriptorSetLayout(vk.device, &dl, nullptr, &sets[1]));
        VkDescriptorPoolSize pool_sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2}};
        VkDescriptorPoolCreateInfo dp{}; dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dp.maxSets = 2; dp.poolSizeCount = 2; dp.pPoolSizes = pool_sizes;
        GE_OK(vk.CreateDescriptorPool(vk.device, &dp, nullptr, &descriptors));
        VkDescriptorSetAllocateInfo da{}; da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        da.descriptorPool = descriptors; da.descriptorSetCount = 2; da.pSetLayouts = sets;
        GE_OK(vk.AllocateDescriptorSets(vk.device, &da, bindings));
        VkDescriptorImageInfo ti{sampler, texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo ub[] = {{environment.handle, 0, 496}, {object.handle, 0, 144}};
        VkWriteDescriptorSet writes[3]{};
        for (unsigned i = 0; i < 3; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].descriptorCount = 1;
            writes[i].dstSet = bindings[i ? 1 : 0]; writes[i].dstBinding = i ? i - 1 : 0;
            writes[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            if (i) writes[i].pBufferInfo = &ub[i - 1]; else writes[i].pImageInfo = &ti;
        }
        vk.UpdateDescriptorSets(vk.device, 3, writes, 0, nullptr);
        VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ge::Push)};
        VkPipelineLayoutCreateInfo pl{}; pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount = 2; pl.pSetLayouts = sets; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &range;
        GE_OK(vk.CreatePipelineLayout(vk.device, &pl, nullptr, &layout));
        for (unsigned variant = 0; variant < 2; ++variant)
            for (unsigned kind = 0; kind < 3; ++kind) if (!pipeline(variant, kind)) return false;
        log.write("[ge] setup complete: original decoded GE vertex, specialized(false/true)/plain fragment; RGBA8+D32; six pipelines");
        return true;
    }
    bool pipeline(unsigned variant, unsigned kind) {
        log.write("[ge] creating pipeline variant=%s kind=%u", variant ? "plain" : "specialized", kind);
        VkPipelineShaderStageCreateInfo stages[2]{};
        for (auto& s : stages) { s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; s.pName = "main"; }
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = shaders[0];
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = shaders[variant ? 2 : 1];
        const VkBool32 alpha = kind == 1;
        const VkSpecializationMapEntry entry{0, 0, sizeof(alpha)};
        const VkSpecializationInfo specialization{1, &entry, sizeof(alpha), &alpha};
        if (!variant) stages[1].pSpecializationInfo = &specialization;
        VkVertexInputBindingDescription binding{0, sizeof(ge::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attributes[] = {{0,0,VK_FORMAT_R32G32B32A32_SFLOAT,0},
            {1,0,VK_FORMAT_R32G32_SFLOAT,16}, {2,0,VK_FORMAT_R8G8B8A8_UNORM,24}, {3,0,VK_FORMAT_R32G32B32_SFLOAT,28}};
        VkPipelineVertexInputStateCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = 4; vi.pVertexAttributeDescriptions = attributes;
        VkPipelineInputAssemblyStateCreateInfo ia{}; ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport{0, 0, float(ge::width), float(ge::height), 0, 1};
        VkRect2D scissor{{0,0}, {ge::width, ge::height}};
        VkPipelineViewportStateCreateInfo vp{}; vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = vp.scissorCount = 1; vp.pViewports = &viewport; vp.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo rs{}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{}; ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = kind != 2; ds.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState ba{}; ba.colorWriteMask = 15; ba.blendEnable = kind == 2;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.colorBlendOp = ba.alphaBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        VkPipelineColorBlendStateCreateInfo bs{}; bs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        bs.attachmentCount = 1; bs.pAttachments = &ba;
        VkGraphicsPipelineCreateInfo p{}; p.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        p.stageCount = 2; p.pStages = stages; p.pVertexInputState = &vi; p.pInputAssemblyState = &ia;
        p.pViewportState = &vp; p.pRasterizationState = &rs; p.pMultisampleState = &ms;
        p.pDepthStencilState = &ds; p.pColorBlendState = &bs; p.layout = layout; p.renderPass = pass;
        GE_OK(vk.CreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &p, nullptr, &pipelines[variant][kind])); return true;
    }
    void barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
                 VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags before, VkPipelineStageFlags after) {
        VkImageMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = src; b.dstAccessMask = dst; b.oldLayout = from; b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk.CmdPipelineBarrier(cmd, before, after, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    ge::Markers markers{};
    bool dynamic{};
    bool update_markers(const ge::Markers& value) {
        markers=value; dynamic=true;scene=base_scene;
        for(unsigned i=0;i<2;++i) scene.quad(float(markers.x[i]),260,12,8,.5f,markers.color[i]);
        void* mapped{};GE_OK(vk.MapMemory(vk.device,vertex.memory,0,sizeof(scene.vertices),0,&mapped));
        std::memcpy(mapped,scene.vertices,sizeof(scene.vertices));vk.UnmapMemory(vk.device,vertex.memory);
        return true;
    }
    void record(VkCommandBuffer cmd, VkImage swap, VkExtent2D extent, unsigned frame) {
        if (!frame) {
            barrier(cmd, texture.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {2,2,1};
            vk.CmdCopyBufferToImage(cmd, staging.handle, texture.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            barrier(cmd, texture.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
        VkClearValue clear[2]{}; clear[0].color.float32[3] = 1; clear[1].depthStencil.depth = 1;
        VkRenderPassBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass = pass; begin.framebuffer = framebuffer; begin.renderArea.extent = {ge::width,ge::height};
        begin.clearValueCount = 2; begin.pClearValues = clear;
        vk.CmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
        const std::uint32_t offsets[] = {0,0}; const VkDeviceSize offset = 0;
        vk.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, bindings, 2, offsets);
        vk.CmdBindVertexBuffers(cmd, 0, 1, &vertex.handle, &offset);
        for (unsigned i = 0; i < scene.draw_count; ++i) {
            const auto& d = scene.draws[i]; const auto push = ge::push(d);
            vk.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[(frame % ge::frames) / 600][d.blend ? 2 : d.alpha ? 1 : 0]);
            vk.CmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
            vk.CmdDraw(cmd, 6, 1, d.first, 0);
        }
        vk.CmdEndRenderPass(cmd);
        const auto shader_frame=frame % ge::frames;
        if (shader_frame == 0 || shader_frame == 599 || shader_frame == 600 || shader_frame == 1199 || (dynamic && frame%300==0)) {
            VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            copy.imageExtent = {ge::width,ge::height,1};
            vk.CmdCopyImageToBuffer(cmd, color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.handle, 1, &copy);
            VkBufferMemoryBarrier host{}; host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.buffer = readback.handle; host.size = VK_WHOLE_SIZE;
            vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
        }
        if (!swap) return; // Headless host verification uses the same offscreen draw/readback path.
        barrier(cmd, swap, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit blit{}; blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        blit.srcOffsets[1] = {int(ge::width),int(ge::height),1}; blit.dstOffsets[1] = {int(extent.width),int(extent.height),1};
        vk.CmdBlitImage(cmd, color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        barrier(cmd, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
    bool check(unsigned frame) {
        void* mapped{}; GE_OK(vk.MapMemory(vk.device, readback.memory, 0, ge::width * ge::height * 4, 0, &mapped));
        const auto* bytes = static_cast<const std::uint8_t*>(mapped); bool passed = true;
        for (const auto& s : ge::samples) {
            const auto* p = bytes + (s.y * ge::width + s.x) * 4; const bool ok = ge::matches(bytes, s);
            log.write("[ge] pixel frame=%u %s got=%u,%u,%u expected=%d,%d,%d %s", frame + 1,
                s.name, p[0], p[1], p[2], s.r, s.g, s.b, ok ? "PASS" : "FAIL");
            passed &= ok;
        }
        if(dynamic) for(const auto& s:markers.expected) {
            const bool ok=ge::matches(bytes,s);const auto* p=bytes+(s.y*ge::width+s.x)*4;
            log.write("[integration-ge] frame=%u marker=%s got=%u,%u,%u expected=%d,%d,%d %s",frame+1,s.name,p[0],p[1],p[2],s.r,s.g,s.b,ok?"PASS":"FAIL");
            passed &= ok;
        }
        vk.UnmapMemory(vk.device, readback.memory); return passed;
    }
    bool cleanup() {
        cleaned = true;
        if (!vk.device) return true;
        log.write("[ge] cleanup waiting for device idle");
        const VkResult idle = vk.DeviceWaitIdle ? vk.DeviceWaitIdle(vk.device) : VK_ERROR_INITIALIZATION_FAILED;
        log.write("[ge] cleanup DeviceWaitIdle=%d", idle);
        for (auto& group : pipelines) for (auto p : group) if (p) vk.DestroyPipeline(vk.device, p, nullptr);
        if (layout) vk.DestroyPipelineLayout(vk.device, layout, nullptr);
        if (descriptors) vk.DestroyDescriptorPool(vk.device, descriptors, nullptr);
        for (auto s : sets) if (s) vk.DestroyDescriptorSetLayout(vk.device, s, nullptr);
        for (auto s : shaders) if (s) vk.DestroyShaderModule(vk.device, s, nullptr);
        if (sampler) vk.DestroySampler(vk.device, sampler, nullptr);
        if (framebuffer) vk.DestroyFramebuffer(vk.device, framebuffer, nullptr);
        if (pass) vk.DestroyRenderPass(vk.device, pass, nullptr);
        for (auto* i : {&color, &depth, &texture}) {
            if (i->view) vk.DestroyImageView(vk.device, i->view, nullptr);
            if (i->handle) vk.DestroyImage(vk.device, i->handle, nullptr);
            if (i->memory) vk.FreeMemory(vk.device, i->memory, nullptr);
        }
        for (auto* b : {&vertex, &environment, &object, &staging, &readback}) {
            if (b->handle) vk.DestroyBuffer(vk.device, b->handle, nullptr);
            if (b->memory) vk.FreeMemory(vk.device, b->memory, nullptr);
        }
        log.write("[ge] cleanup complete; destroy calls returned (void API); idle=%d", idle);
        return idle == VK_SUCCESS;
    }
#undef GE_OK
};
}
