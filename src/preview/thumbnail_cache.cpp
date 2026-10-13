#include "thumbnail_cache.hpp"
#include "error.hpp"
#include "texture.hpp"
#include "strings.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <imgui_impl_vulkan.h>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>

namespace skeleton_repair {
namespace {
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
// Bump to discard every cached thumbnail, for example after the renderer's look changes.
constexpr char kCacheVersion[] = "thumbnail-v2";
// The version used by the image files of earlier releases, which are still read once and copied into the library.
constexpr char kLegacyCacheVersion[] = "thumbnail-v1";

void Hash(uint64_t& hash, std::string_view text) {
    for (const unsigned char c : text) { hash ^= c; hash *= 0x100000001b3ull; }
    hash ^= 0xff; hash *= 0x100000001b3ull;
}
// Identifies one version of a file by its normalized path, size and write time.
void HashFile(uint64_t& hash, const fs::path& path) {
    std::wstring name = fs::absolute(path).lexically_normal().wstring();
    CharLowerBuffW(name.data(), static_cast<DWORD>(name.size()));
    Hash(hash, Narrow(name));
    std::error_code error;
    const auto size = fs::file_size(path, error);
    Hash(hash, error ? "?" : std::to_string(size));
    const auto stamp = fs::last_write_time(path, error);
    Hash(hash, error ? "?" : std::to_string(stamp.time_since_epoch().count()));
}
std::string Lower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}
std::string Hex64(uint64_t hash) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (int i = 15; i >= 0; --i, hash >>= 4) text[static_cast<size_t>(i)] = digits[hash & 15];
    return text;
}
void Check(HRESULT result, const char* call) {
    if (FAILED(result)) throw IoError(std::string(call) + " failed with HRESULT " + std::to_string(static_cast<unsigned long>(result)));
}
void Check(VkResult result, const char* call) {
    if (result != VK_SUCCESS) throw VulkanError(call, result);
}
ComPtr<IWICImagingFactory> Factory() {
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "CoCreateInstance(WICImagingFactory)");
    return factory;
}
uint32_t MemoryType(VkPhysicalDevice physical, uint32_t type_bits, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((type_bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & properties) == properties) return i;
    throw VulkanError("vkAllocateMemory (no suitable memory type)", VK_ERROR_OUT_OF_DEVICE_MEMORY);
}
}

fs::path LegacyThumbnailFolder(const fs::path& store_root) {
    return store_root.empty() ? fs::path() : store_root / L"thumbnails";
}

fs::path LegacyThumbnailPath(const fs::path& folder, const LibraryModel& model, const fs::path& game) {
    uint64_t hash = 0xcbf29ce484222325ull;
    Hash(hash, kLegacyCacheVersion);
    Hash(hash, std::to_string(kThumbnailSize));
    HashFile(hash, model.Archive);
    Hash(hash, Lower(model.Model));
    // Base game materials show through in the render, so a game update renders the thumbnail again.
    if (game.empty()) Hash(hash, "");
    else HashFile(hash, game);
    return folder / (Hex64(hash) + ".png");
}

std::string ThumbnailKey(const std::optional<std::string>& content_id, const LibraryModel& model, const fs::path& game) {
    uint64_t hash = 0xcbf29ce484222325ull;
    Hash(hash, kCacheVersion);
    Hash(hash, std::to_string(kThumbnailSize));
    // Stored mods are identified by content, so their thumbnails survive moving or sharing the library.
    if (content_id) Hash(hash, *content_id);
    else HashFile(hash, model.Archive);
    Hash(hash, Lower(model.Model));
    if (game.empty()) Hash(hash, "");
    else HashFile(hash, game);
    return Hex64(hash);
}

