#pragma once

#include "preview_scene.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace skeleton_repair {

class VulkanError : public std::runtime_error {
public:
    VulkanError(const char* call, VkResult result);
    VkResult Result;
};

// A device created by the host. It must be Vulkan 1.3 with dynamicRendering and synchronization2 enabled.
struct VulkanDevice {
    VkPhysicalDevice Physical = VK_NULL_HANDLE;
    VkDevice Device = VK_NULL_HANDLE;
    VkQueue Queue = VK_NULL_HANDLE;
    uint32_t QueueFamily = 0;
};

enum class ColorMode {
    Clay,
    Material,
    // Each vertex takes the color of its strongest bone (BoneNameColor).
    Bone,
};

struct RenderOptions {
    // One byte per scene mesh. Missing entries are hidden.
    std::span<const uint8_t> VisibleMeshes;
    bool ShowSkeleton = true;
    bool ShowGrid = true;
    ColorMode Coloring = ColorMode::Clay;
    // Display-encoded RGB.
    std::array<float, 3> Background{0.11f, 0.12f, 0.14f};
};

// Draws a PreviewScene into its own offscreen image for an ImGui panel (or a screenshot).
// After the recorded commands run, the image is in SHADER_READ_ONLY_OPTIMAL.
class ModelRenderer {
public:
    static constexpr VkFormat ColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    static constexpr VkFormat DepthFormat = VK_FORMAT_D32_SFLOAT;

    explicit ModelRenderer(const VulkanDevice& device);
    ~ModelRenderer();
    ModelRenderer(const ModelRenderer&) = delete;
    ModelRenderer& operator=(const ModelRenderer&) = delete;

    // Uploads the scene and waits for the copy. Waits for the device first if a scene is already loaded.
    void SetScene(const PreviewScene& scene);
    void ClearScene();
    // Replaces only the skeleton lines of the current scene, for example to show a different skeleton or diff colors.
    void SetSkeleton(std::span<const LineVertex> skeleton);
    bool HasScene() const { return index_count_ > 0 || line_count_ > 0; }

    // Recreates the target when the size changes, waiting for the device first.
    // Returns true when the image view changed and any texture handle for it must be refreshed.
    bool Resize(uint32_t width, uint32_t height);

    // Records the offscreen pass. Call outside any render pass, before the commands that sample the image.
    void Record(VkCommandBuffer command_buffer, const OrbitCamera& camera, const RenderOptions& options);

    VkImageView View() const { return resolve_.View; }
    VkSampler Sampler() const { return sampler_; }
    VkExtent2D Extent() const { return extent_; }
    VkSampleCountFlagBits Samples() const { return samples_; }

    // Copies the last rendered image to RGBA8 rows, waiting for the device.
    std::vector<uint8_t> ReadPixels();

private:
    struct Buffer {
        VkBuffer Handle = VK_NULL_HANDLE;
        VkDeviceMemory Memory = VK_NULL_HANDLE;
        VkDeviceSize Size = 0;
    };
    struct Image {
        VkImage Handle = VK_NULL_HANDLE;
        VkDeviceMemory Memory = VK_NULL_HANDLE;
        VkImageView View = VK_NULL_HANDLE;
    };

    void Release();
    uint32_t MemoryType(uint32_t type_bits, VkMemoryPropertyFlags properties) const;
    Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) const;
    void Destroy(Buffer& buffer) const;
    Image CreateImage(VkFormat format, VkImageUsageFlags usage, VkSampleCountFlagBits samples, VkImageAspectFlags aspect) const;
    void Destroy(Image& image) const;
    void DestroyTargets();
    void CreatePipelines();
    template <typename Fn>
    void Submit(Fn&& record);
    Buffer Upload(const void* data, VkDeviceSize size, VkBufferUsageFlags usage);
    void UploadLines(std::span<const LineVertex> skeleton);

    VulkanDevice device_;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkPipelineLayout mesh_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout line_layout_ = VK_NULL_HANDLE;
    VkPipeline mesh_pipeline_ = VK_NULL_HANDLE;
    VkPipeline line_pipeline_ = VK_NULL_HANDLE;

    VkExtent2D extent_{0, 0};
    Image color_;
    Image depth_;
    Image resolve_;
    // UNDEFINED until the first Record; afterwards the image holds the last frame.
    bool rendered_ = false;

    Buffer vertices_;
    Buffer indices_;
    Buffer lines_;
    uint32_t index_count_ = 0;
    uint32_t line_count_ = 0;
    uint32_t grid_first_ = 0;
    uint32_t grid_count_ = 0;
    uint32_t skeleton_first_ = 0;
    uint32_t skeleton_count_ = 0;
    std::vector<PreviewDraw> draws_;
    std::vector<LineVertex> grid_;
};

}  // namespace skeleton_repair
