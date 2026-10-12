#pragma once
#include "model_library.hpp"
#include "model_renderer.hpp"
#include <imgui.h>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace skeleton_repair {
// Gallery thumbnails are rendered once at this size and reused from disk afterwards.
constexpr uint32_t kThumbnailSize = 256;

struct ThumbnailPixels {
    uint32_t Width = 0;
    uint32_t Height = 0;
    // RGBA8 rows.
    std::vector<uint8_t> Rgba;
};

// The folder that holds rendered thumbnails, or empty when local app data is unavailable.
std::filesystem::path ThumbnailCacheFolder();
// The cached image for `model`. The name changes whenever the source archive or the base game archive changes.
std::filesystem::path ThumbnailCachePath(const std::filesystem::path& folder, const LibraryModel& model, const std::filesystem::path& game);
// Reads a cached PNG. Returns nothing when it is missing or unreadable. The calling thread must have initialized COM.
std::optional<ThumbnailPixels> LoadThumbnailImage(const std::filesystem::path& path);
// Writes a PNG atomically. The calling thread must have initialized COM.
void SaveThumbnailImage(const std::filesystem::path& path, const ThumbnailPixels& pixels);

// A static RGBA image shown through ImGui. Requires imgui_impl_vulkan to be initialized.
class ThumbnailImage {
public:
    ThumbnailImage(const VulkanDevice& device, const ThumbnailPixels& pixels);
    ~ThumbnailImage();
    ThumbnailImage(const ThumbnailImage&) = delete;
    ThumbnailImage& operator=(const ThumbnailImage&) = delete;
    ImTextureID Texture() const { return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(set_)); }

private:
    void Release();
    VulkanDevice device_;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
};
}