std::optional<ThumbnailPixels> DecodeThumbnail(const std::vector<uint8_t>& png) {
    if (png.empty()) return std::nullopt;
    try {
        const auto factory = Factory();
        ComPtr<IWICStream> stream;
        Check(factory->CreateStream(&stream), "CreateStream");
        Check(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()), static_cast<DWORD>(png.size())), "InitializeFromMemory");
        ComPtr<IWICBitmapDecoder> decoder;
        Check(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder), "CreateDecoderFromStream");
        ComPtr<IWICBitmapFrameDecode> frame;
        Check(decoder->GetFrame(0, &frame), "GetFrame");
        ComPtr<IWICFormatConverter> converter;
        Check(factory->CreateFormatConverter(&converter), "CreateFormatConverter");
        Check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "IWICFormatConverter::Initialize");
        ThumbnailPixels pixels;
        Check(converter->GetSize(&pixels.Width, &pixels.Height), "GetSize");
        if (pixels.Width == 0 || pixels.Height == 0 || pixels.Width > 4096 || pixels.Height > 4096) return std::nullopt;
        pixels.Rgba.resize(static_cast<size_t>(pixels.Width) * pixels.Height * 4);
        Check(converter->CopyPixels(nullptr, pixels.Width * 4, static_cast<UINT>(pixels.Rgba.size()), pixels.Rgba.data()), "CopyPixels");
        return pixels;
    } catch (const std::exception&) {
        // A damaged image is rendered again.
        return std::nullopt;
    }
}

std::optional<ThumbnailPixels> LoadThumbnailImage(const fs::path& path) {
    std::error_code error;
    if (!fs::is_regular_file(path, error)) return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    return DecodeThumbnail(std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {}));
}

std::vector<uint8_t> EncodeThumbnail(const ThumbnailPixels& pixels) {
    const auto factory = Factory();
    ComPtr<IWICBitmap> bitmap;
    Check(factory->CreateBitmapFromMemory(pixels.Width, pixels.Height, GUID_WICPixelFormat32bppRGBA, pixels.Width * 4,
        static_cast<UINT>(pixels.Rgba.size()), const_cast<BYTE*>(pixels.Rgba.data()), &bitmap), "CreateBitmapFromMemory");
    ComPtr<IStream> stream;
    Check(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "CreateStreamOnHGlobal");
    ComPtr<IWICBitmapEncoder> encoder;
    Check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "CreateEncoder");
    Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "IWICBitmapEncoder::Initialize");
    ComPtr<IWICBitmapFrameEncode> frame;
    Check(encoder->CreateNewFrame(&frame, nullptr), "CreateNewFrame");
    Check(frame->Initialize(nullptr), "IWICBitmapFrameEncode::Initialize");
    Check(frame->SetSize(pixels.Width, pixels.Height), "SetSize");
    // Renders are opaque, so the alpha channel is dropped to keep images small.
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    Check(frame->SetPixelFormat(&format), "SetPixelFormat");
    Check(frame->WriteSource(bitmap.Get(), nullptr), "WriteSource");
    Check(frame->Commit(), "IWICBitmapFrameEncode::Commit");
    Check(encoder->Commit(), "IWICBitmapEncoder::Commit");
    STATSTG stat{};
    Check(stream->Stat(&stat, STATFLAG_NONAME), "IStream::Stat");
    std::vector<uint8_t> png(static_cast<size_t>(stat.cbSize.QuadPart));
    Check(stream->Seek({}, STREAM_SEEK_SET, nullptr), "IStream::Seek");
    ULONG read = 0;
    Check(stream->Read(png.data(), static_cast<ULONG>(png.size()), &read), "IStream::Read");
    png.resize(read);
    return png;
}

