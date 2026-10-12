#include "model_renderer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

namespace skeleton_repair {
namespace {

// SPIR-V compiled from src/render/shaders by glslc at build time.
const uint32_t kMeshVert[] = {
#include "mesh.vert.spv.inc"
};
const uint32_t kMeshFrag[] = {
#include "mesh.frag.spv.inc"
};
const uint32_t kLineVert[] = {
#include "line.vert.spv.inc"
};
const uint32_t kLineFrag[] = {
#include "line.frag.spv.inc"
};

void Check(VkResult result, const char* call) {
    if (result != VK_SUCCESS) {
        throw VulkanError(call, result);
    }
}

struct MeshPush {
    Mat4 ViewProjection;
    std::array<float, 4> Eye;
    std::array<float, 4> Color;
};
constexpr uint32_t kMeshPushColorOffset = offsetof(MeshPush, Color);

constexpr std::array<float, 3> kClay{0.70f, 0.68f, 0.64f};

VkShaderModule CreateShader(VkDevice device, std::span<const uint32_t> code) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = code.size_bytes();
    info.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    Check(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule");
    return module;
}

void Barrier(VkCommandBuffer command_buffer, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
    VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(command_buffer, &dependency);
}

}  // namespace

VulkanError::VulkanError(const char* call, VkResult result)
    : std::runtime_error(std::string(call) + " failed (VkResult " + std::to_string(static_cast<int>(result)) + ")."), Result(result) {}

ModelRenderer::ModelRenderer(const VulkanDevice& device) : device_(device) {
    vkGetPhysicalDeviceMemoryProperties(device_.Physical, &memory_);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device_.Physical, &properties);
    const VkSampleCountFlags supported = properties.limits.framebufferColorSampleCounts & properties.limits.framebufferDepthSampleCounts;
    samples_ = (supported & VK_SAMPLE_COUNT_4_BIT) != 0 ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;

    try {
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = device_.QueueFamily;
        Check(vkCreateCommandPool(device_.Device, &pool, nullptr, &command_pool_), "vkCreateCommandPool");

        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = VK_FILTER_LINEAR;
        sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = VK_LOD_CLAMP_NONE;
        Check(vkCreateSampler(device_.Device, &sampler, nullptr, &sampler_), "vkCreateSampler");

        CreatePipelines();
    } catch (...) {
        Release();
        throw;
    }
}

ModelRenderer::~ModelRenderer() {
    Release();
}

void ModelRenderer::Release() {
    if (device_.Device == VK_NULL_HANDLE) {
        return;
    }
    vkDeviceWaitIdle(device_.Device);
    ClearScene();
    DestroyTargets();
    vkDestroyPipeline(device_.Device, mesh_pipeline_, nullptr);
    vkDestroyPipeline(device_.Device, line_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_.Device, mesh_layout_, nullptr);
    vkDestroyPipelineLayout(device_.Device, line_layout_, nullptr);
    vkDestroySampler(device_.Device, sampler_, nullptr);
    vkDestroyCommandPool(device_.Device, command_pool_, nullptr);
    mesh_pipeline_ = line_pipeline_ = VK_NULL_HANDLE;
    mesh_layout_ = line_layout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    command_pool_ = VK_NULL_HANDLE;
    device_.Device = VK_NULL_HANDLE;
}

uint32_t ModelRenderer::MemoryType(uint32_t type_bits, VkMemoryPropertyFlags properties) const {
    for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) != 0 && (memory_.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    throw VulkanError("Finding a memory type", VK_ERROR_FEATURE_NOT_PRESENT);
}

ModelRenderer::Buffer ModelRenderer::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) const {
    Buffer buffer;
    buffer.Size = size;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Check(vkCreateBuffer(device_.Device, &info, nullptr, &buffer.Handle), "vkCreateBuffer");

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_.Device, buffer.Handle, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, properties);
    if (VkResult result = vkAllocateMemory(device_.Device, &allocate, nullptr, &buffer.Memory); result != VK_SUCCESS) {
        vkDestroyBuffer(device_.Device, buffer.Handle, nullptr);
        throw VulkanError("vkAllocateMemory", result);
    }
    vkBindBufferMemory(device_.Device, buffer.Handle, buffer.Memory, 0);
    return buffer;
}

void ModelRenderer::Destroy(Buffer& buffer) const {
    vkDestroyBuffer(device_.Device, buffer.Handle, nullptr);
    vkFreeMemory(device_.Device, buffer.Memory, nullptr);
    buffer = {};
}

