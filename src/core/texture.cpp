#include "texture.hpp"

#include "error.hpp"
#include "kv3.hpp"
#include "resource.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <lz4.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace skeleton_repair {
namespace {

// VTexFormat values.
enum VTexFormat : uint8_t {
    DXT1 = 1,
    DXT5 = 2,
    RGBA8888 = 4,
    JPEG_RGBA8888 = 15,
    PNG_RGBA8888 = 16,
    JPEG_DXT5 = 17,
    PNG_DXT5 = 18,
    BC7 = 20,
    ATI2N = 21,
    ATI1N = 27,
    BGRA8888 = 28,
    WEBP_RGBA8888 = 29,
    WEBP_DXT5 = 30,
};

enum class ImageFile { Png, Jpeg, Webp };

// VTexFlags bits that make a texture something other than a plain 2D image.
constexpr uint16_t kClampS = 0x1;
constexpr uint16_t kClampT = 0x2;
constexpr uint16_t kNot2D = 0x10 | 0x20 | 0x40;  // cube, volume, array

constexpr uint32_t kCompressedMipSize = 4;  // VTexExtraData::COMPRESSED_MIP_SIZE

template <typename T>
T Read(std::span<const uint8_t> bytes, size_t offset) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        throw InvalidData("Texture header is truncated.");
    }
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

std::optional<ImageFile> ImageFileFor(uint8_t format) {
    switch (format) {
    case PNG_RGBA8888:
    case PNG_DXT5: return ImageFile::Png;
    case JPEG_RGBA8888:
    case JPEG_DXT5: return ImageFile::Jpeg;
    case WEBP_RGBA8888:
    case WEBP_DXT5: return ImageFile::Webp;
    default: return std::nullopt;
    }
}

uint32_t ReadBigEndian(std::span<const uint8_t> bytes, size_t offset) {
    if (offset > bytes.size() || 4 > bytes.size() - offset) {
        throw InvalidData("Texture image data is truncated.");
    }
    return (static_cast<uint32_t>(bytes[offset]) << 24) | (static_cast<uint32_t>(bytes[offset + 1]) << 16)
        | (static_cast<uint32_t>(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

// PNG bytes are one image at the start of the payload. Chunks run until IEND.
size_t PngSize(std::span<const uint8_t> bytes) {
    const uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (bytes.size() < sizeof(signature) || std::memcmp(bytes.data(), signature, sizeof(signature)) != 0) {
        throw InvalidData("Texture PNG data is invalid.");
    }
    size_t cursor = sizeof(signature);
    for (;;) {
        const uint32_t length = ReadBigEndian(bytes, cursor);
        const uint32_t type = ReadBigEndian(bytes, cursor + 4);
        if (bytes.size() - cursor < 12 || length > bytes.size() - cursor - 12) {
            throw InvalidData("Texture PNG data is truncated.");
        }
        cursor += static_cast<size_t>(length) + 12;
        if (type == 0x49454E44u) {
            return cursor;
        }
    }
}

size_t WebpSize(std::span<const uint8_t> bytes) {
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WEBP", 4) != 0) {
        throw InvalidData("Texture WebP data is invalid.");
    }
    const uint32_t payload = Read<uint32_t>(bytes, 4);
    const uint64_t total = 8ull + payload;
    if (total > bytes.size()) {
        throw InvalidData("Texture WebP data is truncated.");
    }
    return static_cast<size_t>(total);
}

// Keeps an STA thread's apartment when this thread already has one.
struct ComApartment {
    bool release = false;
    ComApartment() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (result == RPC_E_CHANGED_MODE) {
            return;
        }
        if (FAILED(result)) {
            throw InvalidData("Could not start image decoding.");
        }
        release = true;
    }
    ~ComApartment() {
        if (release) {
            CoUninitialize();
        }
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
};

TextureImage DecodeImageFile(uint16_t flags, std::span<const uint8_t> encoded, uint32_t max_size) {
    if (encoded.empty() || encoded.size() > static_cast<size_t>(std::numeric_limits<DWORD>::max())) {
        throw InvalidData("Texture image data is invalid.");
    }
    const ComApartment apartment;
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        throw InvalidData("Could not start image decoding.");
    }
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))
        || FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(encoded.data()), static_cast<DWORD>(encoded.size())))) {
        throw InvalidData("Texture image data is invalid.");
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) {
        throw InvalidData("Texture image could not be decoded.");
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        throw InvalidData("Texture image could not be decoded.");
    }
    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) {
        throw InvalidData("Texture has no pixels.");
    }
    IWICBitmapSource* source = frame.Get();
    ComPtr<IWICBitmapScaler> scaler;
    if (max_size > 0 && (width > max_size || height > max_size)) {
        const double scale = static_cast<double>(max_size) / static_cast<double>(std::max(width, height));
        width = std::max(1u, static_cast<UINT>(std::lround(static_cast<double>(width) * scale)));
        height = std::max(1u, static_cast<UINT>(std::lround(static_cast<double>(height) * scale)));
        if (FAILED(factory->CreateBitmapScaler(&scaler))
            || FAILED(scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeFant))) {
            throw InvalidData("Texture image could not be decoded.");
        }
        source = scaler.Get();
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))
        || FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        throw InvalidData("Texture image could not be decoded.");
    }
    const uint64_t bytes = static_cast<uint64_t>(width) * height * 4;
    if (bytes > static_cast<uint64_t>(std::numeric_limits<UINT>::max())) {
        throw InvalidData("Texture image is too large.");
    }
    TextureImage image;
    image.Format = TextureFormat::RGBA8;
    image.Width = width;
    image.Height = height;
    image.ClampU = (flags & kClampS) != 0;
    image.ClampV = (flags & kClampT) != 0;
    image.Mips.emplace_back(static_cast<size_t>(bytes));
    if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(bytes), image.Mips.front().data()))) {
        throw InvalidData("Texture image could not be decoded.");
    }
    return image;
}