ThumbnailImage::ThumbnailImage(const VulkanDevice& device, const ThumbnailPixels& pixels) : device_(device) {
    const VkDevice vk = device_.Device;
    const VkDeviceSize size = pixels.Rgba.size();
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    const auto cleanup = [&] {
        vkDestroyFence(vk, fence, nullptr);
        vkDestroyCommandPool(vk, pool, nullptr);
        vkDestroyBuffer(vk, staging, nullptr);
        vkFreeMemory(vk, staging_memory, nullptr);
    };
    try {
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = VK_FORMAT_R8G8B8A8_UNORM;
        image.extent = {pixels.Width, pixels.Height, 1};
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        Check(vkCreateImage(vk, &image, nullptr, &image_), "vkCreateImage");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(vk, image_, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = MemoryType(device_.Physical, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(vkAllocateMemory(vk, &allocate, nullptr, &memory_), "vkAllocateMemory");
        Check(vkBindImageMemory(vk, image_, memory_, 0), "vkBindImageMemory");

        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image_;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = image.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        Check(vkCreateImageView(vk, &view, nullptr, &view_), "vkCreateImageView");

        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = VK_FILTER_LINEAR;
        sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = 0;
        Check(vkCreateSampler(vk, &sampler, nullptr, &sampler_), "vkCreateSampler");

        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = size;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(vkCreateBuffer(vk, &buffer, nullptr, &staging), "vkCreateBuffer");
        vkGetBufferMemoryRequirements(vk, staging, &requirements);
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = MemoryType(device_.Physical, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        Check(vkAllocateMemory(vk, &allocate, nullptr, &staging_memory), "vkAllocateMemory");
        Check(vkBindBufferMemory(vk, staging, staging_memory, 0), "vkBindBufferMemory");
        void* mapped = nullptr;
        Check(vkMapMemory(vk, staging_memory, 0, size, 0, &mapped), "vkMapMemory");
        std::memcpy(mapped, pixels.Rgba.data(), pixels.Rgba.size());
        vkUnmapMemory(vk, staging_memory);

        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool_info.queueFamilyIndex = device_.QueueFamily;
        Check(vkCreateCommandPool(vk, &pool_info, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command_info.commandPool = pool;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 1;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        Check(vkAllocateCommandBuffers(vk, &command_info, &command_buffer), "vkAllocateCommandBuffers");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(vkBeginCommandBuffer(command_buffer, &begin), "vkBeginCommandBuffer");
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image_;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(command_buffer, &dependency);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {pixels.Width, pixels.Height, 1};
        vkCmdCopyBufferToImage(command_buffer, staging, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(command_buffer, &dependency);
        Check(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer");

        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        Check(vkCreateFence(vk, &fence_info, nullptr, &fence), "vkCreateFence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffer;
        Check(vkQueueSubmit(device_.Queue, 1, &submit, fence), "vkQueueSubmit");
        Check(vkWaitForFences(vk, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        cleanup();
        set_ = ImGui_ImplVulkan_AddTexture(sampler_, view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    } catch (...) {
        cleanup();
        Release();
        throw;
    }
}

ThumbnailImage::ThumbnailImage(const VulkanDevice& device, const TextureImage& texture) : device_(device) {
    if (texture.Mips.empty() || texture.Width == 0 || texture.Height == 0) throw InvalidData("Texture has no pixels.");
    std::optional<VkFormat> format;
    switch (texture.Format) {
    // ImGui writes the sample straight to a UNORM target. sRGB block formats would linearize the
    // texels first, so the card is much darker than the texture. UNORM block views keep the encoding.
    case TextureFormat::BC1: format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; break;
    case TextureFormat::BC3: format = VK_FORMAT_BC3_UNORM_BLOCK; break;
    case TextureFormat::BC4: format = VK_FORMAT_BC4_UNORM_BLOCK; break;
    case TextureFormat::BC5: format = VK_FORMAT_BC5_UNORM_BLOCK; break;
    case TextureFormat::BC7: format = VK_FORMAT_BC7_UNORM_BLOCK; break;
    case TextureFormat::RGBA8: format = VK_FORMAT_R8G8B8A8_UNORM; break;
    case TextureFormat::BGRA8: format = VK_FORMAT_B8G8R8A8_UNORM; break;
    }
    if (!format) throw InvalidData("Texture format is not supported.");
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(device_.Physical, *format, &properties);
    const VkFormatFeatureFlags features = properties.optimalTilingFeatures;
    if ((features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0 || (features & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) == 0)
        throw InvalidData("This GPU cannot display that texture.");
    const std::vector<uint8_t>& pixels = texture.Mips.front();
    const VkDevice vk = device_.Device;
    const VkDeviceSize size = pixels.size();
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    const auto cleanup = [&] {
        vkDestroyFence(vk, fence, nullptr);
        vkDestroyCommandPool(vk, pool, nullptr);
        vkDestroyBuffer(vk, staging, nullptr);
        vkFreeMemory(vk, staging_memory, nullptr);
    };
    try {
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = *format;
        image.extent = {texture.Width, texture.Height, 1};
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        Check(vkCreateImage(vk, &image, nullptr, &image_), "vkCreateImage");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(vk, image_, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = MemoryType(device_.Physical, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(vkAllocateMemory(vk, &allocate, nullptr, &memory_), "vkAllocateMemory");
        Check(vkBindImageMemory(vk, image_, memory_, 0), "vkBindImageMemory");

        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image_;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = image.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        // Color maps often store a usable picture with alpha 0, because the mesh shader reads rgb only.
        // ImGui multiplies by alpha, which made those cards invisible. BC4 is one channel and samples as red.
        view.components.a = VK_COMPONENT_SWIZZLE_ONE;
        if (image.format == VK_FORMAT_BC4_UNORM_BLOCK) {
            view.components.r = VK_COMPONENT_SWIZZLE_R;
            view.components.g = VK_COMPONENT_SWIZZLE_R;
            view.components.b = VK_COMPONENT_SWIZZLE_R;
        }
        Check(vkCreateImageView(vk, &view, nullptr, &view_), "vkCreateImageView");

        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = VK_FILTER_LINEAR;
        sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = 0;
        Check(vkCreateSampler(vk, &sampler, nullptr, &sampler_), "vkCreateSampler");

        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = size;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(vkCreateBuffer(vk, &buffer, nullptr, &staging), "vkCreateBuffer");
        vkGetBufferMemoryRequirements(vk, staging, &requirements);
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = MemoryType(device_.Physical, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        Check(vkAllocateMemory(vk, &allocate, nullptr, &staging_memory), "vkAllocateMemory");
        Check(vkBindBufferMemory(vk, staging, staging_memory, 0), "vkBindBufferMemory");
        void* mapped = nullptr;
        Check(vkMapMemory(vk, staging_memory, 0, size, 0, &mapped), "vkMapMemory");
        std::memcpy(mapped, pixels.data(), pixels.size());
        vkUnmapMemory(vk, staging_memory);

        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool_info.queueFamilyIndex = device_.QueueFamily;
        Check(vkCreateCommandPool(vk, &pool_info, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command_info.commandPool = pool;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 1;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        Check(vkAllocateCommandBuffers(vk, &command_info, &command_buffer), "vkAllocateCommandBuffers");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(vkBeginCommandBuffer(command_buffer, &begin), "vkBeginCommandBuffer");
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image_;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(command_buffer, &dependency);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {texture.Width, texture.Height, 1};
        vkCmdCopyBufferToImage(command_buffer, staging, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(command_buffer, &dependency);
        Check(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer");

        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        Check(vkCreateFence(vk, &fence_info, nullptr, &fence), "vkCreateFence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffer;
        Check(vkQueueSubmit(device_.Queue, 1, &submit, fence), "vkQueueSubmit");
        Check(vkWaitForFences(vk, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        cleanup();
        set_ = ImGui_ImplVulkan_AddTexture(sampler_, view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    } catch (...) {
        cleanup();
        Release();
        throw;
    }
}

ThumbnailImage::~ThumbnailImage() { Release(); }

void ThumbnailImage::Release() {
    if (set_ != VK_NULL_HANDLE) ImGui_ImplVulkan_RemoveTexture(set_);
    vkDestroySampler(device_.Device, sampler_, nullptr);
    vkDestroyImageView(device_.Device, view_, nullptr);
    vkDestroyImage(device_.Device, image_, nullptr);
    vkFreeMemory(device_.Device, memory_, nullptr);
    set_ = VK_NULL_HANDLE; sampler_ = VK_NULL_HANDLE; view_ = VK_NULL_HANDLE; image_ = VK_NULL_HANDLE; memory_ = VK_NULL_HANDLE;
}
}