ModelRenderer::Image ModelRenderer::CreateImage(VkFormat format, VkImageUsageFlags usage, VkSampleCountFlagBits samples,
    VkImageAspectFlags aspect) const {
    Image image;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {extent_.width, extent_.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = samples;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Check(vkCreateImage(device_.Device, &info, nullptr, &image.Handle), "vkCreateImage");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_.Device, image.Handle, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (VkResult result = vkAllocateMemory(device_.Device, &allocate, nullptr, &image.Memory); result != VK_SUCCESS) {
        vkDestroyImage(device_.Device, image.Handle, nullptr);
        throw VulkanError("vkAllocateMemory", result);
    }
    vkBindImageMemory(device_.Device, image.Handle, image.Memory, 0);

    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image.Handle;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {aspect, 0, 1, 0, 1};
    if (VkResult result = vkCreateImageView(device_.Device, &view, nullptr, &image.View); result != VK_SUCCESS) {
        Image partial = image;
        Destroy(partial);
        throw VulkanError("vkCreateImageView", result);
    }
    return image;
}

void ModelRenderer::Destroy(Image& image) const {
    vkDestroyImageView(device_.Device, image.View, nullptr);
    vkDestroyImage(device_.Device, image.Handle, nullptr);
    vkFreeMemory(device_.Device, image.Memory, nullptr);
    image = {};
}

void ModelRenderer::DestroyTargets() {
    Destroy(color_);
    Destroy(depth_);
    Destroy(resolve_);
    rendered_ = false;
}

void ModelRenderer::CreatePipelines() {
    VkPushConstantRange mesh_push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MeshPush)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &mesh_push;
    Check(vkCreatePipelineLayout(device_.Device, &layout, nullptr, &mesh_layout_), "vkCreatePipelineLayout");
    VkPushConstantRange line_push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4)};
    layout.pPushConstantRanges = &line_push;
    Check(vkCreatePipelineLayout(device_.Device, &layout, nullptr, &line_layout_), "vkCreatePipelineLayout");

    struct Modules {
        VkDevice Device;
        VkShaderModule Vertex = VK_NULL_HANDLE;
        VkShaderModule Fragment = VK_NULL_HANDLE;
        ~Modules() {
            vkDestroyShaderModule(Device, Vertex, nullptr);
            vkDestroyShaderModule(Device, Fragment, nullptr);
        }
    };

    auto build = [&](std::span<const uint32_t> vert, std::span<const uint32_t> frag, bool lines, VkPipelineLayout pipeline_layout) {
        Modules modules{device_.Device};
        modules.Vertex = CreateShader(device_.Device, vert);
        modules.Fragment = CreateShader(device_.Device, frag);
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, modules.Vertex, "main", nullptr};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, modules.Fragment, "main", nullptr};

        const VkVertexInputBindingDescription binding{0, lines ? static_cast<uint32_t>(sizeof(LineVertex)) : static_cast<uint32_t>(sizeof(PreviewVertex)),
            VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription attributes[2] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
            lines ? VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(LineVertex, Color)}
                  : VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(PreviewVertex, Normal)},
        };
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        input.vertexBindingDescriptionCount = 1;
        input.pVertexBindingDescriptions = &binding;
        input.vertexAttributeDescriptionCount = 2;
        input.pVertexAttributeDescriptions = attributes;

        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        // Mods do not agree on winding, so draw both sides.
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = samples_;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = lines ? VK_FALSE : VK_TRUE;
        depth.depthCompareOp = lines ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;

        // Lines switch depth testing per draw: the grid is occluded by the model, the skeleton is drawn on top.
        const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = lines ? 3 : 2;
        dynamic.pDynamicStates = dynamic_states;

        const VkFormat color_format = ColorFormat;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &color_format;
        rendering.depthAttachmentFormat = DepthFormat;

        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.pNext = &rendering;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = pipeline_layout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        Check(vkCreateGraphicsPipelines(device_.Device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline), "vkCreateGraphicsPipelines");
        return pipeline;
    };

    mesh_pipeline_ = build(kMeshVert, kMeshFrag, false, mesh_layout_);
    line_pipeline_ = build(kLineVert, kLineFrag, true, line_layout_);
}

