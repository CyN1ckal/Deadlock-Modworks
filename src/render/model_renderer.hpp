#pragma once

#include "preview_materials.hpp"
#include "preview_scene.hpp"
#include "skeleton_pose.hpp"

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
// Enable textureCompressionBC too where supported; block-compressed textures are skipped without it.
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
    // Material color maps from SetMaterials; draws without one fall back to clay.
    Textured,
};

// One animation frame: skinning matrices for every model bone and, optionally, the posed skeleton lines.
struct FramePose {
    std::span<const Mat34> Skinning;
    // Same line count as the scene's skeleton; ignored otherwise.
    std::span<const LineVertex> Skeleton;
};

struct RenderOptions {
    // One byte per scene mesh. Missing entries are hidden.
    std::span<const uint8_t> VisibleMeshes;
    bool ShowSkeleton = true;
    bool ShowGrid = true;
    ColorMode Coloring = ColorMode::Clay;
    // Display-encoded RGB.
    std::array<float, 3> Background{0.11f, 0.12f, 0.14f};
    // Bind pose when null.
    const FramePose* Pose = nullptr;
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
    // Drops the mesh, skeleton and textures. Waits for the device first, because a frame in flight may still draw them.
    void ClearScene();
    // Replaces only the skeleton lines of the current scene, for example to show a different skeleton or diff colors.
    void SetSkeleton(std::span<const LineVertex> skeleton);
    // Uploads color maps for the current scene's draws, matched by material name, and waits for the copy.
    // Returns a note for each texture the device cannot sample.
    std::vector<std::string> SetMaterials(std::span<const PreviewMaterial> materials);
    size_t TextureCount() const { return textures_.size(); }
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
    // Renders one frame on its own submit and returns it as RGBA8 rows, waiting for the device.
    std::vector<uint8_t> Capture(const OrbitCamera& camera, const RenderOptions& options);

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
    Image CreateImage(VkFormat format, VkImageUsageFlags usage, VkSampleCountFlagBits samples, VkImageAspectFlags aspect,
        uint32_t width, uint32_t height, uint32_t mip_levels = 1) const;
    void Destroy(Image& image) const;
    struct Texture {
        Image Pixels;
        VkDescriptorSet Set = VK_NULL_HANDLE;
    };
    void DestroyTextures();
    // Creates images and descriptor sets for `images`, uploading every mip in one submit. Null entries are skipped.
    std::vector<Texture> CreateTextures(std::span<const TextureImage* const> images);
    void DestroyTargets();
    void CreatePipelines();
    template <typename Fn>
    void Submit(Fn&& record);
    Buffer Upload(const void* data, VkDeviceSize size, VkBufferUsageFlags usage);
    void UploadLines(std::span<const LineVertex> skeleton);
    // Writes `bytes` into `buffer` inside the command buffer, ordered after earlier reads in `reader_stage`.
    static void UpdateBuffer(VkCommandBuffer command_buffer, VkBuffer buffer, VkDeviceSize offset, std::span<const uint8_t> bytes,
        VkPipelineStageFlags2 reader_stage, VkAccessFlags2 reader_access);

    VulkanDevice device_;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkPipelineLayout mesh_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout line_layout_ = VK_NULL_HANDLE;
    VkPipeline mesh_pipeline_ = VK_NULL_HANDLE;
    VkPipeline blend_pipeline_ = VK_NULL_HANDLE;
    VkPipeline line_pipeline_ = VK_NULL_HANDLE;
    VkSampler texture_sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    // Set 1: the bone matrix storage buffer, rewritten by Record for animated frames.
    VkDescriptorSetLayout bone_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool bone_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet bone_set_ = VK_NULL_HANDLE;
    // Holds the fallback set and the current textures; recreated by SetMaterials.
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    Texture fallback_;

    VkExtent2D extent_{0, 0};
    Image color_;
    Image depth_;
    Image resolve_;
    // UNDEFINED until the first Record; afterwards the image holds the last frame.
    bool rendered_ = false;

    Buffer vertices_;
    Buffer indices_;
    Buffer lines_;
    Buffer bones_;
    uint32_t bone_count_ = 0;
    uint32_t index_count_ = 0;
    uint32_t line_count_ = 0;
    uint32_t grid_first_ = 0;
    uint32_t grid_count_ = 0;
    uint32_t skeleton_first_ = 0;
    uint32_t skeleton_count_ = 0;
    std::vector<PreviewDraw> draws_;
    std::vector<LineVertex> grid_;
    std::vector<Texture> textures_;
    struct DrawMaterial {
        int Texture = -1;
        bool AlphaTest = false;
        bool Translucent = false;
    };
    // One per draw, filled by SetMaterials.
    std::vector<DrawMaterial> draw_materials_;
};

}  // namespace skeleton_repair
