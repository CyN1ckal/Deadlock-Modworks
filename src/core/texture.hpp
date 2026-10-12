#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace skeleton_repair {

// Block-compressed and 8-bit formats a GPU can sample directly.
enum class TextureFormat {
    BC1,
    BC3,
    BC4,
    BC5,
    BC7,
    RGBA8,
    BGRA8,
};

struct TextureImage {
    TextureFormat Format = TextureFormat::RGBA8;
    uint32_t Width = 0;
    uint32_t Height = 0;
    // Largest first. Mips[0] is Width x Height.
    std::vector<std::vector<uint8_t>> Mips;
    bool ClampU = false;
    bool ClampV = false;
};

// Bytes for one mip of the given size, padding block formats to whole 4x4 blocks.
size_t TextureLevelSize(TextureFormat format, uint32_t width, uint32_t height);

// Reads a compiled 2D texture (.vtex_c). Levels larger than `max_size` on either side are skipped,
// so a 4096 texture with max_size 2048 starts at its 2048 mip. Cube, volume and array textures and
// image-file formats (PNG/JPEG/WebP) are rejected.
// Layout pinned to ValveResourceFormat 20.0.6980 Texture.
TextureImage DecodeTexture(std::span<const uint8_t> file, uint32_t max_size = 2048);

struct MaterialInfo {
    std::string Name;
    std::string Shader;
    // m_textureParams as (parameter, texture resource name without _c).
    std::vector<std::pair<std::string, std::string>> Textures;
    std::vector<std::pair<std::string, int32_t>> IntParams;

    // g_tColor, or the first layered color (g_tColor1, g_tColorA, ...).
    std::optional<std::string> ColorTexture() const;
    int32_t IntParam(const std::string& name, int32_t fallback = 0) const;
};

// Reads the parameters of a compiled material (.vmat_c).
MaterialInfo DecodeMaterial(std::span<const uint8_t> file);

}  // namespace skeleton_repair
