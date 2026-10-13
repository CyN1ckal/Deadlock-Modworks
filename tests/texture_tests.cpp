#include "error.hpp"
#include "kv3.hpp"
#include "texture.hpp"

#include <gtest/gtest.h>
#include <lz4.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

using skeleton_repair::DecodeMaterial;
using skeleton_repair::DecodeTexture;
using skeleton_repair::InvalidData;
using skeleton_repair::KvDocument;
using skeleton_repair::KvValue;
using skeleton_repair::TextureFormat;

using Fields = std::vector<std::pair<std::string, KvValue>>;

template <typename T>
void Put(std::vector<uint8_t>& bytes, size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

// Version 12 resource whose only block is DATA, followed by `trailer` (the texture's pixel data).
std::vector<uint8_t> Resource(const std::vector<uint8_t>& data, const std::vector<uint8_t>& trailer = {}) {
    std::vector<uint8_t> file(28);
    Put<uint16_t>(file, 4, 12);
    Put<uint32_t>(file, 8, 8);
    Put<int32_t>(file, 12, 1);
    std::memcpy(file.data() + 16, "DATA", 4);
    Put<uint32_t>(file, 20, 8);
    Put<uint32_t>(file, 24, static_cast<uint32_t>(data.size()));
    file.insert(file.end(), data.begin(), data.end());
    file.insert(file.end(), trailer.begin(), trailer.end());
    Put<uint32_t>(file, 0, static_cast<uint32_t>(file.size()));
    return file;
}

struct Mip {
    std::vector<uint8_t> Raw;
    bool Lz4 = false;
};

// VTEX header for a 2D texture. Mips are given largest first and stored smallest first.
std::vector<uint8_t> Texture(uint8_t format, uint16_t width, uint16_t height, const std::vector<Mip>& mips, uint16_t flags = 0) {
    const bool any_lz4 = std::any_of(mips.begin(), mips.end(), [](const Mip& mip) { return mip.Lz4; });
    std::vector<std::vector<uint8_t>> stored;
    for (const Mip& mip : mips) {
        if (!mip.Lz4) {
            stored.push_back(mip.Raw);
            continue;
        }
        std::vector<uint8_t> packed(static_cast<size_t>(LZ4_compressBound(static_cast<int>(mip.Raw.size()))));
        packed.resize(static_cast<size_t>(LZ4_compress_default(reinterpret_cast<const char*>(mip.Raw.data()),
            reinterpret_cast<char*>(packed.data()), static_cast<int>(mip.Raw.size()), static_cast<int>(packed.size()))));
        stored.push_back(packed);
    }

    std::vector<uint8_t> header(40);
    Put<uint16_t>(header, 0, 1);
    Put<uint16_t>(header, 2, flags);
    Put<uint16_t>(header, 20, width);
    Put<uint16_t>(header, 22, height);
    Put<uint16_t>(header, 24, 1);
    Put<uint8_t>(header, 26, format);
    Put<uint8_t>(header, 27, static_cast<uint8_t>(mips.size()));
    Put<uint32_t>(header, 32, 8);
    Put<uint32_t>(header, 36, any_lz4 ? 1u : 0u);
    if (any_lz4) {
        // One COMPRESSED_MIP_SIZE entry at 40, its data at 52: {flag, offset to sizes, count, sizes...}.
        header.resize(64 + mips.size() * 4);
        Put<uint32_t>(header, 40, 4);
        Put<uint32_t>(header, 44, 52 - 44);
        Put<uint32_t>(header, 48, static_cast<uint32_t>(12 + mips.size() * 4));
        Put<uint32_t>(header, 52, 1);
        Put<uint32_t>(header, 56, 64 - 56);
        Put<uint32_t>(header, 60, static_cast<uint32_t>(mips.size()));
        for (size_t level = 0; level < mips.size(); ++level) {
            Put<int32_t>(header, 64 + level * 4, static_cast<int32_t>(stored[level].size()));
        }
    }

    std::vector<uint8_t> pixels;
    for (size_t level = mips.size(); level-- > 0;) {
        pixels.insert(pixels.end(), stored[level].begin(), stored[level].end());
    }
    return Resource(header, pixels);
}

std::vector<uint8_t> Filled(size_t size, uint8_t value) {
    return std::vector<uint8_t>(size, value);
}

constexpr uint8_t kRgba8888 = 4;
constexpr uint8_t kBc7 = 20;
constexpr uint8_t kDxt1 = 1;

TEST(Texture, LevelSizesPadBlockFormats) {
    EXPECT_EQ(skeleton_repair::TextureLevelSize(TextureFormat::BC1, 1, 1), 8u);
    EXPECT_EQ(skeleton_repair::TextureLevelSize(TextureFormat::BC7, 6, 6), 64u);
    EXPECT_EQ(skeleton_repair::TextureLevelSize(TextureFormat::RGBA8, 3, 2), 24u);
}

TEST(Texture, ReadsMipsLargestFirst) {
    const auto file = Texture(kRgba8888, 4, 2, {{Filled(32, 1)}, {Filled(8, 2)}, {Filled(4, 3)}});
    const auto image = DecodeTexture(file);
    EXPECT_EQ(image.Format, TextureFormat::RGBA8);
    EXPECT_EQ(image.Width, 4u);
    EXPECT_EQ(image.Height, 2u);
    ASSERT_EQ(image.Mips.size(), 3u);
    EXPECT_EQ(image.Mips[0], Filled(32, 1));
    EXPECT_EQ(image.Mips[1], Filled(8, 2));
    EXPECT_EQ(image.Mips[2], Filled(4, 3));
}

TEST(Texture, DecompressesLz4Mips) {
    // A uniform 8x8 BC7 level compresses well; the 4x4 level stays raw.
    const auto file = Texture(kBc7, 8, 8, {{Filled(64, 7), true}, {Filled(16, 9), false}});
    const auto image = DecodeTexture(file);
    EXPECT_EQ(image.Format, TextureFormat::BC7);
    ASSERT_EQ(image.Mips.size(), 2u);
    EXPECT_EQ(image.Mips[0], Filled(64, 7));
    EXPECT_EQ(image.Mips[1], Filled(16, 9));
}

TEST(Texture, SkipsLevelsLargerThanTheLimit) {
    const auto file = Texture(kDxt1, 16, 16, {{Filled(128, 1)}, {Filled(32, 2)}, {Filled(8, 3)}});
    const auto image = DecodeTexture(file, 8);
    EXPECT_EQ(image.Width, 8u);
    EXPECT_EQ(image.Height, 8u);
    ASSERT_EQ(image.Mips.size(), 2u);
    EXPECT_EQ(image.Mips[0], Filled(32, 2));
}

TEST(Texture, ReportsClampFlags) {
    const auto image = DecodeTexture(Texture(kRgba8888, 1, 1, {{Filled(4, 0)}}, 0x2));
    EXPECT_FALSE(image.ClampU);
    EXPECT_TRUE(image.ClampV);
}

TEST(Texture, RejectsCubeMapsAndUnknownFormats) {
    EXPECT_THROW(DecodeTexture(Texture(kRgba8888, 1, 1, {{Filled(4, 0)}}, 0x10)), InvalidData);
    EXPECT_THROW(DecodeTexture(Texture(15, 1, 1, {{Filled(4, 0)}})), InvalidData);  // JPEG bytes that are not a JPEG
    EXPECT_THROW(DecodeTexture(Texture(99, 1, 1, {{Filled(4, 0)}})), InvalidData);
}

TEST(Texture, DecodesAnEmbeddedPngToItsPixels) {
    // 1x1 opaque red PNG, compiled as VTexFormat PNG_RGBA8888.
    const std::vector<uint8_t> png = {137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 1, 0, 0, 0, 1, 8, 6, 0, 0, 0, 31, 21, 196,
        137, 0, 0, 0, 1, 115, 82, 71, 66, 0, 174, 206, 28, 233, 0, 0, 0, 4, 103, 65, 77, 65, 0, 0, 177, 143, 11, 252, 97, 5, 0, 0, 0, 9, 112, 72, 89, 115,
        0, 0, 14, 195, 0, 0, 14, 195, 1, 199, 111, 168, 100, 0, 0, 0, 13, 73, 68, 65, 84, 24, 87, 99, 248, 207, 192, 240, 31, 0, 5, 0, 1, 255, 166, 92, 155,
        93, 0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};
    const auto image = DecodeTexture(Texture(16, 1, 1, {{png}}));
    EXPECT_EQ(image.Format, TextureFormat::RGBA8);
    EXPECT_EQ(image.Width, 1u);
    EXPECT_EQ(image.Height, 1u);
    ASSERT_EQ(image.Mips.size(), 1u);
    ASSERT_EQ(image.Mips[0].size(), 4u);
    EXPECT_EQ(image.Mips[0], (std::vector<uint8_t>{255, 0, 0, 255}));
    EXPECT_THROW(DecodeTexture(Texture(16, 1, 1, {{png}}, 0x10)), InvalidData);
}

TEST(Texture, RejectsTruncatedPixels) {
    auto file = Texture(kRgba8888, 4, 4, {{Filled(64, 1)}});
    file.resize(file.size() - 10);
    EXPECT_THROW(DecodeTexture(file), InvalidData);
}

KvValue Param(const char* name, KvValue value, const char* key) {
    return KvValue::Object(Fields{{"m_name", KvValue::String(name)}, {key, std::move(value)}});
}

std::vector<uint8_t> MaterialFile(std::vector<KvValue> textures, std::vector<KvValue> ints) {
    KvDocument document;
    document.Root = KvValue::Object(Fields{
        {"m_materialName", KvValue::String("models/test/body.vmat")},
        {"m_shaderName", KvValue::String("pbr.vfx")},
        {"m_intParams", KvValue::Array(std::move(ints))},
        {"m_textureParams", KvValue::Array(std::move(textures))},
    });
    return Resource(skeleton_repair::WriteKv3(document));
}

TEST(Material, FindsTheColorTextureAndFlags) {
    const auto material = DecodeMaterial(MaterialFile(
        {Param("g_tNormalRoughness", KvValue::String("models/test/body_normal.vtex"), "m_pValue"),
            Param("g_tColor", KvValue::String("models/test/body_color.vtex"), "m_pValue")},
        {Param("F_ALPHA_TEST", KvValue::Int32(1), "m_nValue")}));
    EXPECT_EQ(material.Name, "models/test/body.vmat");
    EXPECT_EQ(material.Shader, "pbr.vfx");
    EXPECT_EQ(material.ColorTexture(), std::optional<std::string>("models/test/body_color.vtex"));
    EXPECT_EQ(material.IntParam("F_ALPHA_TEST"), 1);
    EXPECT_EQ(material.IntParam("F_TRANSLUCENT"), 0);
}

TEST(Material, FallsBackToLayeredColor) {
    const auto material = DecodeMaterial(MaterialFile({Param("g_tColor1", KvValue::String("models/test/layer1.vtex"), "m_pValue")}, {}));
    EXPECT_EQ(material.ColorTexture(), std::optional<std::string>("models/test/layer1.vtex"));
    EXPECT_FALSE(DecodeMaterial(MaterialFile({}, {})).ColorTexture());
}

}  // namespace
