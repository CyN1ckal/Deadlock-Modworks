#pragma once
#include "model_library.hpp"
#include "mod_store.hpp"
#include "model_renderer.hpp"
#include <imgui.h>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace skeleton_repair {
// Gallery thumbnails are rendered once at this size and reused from the library database afterwards.
constexpr uint32_t kThumbnailSize = 256;

struct ThumbnailPixels {
    uint32_t Width = 0;
    uint32_t Height = 0;
    // RGBA8 rows.
    std::vector<uint8_t> Rgba;
};

// The image folder used by earlier releases, or empty when local app data is unavailable. Only read, never changed.
std::filesystem::path LegacyThumbnailFolder();
// Where an earlier release cached the thumbnail of `model`, which still names the archive copy it stored.
std::filesystem::path LegacyThumbnailPath(const std::filesystem::path& folder, const LibraryModel& model, const std::filesystem::path& game);
// The library database key of `model`'s thumbnail. It changes with the archive's content and the base game archive.
std::string ThumbnailKey(const LocalModStore* store, const LibraryModel& model, const std::filesystem::path& game);
// Reads a PNG. Returns nothing when it is missing or unreadable. The calling thread must have initialized COM.
std::optional<ThumbnailPixels> DecodeThumbnail(const std::vector<uint8_t>& png);
std::optional<ThumbnailPixels> LoadThumbnailImage(const std::filesystem::path& path);
// Encodes an opaque PNG. The calling thread must have initialized COM.
std::vector<uint8_t> EncodeThumbnail(const ThumbnailPixels& pixels);

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