std::optional<TextureFormat> MapFormat(uint8_t format) {
    switch (format) {
    case DXT1: return TextureFormat::BC1;
    case DXT5: return TextureFormat::BC3;
    case ATI1N: return TextureFormat::BC4;
    case ATI2N: return TextureFormat::BC5;
    case BC7: return TextureFormat::BC7;
    case RGBA8888: return TextureFormat::RGBA8;
    case BGRA8888: return TextureFormat::BGRA8;
    default: return std::nullopt;
    }
}

bool IsBlockFormat(TextureFormat format) {
    return format != TextureFormat::RGBA8 && format != TextureFormat::BGRA8;
}

uint32_t LevelExtent(uint32_t size, uint32_t level) {
    return std::max(size >> level, 1u);
}

const KvValue* Field(const KvValue& object, const char* key) {
    return object.IsObject() ? object.Find(key) : nullptr;
}

}  // namespace

size_t TextureLevelSize(TextureFormat format, uint32_t width, uint32_t height) {
    if (!IsBlockFormat(format)) {
        return static_cast<size_t>(width) * height * 4;
    }
    const size_t blocks = static_cast<size_t>((width + 3) / 4) * ((height + 3) / 4);
    const bool eight_byte = format == TextureFormat::BC1 || format == TextureFormat::BC4;
    return blocks * (eight_byte ? 8 : 16);
}