template <typename Fn>
void ModelRenderer::Submit(Fn&& record) {
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = command_pool_;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    Check(vkAllocateCommandBuffers(device_.Device, &allocate, &command_buffer), "vkAllocateCommandBuffers");
    VkFence fence = VK_NULL_HANDLE;
    try {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(vkBeginCommandBuffer(command_buffer, &begin), "vkBeginCommandBuffer");
        record(command_buffer);
        Check(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer");

        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        Check(vkCreateFence(device_.Device, &fence_info, nullptr, &fence), "vkCreateFence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffer;
        Check(vkQueueSubmit(device_.Queue, 1, &submit, fence), "vkQueueSubmit");
        Check(vkWaitForFences(device_.Device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    } catch (...) {
        vkDestroyFence(device_.Device, fence, nullptr);
        vkFreeCommandBuffers(device_.Device, command_pool_, 1, &command_buffer);
        throw;
    }
    vkDestroyFence(device_.Device, fence, nullptr);
    vkFreeCommandBuffers(device_.Device, command_pool_, 1, &command_buffer);
}

ModelRenderer::Buffer ModelRenderer::Upload(const void* data, VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer staging = CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Buffer target;
    try {
        void* mapped = nullptr;
        Check(vkMapMemory(device_.Device, staging.Memory, 0, size, 0, &mapped), "vkMapMemory");
        std::memcpy(mapped, data, static_cast<size_t>(size));
        vkUnmapMemory(device_.Device, staging.Memory);

        target = CreateBuffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Submit([&](VkCommandBuffer command_buffer) {
            const VkBufferCopy region{0, 0, size};
            vkCmdCopyBuffer(command_buffer, staging.Handle, target.Handle, 1, &region);
        });
    } catch (...) {
        Destroy(staging);
        Destroy(target);
        throw;
    }
    Destroy(staging);
    return target;
}

void ModelRenderer::SetScene(const PreviewScene& scene) {
    if (vertices_.Handle != VK_NULL_HANDLE || indices_.Handle != VK_NULL_HANDLE || lines_.Handle != VK_NULL_HANDLE) {
        Check(vkDeviceWaitIdle(device_.Device), "vkDeviceWaitIdle");
    }
    ClearScene();

    if (!scene.Indices.empty()) {
        vertices_ = Upload(scene.Vertices.data(), scene.Vertices.size() * sizeof(PreviewVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        indices_ = Upload(scene.Indices.data(), scene.Indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        index_count_ = static_cast<uint32_t>(scene.Indices.size());
        draws_ = scene.Draws;
    }

    std::vector<LineVertex> lines = scene.Grid;
    lines.insert(lines.end(), scene.Skeleton.begin(), scene.Skeleton.end());
    if (!lines.empty()) {
        lines_ = Upload(lines.data(), lines.size() * sizeof(LineVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        line_count_ = static_cast<uint32_t>(lines.size());
        grid_first_ = 0;
        grid_count_ = static_cast<uint32_t>(scene.Grid.size());
        skeleton_first_ = grid_count_;
        skeleton_count_ = static_cast<uint32_t>(scene.Skeleton.size());
    }
}

void ModelRenderer::ClearScene() {
    Destroy(vertices_);
    Destroy(indices_);
    Destroy(lines_);
    index_count_ = line_count_ = 0;
    grid_first_ = grid_count_ = skeleton_first_ = skeleton_count_ = 0;
    draws_.clear();
}

bool ModelRenderer::Resize(uint32_t width, uint32_t height) {
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    if (resolve_.Handle != VK_NULL_HANDLE && extent_.width == width && extent_.height == height) {
        return false;
    }
    if (resolve_.Handle != VK_NULL_HANDLE) {
        Check(vkDeviceWaitIdle(device_.Device), "vkDeviceWaitIdle");
    }
    DestroyTargets();
    extent_ = {width, height};
    try {
        resolve_ = CreateImage(ColorFormat,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        if (samples_ != VK_SAMPLE_COUNT_1_BIT) {
            color_ = CreateImage(ColorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                samples_, VK_IMAGE_ASPECT_COLOR_BIT);
        }
        depth_ = CreateImage(DepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
            samples_, VK_IMAGE_ASPECT_DEPTH_BIT);
    } catch (...) {
        DestroyTargets();
        extent_ = {0, 0};
        throw;
    }
    return true;
}

void ModelRenderer::Record(VkCommandBuffer command_buffer, const OrbitCamera& camera, const RenderOptions& options) {
    if (resolve_.Handle == VK_NULL_HANDLE) {
        return;
    }
    const bool msaa = samples_ != VK_SAMPLE_COUNT_1_BIT;

    // Previous contents are discarded. The source scopes cover last frame's sampling, attachment writes and depth use.
    Barrier(command_buffer, resolve_.Handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    if (msaa) {
        Barrier(command_buffer, color_.Handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    }
    Barrier(command_buffer, depth_.Handle, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = msaa ? color_.View : resolve_.View;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (msaa) {
        color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color.resolveImageView = resolve_.View;
        color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{options.Background[0], options.Background[1], options.Background[2], 1.0f}};

    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = depth_.View;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, extent_};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    rendering.pDepthAttachment = &depth;
    vkCmdBeginRendering(command_buffer, &rendering);

    const VkViewport viewport{0, 0, static_cast<float>(extent_.width), static_cast<float>(extent_.height), 0, 1};
    const VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewport(command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);

    const Mat4 view_projection = camera.ViewProjection(static_cast<float>(extent_.width) / static_cast<float>(extent_.height));
    const VkDeviceSize zero = 0;

    if (index_count_ > 0) {
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipeline_);
        vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertices_.Handle, &zero);
        vkCmdBindIndexBuffer(command_buffer, indices_.Handle, 0, VK_INDEX_TYPE_UINT32);
        const Vec3 eye = camera.Eye();
        MeshPush push{view_projection, {eye.X, eye.Y, eye.Z, 1}, {kClay[0], kClay[1], kClay[2], 1}};
        constexpr VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        vkCmdPushConstants(command_buffer, mesh_layout_, stages, 0, sizeof(push), &push);
        for (const PreviewDraw& draw : draws_) {
            if (draw.Mesh >= options.VisibleMeshes.size() || options.VisibleMeshes[draw.Mesh] == 0) {
                continue;
            }
            if (options.ColorByMaterial) {
                const std::array<float, 4> tint{draw.MaterialColor[0], draw.MaterialColor[1], draw.MaterialColor[2], 1};
                vkCmdPushConstants(command_buffer, mesh_layout_, stages, kMeshPushColorOffset, sizeof(tint), tint.data());
            }
            vkCmdDrawIndexed(command_buffer, draw.IndexCount, 1, draw.FirstIndex, draw.VertexOffset, 0);
        }
    }

    const bool grid = options.ShowGrid && grid_count_ > 0;
    const bool skeleton = options.ShowSkeleton && skeleton_count_ > 0;
    if (grid || skeleton) {
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, line_pipeline_);
        vkCmdBindVertexBuffers(command_buffer, 0, 1, &lines_.Handle, &zero);
        vkCmdPushConstants(command_buffer, line_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(view_projection), view_projection.data());
        if (grid) {
            vkCmdSetDepthTestEnable(command_buffer, VK_TRUE);
            vkCmdDraw(command_buffer, grid_count_, 1, grid_first_, 0);
        }
        if (skeleton) {
            vkCmdSetDepthTestEnable(command_buffer, VK_FALSE);
            vkCmdDraw(command_buffer, skeleton_count_, 1, skeleton_first_, 0);
        }
    }

    vkCmdEndRendering(command_buffer);
    Barrier(command_buffer, resolve_.Handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    rendered_ = true;
}

std::vector<uint8_t> ModelRenderer::ReadPixels() {
    if (!rendered_) {
        throw VulkanError("ReadPixels before Record", VK_ERROR_INITIALIZATION_FAILED);
    }
    const VkDeviceSize size = static_cast<VkDeviceSize>(extent_.width) * extent_.height * 4;
    Buffer readback = CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    std::vector<uint8_t> pixels(static_cast<size_t>(size));
    try {
        Submit([&](VkCommandBuffer command_buffer) {
            Barrier(command_buffer, resolve_.Handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {extent_.width, extent_.height, 1};
            vkCmdCopyImageToBuffer(command_buffer, resolve_.Handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.Handle, 1, &region);
            Barrier(command_buffer, resolve_.Handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

            VkBufferMemoryBarrier2 host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.buffer = readback.Handle;
            host.size = VK_WHOLE_SIZE;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.bufferMemoryBarrierCount = 1;
            dependency.pBufferMemoryBarriers = &host;
            vkCmdPipelineBarrier2(command_buffer, &dependency);
        });
        void* mapped = nullptr;
        Check(vkMapMemory(device_.Device, readback.Memory, 0, size, 0, &mapped), "vkMapMemory");
        std::memcpy(pixels.data(), mapped, pixels.size());
        vkUnmapMemory(device_.Device, readback.Memory);
    } catch (...) {
        Destroy(readback);
        throw;
    }
    Destroy(readback);
    return pixels;
}

}  // namespace skeleton_repair