TextureImage DecodeTexture(std::span<const uint8_t> file, uint32_t max_size) {
    const std::span<const uint8_t> header = FindDataBlock(file);
    if (Read<uint16_t>(header, 0) != 1) {
        throw InvalidData("Texture header is not version 1.");
    }
    const uint16_t flags = Read<uint16_t>(header, 2);
    const uint32_t width = Read<uint16_t>(header, 20);
    const uint32_t height = Read<uint16_t>(header, 22);
    const uint32_t depth = Read<uint16_t>(header, 24);
    const uint8_t raw_format = Read<uint8_t>(header, 26);
    const uint32_t mip_count = std::max<uint32_t>(Read<uint8_t>(header, 27), 1);
    if ((flags & kNot2D) != 0 || depth > 1) {
        throw InvalidData("Only 2D textures are supported.");
    }
    if (width == 0 || height == 0) {
        throw InvalidData("Texture has no pixels.");
    }
    if (const std::optional<ImageFile> image_file = ImageFileFor(raw_format)) {
        const size_t payload = static_cast<size_t>(header.data() + header.size() - file.data());
        if (payload > file.size()) {
            throw InvalidData("Texture image data is truncated.");
        }
        const std::span<const uint8_t> encoded = file.subspan(payload);
        const std::span<const uint8_t> bytes = *image_file == ImageFile::Png ? encoded.first(PngSize(encoded))
            : *image_file == ImageFile::Webp ? encoded.first(WebpSize(encoded))
                                             : encoded;
        return DecodeImageFile(flags, bytes, max_size);
    }
    const std::optional<TextureFormat> format = MapFormat(raw_format);
    if (!format) {
        throw InvalidData("Texture format " + std::to_string(raw_format) + " is not supported.");
    }

    // Extra data: 12-byte entries {type, offset from its own field, size}.
    std::vector<int32_t> compressed_sizes;
    bool lz4_mips = false;
    const size_t extra_field = 32;
    const uint32_t extra_offset = Read<uint32_t>(header, extra_field);
    const uint32_t extra_count = Read<uint32_t>(header, extra_field + 4);
    for (uint32_t i = 0; i < extra_count; ++i) {
        const size_t entry = extra_field + extra_offset + static_cast<size_t>(i) * 12;
        const uint32_t type = Read<uint32_t>(header, entry);
        const size_t data = entry + 4 + Read<uint32_t>(header, entry + 4);
        if (type != kCompressedMipSize) {
            continue;
        }
        const uint32_t compressed = Read<uint32_t>(header, data);
        const size_t sizes = data + 4 + Read<uint32_t>(header, data + 4);
        const uint32_t count = Read<uint32_t>(header, data + 8);
        if (compressed > 1 || count > 32) {
            throw InvalidData("Texture compressed-mip table is invalid.");
        }
        lz4_mips = compressed == 1;
        compressed_sizes.resize(count);
        for (uint32_t level = 0; level < count; ++level) {
            compressed_sizes[level] = Read<int32_t>(header, sizes + static_cast<size_t>(level) * 4);
        }
    }

    // Choose the first level that fits, keeping at least the smallest level.
    uint32_t base = 0;
    while (base + 1 < mip_count && (LevelExtent(width, base) > max_size || LevelExtent(height, base) > max_size)) {
        ++base;
    }

    TextureImage image;
    image.Format = *format;
    image.Width = LevelExtent(width, base);
    image.Height = LevelExtent(height, base);
    image.ClampU = (flags & kClampS) != 0;
    image.ClampV = (flags & kClampT) != 0;
    image.Mips.resize(mip_count - base);

    // Pixel data follows the DATA block, smallest mip first.
    size_t cursor = static_cast<size_t>(header.data() + header.size() - file.data());
    for (uint32_t level = mip_count; level-- > 0;) {
        const size_t raw_size = TextureLevelSize(*format, LevelExtent(width, level), LevelExtent(height, level));
        size_t stored = raw_size;
        if (level < compressed_sizes.size()) {
            if (compressed_sizes[level] < 0) {
                throw InvalidData("Texture compressed-mip size is negative.");
            }
            stored = std::min(stored, static_cast<size_t>(compressed_sizes[level]));
        }
        if (cursor > file.size() || stored > file.size() - cursor) {
            throw InvalidData("Texture mip " + std::to_string(level) + " is outside the file.");
        }
        if (level >= base) {
            std::vector<uint8_t>& out = image.Mips[level - base];
            out.resize(raw_size);
            if (lz4_mips && stored < raw_size) {
                if (stored > static_cast<size_t>(std::numeric_limits<int>::max()) || raw_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
                    throw InvalidData("Texture mip is too large.");
                }
                const int written = LZ4_decompress_safe(reinterpret_cast<const char*>(file.data() + cursor), reinterpret_cast<char*>(out.data()),
                    static_cast<int>(stored), static_cast<int>(raw_size));
                if (written != static_cast<int>(raw_size)) {
                    throw InvalidData("Texture mip " + std::to_string(level) + " LZ4 data is invalid.");
                }
            } else if (stored == raw_size) {
                std::memcpy(out.data(), file.data() + cursor, raw_size);
            } else {
                throw InvalidData("Texture mip " + std::to_string(level) + " is shorter than its size.");
            }
        }
        cursor += stored;
    }
    return image;
}

std::optional<std::string> MaterialInfo::ColorTexture() const {
    for (const auto& [name, value] : Textures) {
        if (name == "g_tColor" && !value.empty()) {
            return value;
        }
    }
    for (const auto& [name, value] : Textures) {
        if (name.rfind("g_tColor", 0) == 0 && !value.empty()) {
            return value;
        }
    }
    return std::nullopt;
}

int32_t MaterialInfo::IntParam(const std::string& name, int32_t fallback) const {
    for (const auto& [key, value] : IntParams) {
        if (key == name) {
            return value;
        }
    }
    return fallback;
}

MaterialInfo DecodeMaterial(std::span<const uint8_t> file) {
    const std::span<const uint8_t> data = FindDataBlock(file);
    if (data.size() < 4 || !IsBinaryKv3(Read<uint32_t>(data, 0))) {
        throw InvalidData("Material DATA is not KV3.");
    }
    const KvValue root = ReadKv3(data).Root;
    MaterialInfo material;
    if (const KvValue* name = Field(root, "m_materialName"); name != nullptr && name->Kind == KvKind::String) {
        material.Name = name->String();
    }
    if (const KvValue* shader = Field(root, "m_shaderName"); shader != nullptr && shader->Kind == KvKind::String) {
        material.Shader = shader->String();
    }
    if (const KvValue* textures = Field(root, "m_textureParams"); textures != nullptr && textures->IsArray()) {
        for (const KvValue& param : textures->Items()) {
            const KvValue* name = Field(param, "m_name");
            const KvValue* value = Field(param, "m_pValue");
            if (name != nullptr && value != nullptr && name->Kind == KvKind::String && value->Kind == KvKind::String) {
                material.Textures.emplace_back(name->String(), value->String());
            }
        }
    }
    if (const KvValue* ints = Field(root, "m_intParams"); ints != nullptr && ints->IsArray()) {
        for (const KvValue& param : ints->Items()) {
            const KvValue* name = Field(param, "m_name");
            const KvValue* value = Field(param, "m_nValue");
            if (name != nullptr && value != nullptr && name->Kind == KvKind::String) {
                material.IntParams.emplace_back(name->String(), value->ToInt32());
            }
        }
    }
    return material;
}

}  // namespace skeleton_repair
